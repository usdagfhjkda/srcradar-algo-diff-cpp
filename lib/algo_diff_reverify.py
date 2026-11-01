#!/usr/bin/env python3
"""
algo_diff_reverify.py — T1/T2 reverify 阶段, 完整封装.

调用方 (C++ binary 内部 stage_reverify) 起这个脚本:
   python3 algo_diff_reverify.py <db_path> <phase> [--pdtm-conf PATH]

phase:
   C = T1 C reverify (ct=12, source_ct=12)
       - SELECT rows WHERE ct=12
       - popen httpx → 有响应 → UPDATE 全部字段 + ct=10 (C→A)
                       无响应 → UPDATE curr_body=NULL + ct=13 (C→D)
   D = T2 D reverify (ct=13, source_ct=13)
       - SELECT rows WHERE ct=13
       - popen httpx → 有响应 → UPDATE 全部字段 + ct=10 (D→A)
                       无响应 → UPDATE curr_body=NULL + ct=8 (D→8 终态)

pdtm.conf 读取 (python 自己读, 不需要 C++ 传):
   - httpx_bin        conf 必有, 缺了 exit 2
   - resolvers_file   conf 优先, fallback 到 <repo>/modules/main/pdtm/resolvers,
                      都没就不传 -r

完整字段 UPDATE (跟 import_scan_results.py 一致):
   hash_id        (upsert_web_hash: 找/新建 web_hashes 行, mmh3 作为 response_hash)
   url            httpx input
   status_code    httpx status_code
   content_length httpx content_length
   title          httpx title
   technologies   httpx tech.join(',')
   last_seen      httpx timestamp
   fetched_at     httpx timestamp
   is_active      1 (有响应 = 活)
   raw_json       httpx 完整 jsonl 行
   ips            httpx host_ip
   curr_body      zlib.compress(httpx body, 6)
   content_type   httpx header.content_type

无响应时 UPDATE:
   curr_body      NULL
   change_type    13 (C phase) or 8 (D phase)
   (其他字段保留旧值)

退出码:
   0 = OK
   2 = args / conf / httpx_bin 缺失
   3 = db schema 错 (verify_schema failed)
   4 = httpx subprocess 失败
   5 = batch update 失败
"""
import argparse
import json
import os
import re
import sqlite3
import subprocess
import sys
import tempfile
import time
import zlib
from typing import Any


# ---------------------------------------------------------------------------
# pdtm.conf 解析 (与 modules/main/lib/load_config.py 行为一致)
# ---------------------------------------------------------------------------

KEY_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def read_pdtm_conf(conf_path: str) -> dict[str, str]:
    """解析 pdtm.conf key=value 文件, 跳过 # 注释和空行."""
    if not os.path.isfile(conf_path):
        return {}
    out: dict[str, str] = {}
    with open(conf_path) as f:
        for raw in f:
            if "#" in raw:
                raw = raw.split("#", 1)[0]
            if not raw.strip():
                continue
            if "=" not in raw:
                continue
            key, _, value = raw.partition("=")
            key = key.strip()
            value = value.rstrip()
            if KEY_RE.match(key):
                out[key] = value
    return out


def resolve_httpx_bin(conf: dict[str, str]) -> str:
    """从 conf 找 httpx_bin, 缺了 exit 2."""
    httpx_bin = conf.get("httpx_bin", "").strip()
    if httpx_bin and os.path.isfile(httpx_bin) and os.access(httpx_bin, os.X_OK):
        return httpx_bin
    if httpx_bin:
        print(f"[algo_diff_reverify][warn] conf httpx_bin='{httpx_bin}' 不可执行",
              file=sys.stderr)
    print("[algo_diff_reverify][err] httpx_bin 缺失或不可执行 (在 pdtm.conf)",
          file=sys.stderr)
    sys.exit(2)


def resolve_resolvers_file(conf: dict[str, str], db_path: str) -> str:
    """1: conf resolvers_file  2: <repo>/modules/main/pdtm/resolvers  3: 空."""
    candidate = conf.get("resolvers_file", "").strip()
    if candidate and os.path.isfile(candidate):
        return candidate
    # fallback: <db_path>/../../modules/main/pdtm/resolvers
    db_dir = os.path.dirname(os.path.abspath(db_path))
    fallback = os.path.normpath(
        os.path.join(db_dir, "..", "..", "modules", "main", "pdtm", "resolvers")
    )
    if os.path.isfile(fallback):
        return fallback
    return ""


def default_pdtm_conf(db_path: str) -> str:
    """默认 conf 路径:
       1. $PDTMCONF 环境变量 (lib/run_algo_diff.sh 显式 export)
       2. <db_path>/../../../config/pdtm.conf
          (production: <repo>/modules/main/db/ → <repo>/config/pdtm.conf)
    """
    env = os.environ.get("PDTMCONF", "").strip()
    if env:
        return env
    db_dir = os.path.dirname(os.path.abspath(db_path))
    return os.path.normpath(os.path.join(db_dir, "..", "..", "..", "config", "pdtm.conf"))


# ---------------------------------------------------------------------------
# schema 验证
# ---------------------------------------------------------------------------

REQUIRED_TABLES = {
    "web_subdomains": {
        "id", "business_id", "hash_id", "subdomain", "port",
        "url", "status_code", "content_length", "title", "technologies",
        "first_seen", "last_seen", "fetched_at", "is_active",
        "raw_json", "change_type", "ips",
        "prev_body", "curr_body", "content_type",
        "last_comparison_ratio", "last_comparison_kind", "last_comparison_at",
    },
    "web_hashes": {
        "id", "business_id", "response_hash", "subdomain_count",
        "created_at", "updated_at", "fetched_at",
    },
}


def verify_schema(db_path: str) -> bool:
    """check web_subdomains + web_hashes 表 + 必检列是否存在"""
    conn = sqlite3.connect(db_path)
    try:
        for table, cols in REQUIRED_TABLES.items():
            cur = conn.execute(f"PRAGMA table_info({table})")
            existing = {row[1] for row in cur.fetchall()}
            if not existing:
                print(f"[algo_diff_reverify][err] 缺表: {table}", file=sys.stderr)
                return False
            miss = cols - existing
            if miss:
                print(
                    f"[algo_diff_reverify][err] {table} 缺列: {sorted(miss)}",
                    file=sys.stderr)
                return False
    finally:
        conn.close()
    return True


# ---------------------------------------------------------------------------
# upsert_web_hash (跟 import_scan_results.py:715-730 一致)
# ---------------------------------------------------------------------------

def upsert_web_hash(conn, business_id: int, response_hash: str,
                    fetched_at: str) -> int:
    """INSERT OR IGNORE 到 web_hashes, 然后 SELECT id 返回 hash_id."""
    if not response_hash:
        return 0
    # 跟 modules/main/pdtm/import_scan_results.py 的同名写法一致 (5 个绑定:
    # business_id / response_hash / created_at / updated_at / fetched_at;
    # subdomain_count 是字面量 0, 不占绑定)。
    #
    # 2026-10-22 fix: 这里原本多传了一个 fetched_at (6 个绑定), 语句只有 5 个
    # 占位符, sqlite 抛 "Incorrect number of bindings supplied. The current
    # statement uses 5, and there are 6 supplied." -> run_phase 整个事务
    # rollback, rc=5, T1/T2 的所有行都没落库。
    #
    # 为什么 CI 没抓到: 只有 has_response 分支会走到这里 (update_row_no_response
    # 只写 change_type, 不碰 web_hashes), 而 tests/run_smoke.sh 的 EXPECT_CT
    # 里没有任何端口期望 ct=10 —— ct=10 只能由 has_response 写出, 现有矩阵里
    # 复探要么无响应(28810 -> ct=8)要么不进 reverify, 这条分支在 CI 上是死代码。
    # production schema: subdomain_count / created_at, 没有 first_seen / hit_count
    conn.execute(
        """INSERT INTO web_hashes
               (business_id, response_hash, subdomain_count, created_at, updated_at, fetched_at)
           VALUES (?, ?, 0, ?, ?, ?)
           ON CONFLICT(business_id, response_hash) DO UPDATE SET
             updated_at = excluded.updated_at, fetched_at = excluded.fetched_at""",
        (business_id, response_hash, fetched_at, fetched_at, fetched_at),
    )
    row = conn.execute(
        "SELECT id FROM web_hashes WHERE business_id = ? AND response_hash = ?",
        (business_id, response_hash),
    ).fetchone()
    if not row:
        return 0
    return int(row[0])


# ---------------------------------------------------------------------------
# httpx 字段提取
# ---------------------------------------------------------------------------

def parse_httpx_line(line: str) -> dict[str, Any] | None:
    line = line.strip()
    if not line:
        return None
    try:
        return json.loads(line)
    except json.JSONDecodeError:
        return None


def extract_reverify_row(rec: dict[str, Any], fallback_subdomain: str,
                          fallback_port: int) -> dict[str, Any] | None:
    """从 httpx jsonl 提取 reverify 阶段需要的字段."""
    if rec.get("failed") is True:
        return None
    sc = rec.get("status_code", 0)
    if sc == 0 or sc >= 400:
        return None

    input_url = rec.get("input", "")
    subdomain = fallback_subdomain
    port = fallback_port
    if input_url:
        if "://" in input_url:
            input_url = input_url.split("://", 1)[1]
        if "/" in input_url:
            host_part, _ = input_url.split("/", 1)
        else:
            host_part = input_url
        if ":" in host_part:
            h, p = host_part.rsplit(":", 1)
            subdomain = h
            try:
                port = int(p)
            except ValueError:
                port = fallback_port
        else:
            subdomain = host_part

    body = rec.get("body", "")
    body_bytes = body.encode("utf-8") if isinstance(body, str) else b""
    if body_bytes:
        body_zlib = zlib.compress(body_bytes, 6)
    else:
        body_zlib = None

    hash_dict = rec.get("hash", {}) or {}
    if isinstance(hash_dict, dict):
        response_hash = (
            hash_dict.get("body_mmh3") or hash_dict.get("header_mmh3")
            or hash_dict.get("body") or hash_dict.get("header")
        )
    else:
        response_hash = None
    if not response_hash:
        response_hash = f"{rec.get('status_code', 0)}|{rec.get('content_length', 0)}"

    tech_list = rec.get("tech", []) or []
    technologies = ",".join(t for t in tech_list if t)

    header = rec.get("header", {}) or {}
    content_type = (
        header.get("content_type", "")
        or header.get("Content-Type", "")
    )

    host_ip = rec.get("host_ip", "") or ""
    ips = f'["{host_ip}"]' if host_ip else "[]"

    return {
        "subdomain": subdomain,
        "port": port,
        "hash_id_response_hash": response_hash,
        "status_code": rec.get("status_code", 0),
        "content_length": rec.get("content_length", 0),
        "title": rec.get("title", ""),
        "technologies": technologies,
        "content_type": content_type,
        "ips": ips,
        "timestamp": rec.get("timestamp", ""),
        "curr_body": body_zlib,
        "raw_json": json.dumps(rec, ensure_ascii=False),
    }


# ---------------------------------------------------------------------------
# httpx subprocess 调用
# ---------------------------------------------------------------------------

def run_httpx(httpx_bin: str, input_txt: str, output_jsonl: str,
              resolvers_file: str = "",
              rate_limit: int = 50, timeout: int = 5) -> int:
    """popen httpx subprocess. 返回 exit code."""
    cmd = [
        httpx_bin,
        "-l", input_txt,
        "-sc", "-cl", "-title",
        "-hash", "mmh3,simhash",
        "-random-agent",
        "-follow-redirects", "-max-redirects", "3",
        "-td",
        "-rate-limit", str(rate_limit),
        "-timeout", str(timeout),
        "-silent", "-no-color",
        "-json", "-irr",
    ]
    if resolvers_file:
        cmd += ["-r", resolvers_file]
    print(
        f"[algo_diff_reverify] popen: {' '.join(cmd[:6])}... ({len(cmd)} flags"
        f"{', -r' if resolvers_file else ', no -r'})",
        flush=True,
    )
    print(f"[algo_diff_reverify][DEBUG] cmd full: {cmd}", flush=True)
    with open(output_jsonl, "w") as fout:
        proc = subprocess.run(cmd, stdout=fout, stderr=subprocess.PIPE, timeout=600, check=False)
        fout.flush()
    print(f"[algo_diff_reverify][DEBUG] returncode={proc.returncode}", flush=True)
    if proc.stderr:
        print(f"[algo_diff_reverify][DEBUG] stderr={proc.stderr.decode('utf-8', errors='replace')[:1000]}", flush=True)
    if proc.returncode != 0:
        print(f"[algo_diff_reverify][err] httpx exit={proc.returncode}",
              file=sys.stderr)
        if proc.stderr:
            print(
                f"[algo_diff_reverify][err] stderr: {proc.stderr.decode('utf-8', errors='replace')[:500]}",
                file=sys.stderr,
            )
    return proc.returncode


# ---------------------------------------------------------------------------
# SELECT ct=12/13 行的 (id, subdomain, port) 写到 tmpfile
# ---------------------------------------------------------------------------

def select_pending_rows(db_path: str, ct_value: int) -> list[tuple[int, str, int, int]]:
    """SELECT id, subdomain, port, business_id WHERE change_type = ?"""
    conn = sqlite3.connect(db_path)
    try:
        rows = conn.execute(
            "SELECT id, subdomain, port, business_id FROM web_subdomains "
            "WHERE change_type = ?",
            (ct_value,),
        ).fetchall()
    finally:
        conn.close()
    return [(int(r[0]), str(r[1]), int(r[2]), int(r[3])) for r in rows]


def write_input_txt(rows: list[tuple[int, str, int, int]], path: str) -> None:
    """写 <subdomain>:<port> 一行一个 (httpx -l 格式)."""
    with open(path, "w") as f:
        for _, sub, port, _ in rows:
            f.write(f"{sub}:{port}\n")


# ---------------------------------------------------------------------------
# UPDATE 全部字段
# ---------------------------------------------------------------------------

def update_row_has_response(conn, id_: int, business_id: int, row: dict[str, Any],
                            new_ct: int) -> None:
    hash_id = upsert_web_hash(
        conn, business_id, row["hash_id_response_hash"], row["timestamp"]
    )
    conn.execute(
        """UPDATE web_subdomains SET
             hash_id        = ?,
             url            = ?,
             status_code    = ?,
             content_length = ?,
             title          = ?,
             technologies   = ?,
             last_seen      = ?,
             fetched_at     = ?,
             is_active      = 1,
             raw_json       = ?,
             ips            = ?,
             curr_body      = ?,
             content_type   = ?,
             change_type    = ?
           WHERE id = ?""",
        (
            hash_id,
            f"http://{row['subdomain']}:{row['port']}",
            row["status_code"],
            row["content_length"],
            row["title"],
            row["technologies"],
            row["timestamp"],
            row["timestamp"],
            row["raw_json"],
            row["ips"],
            row["curr_body"],
            row["content_type"],
            new_ct,
            id_,
        ),
    )


def update_row_no_response(conn, id_: int, new_ct: int) -> None:
    # 2026-10-15 fix: 不再 NULL curr_body on T1/T2 miss。
    # C++ algo_diff T3 fast 之前用 pre.curr_body IS NULL 作为 '新行' 代理,
    # 该代理会让 deact→reactivated 行被误判为 ST_1。代理现已替换为
    # 'id NOT in web_subdomains_pre' (pre_is_active_exists=False)。这里仍保留
    # curr_body: 一是为下游 diff / raw_json audit 提供最后一手数据,
    # 二是让 A 算法 (T4) 下一轮能算真实 ratio 而不是掉到 gray-zone-default-2。
    conn.execute(
        """UPDATE web_subdomains SET
             change_type = ?
           WHERE id = ?""",
        (new_ct, id_),
    )


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------

def run_phase(db_path: str, phase: str, httpx_bin: str,
              resolvers_file: str) -> int:
    if phase == "C":
        ct_value = 12
        new_ct_has_resp = 10
        new_ct_no_resp = 13
        phase_name = "T1 C reverify"
    elif phase == "D":
        ct_value = 13
        new_ct_has_resp = 10
        new_ct_no_resp = 8
        phase_name = "T2 D reverify"
    else:
        print(f"[algo_diff_reverify][err] unknown phase: {phase!r}", file=sys.stderr)
        return 2

    rows = select_pending_rows(db_path, ct_value)
    print(f"[algo_diff_reverify] {phase_name} selected {len(rows)} rows (ct={ct_value})",
          flush=True)
    if not rows:
        return 0

    with tempfile.TemporaryDirectory() as tmpdir:
        input_txt = os.path.join(tmpdir, f"ct_{ct_value}_in.txt")
        output_jsonl = os.path.join(tmpdir, f"ct_{ct_value}_out.jsonl")
        write_input_txt(rows, input_txt)

        rc = run_httpx(httpx_bin, input_txt, output_jsonl,
                       resolvers_file=resolvers_file)
        # DEBUG: print output
        with open(output_jsonl) as df:
            data = df.read()
        print(f"[algo_diff_reverify][DEBUG] output_jsonl size={len(data)} content={data[:500]}", flush=True)
        if rc != 0:
            return 4

        responses: dict[str, dict[str, Any]] = {}
        with open(output_jsonl) as f:
            for line in f:
                rec = parse_httpx_line(line)
                if rec is None:
                    continue
                host = rec.get("host", "")
                port = rec.get("port", 0)
                try:
                    port_int = int(port)
                except (ValueError, TypeError):
                    port_int = 0
                key = f"{host}:{port_int}" if host else rec.get("input", "")
                if key:
                    responses[key] = rec
        print(f"[algo_diff_reverify][DEBUG] responses keys: {list(responses.keys())}", flush=True)
        print(f"[algo_diff_reverify][DEBUG] fixture keys: {[f'{s}:{p}' for _,s,p,_ in rows]}", flush=True)

        conn = sqlite3.connect(db_path)
        try:
            n_has_resp = 0
            n_no_resp = 0
            for id_, sub, port, biz_id in rows:
                key = f"{sub}:{port}"
                rec = responses.get(key)
                if rec is None:
                    update_row_no_response(conn, id_, new_ct_no_resp)
                    n_no_resp += 1
                    continue
                row = extract_reverify_row(rec, sub, port)
                if row is None:
                    update_row_no_response(conn, id_, new_ct_no_resp)
                    n_no_resp += 1
                    continue
                update_row_has_response(conn, id_, biz_id, row, new_ct_has_resp)
                n_has_resp += 1
            conn.commit()
        except (sqlite3.Error, OSError) as e:
            conn.rollback()
            print(f"[algo_diff_reverify][err] batch update failed: {e}",
                  file=sys.stderr)
            return 5
        finally:
            conn.close()
        print(
            f"[algo_diff_reverify] {phase_name} has_resp={n_has_resp} no_resp={n_no_resp}",
            flush=True,
        )
    return 0


# ---------------------------------------------------------------------------
# CLI 入口
# ---------------------------------------------------------------------------

def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("db_path")
    p.add_argument("phase", choices=["C", "D"])
    p.add_argument(
        "--pdtm-conf",
        default=None,
        help="pdtm.conf path (默认 = <db_path>/../../config/pdtm.conf)",
    )
    args = p.parse_args()

    conf_path = args.pdtm_conf or default_pdtm_conf(args.db_path)
    conf = read_pdtm_conf(conf_path)
    print(f"[algo_diff_reverify] conf={conf_path} keys={sorted(conf.keys())}",
          flush=True)

    httpx_bin = resolve_httpx_bin(conf)
    resolvers_file = resolve_resolvers_file(conf, args.db_path)
    print(f"[algo_diff_reverify] httpx_bin={httpx_bin}", flush=True)
    print(f"[algo_diff_reverify] resolvers_file={resolvers_file or '(none)'}",
          flush=True)

    if not verify_schema(args.db_path):
        return 3

    t0 = time.time()
    rc = run_phase(args.db_path, args.phase, httpx_bin, resolvers_file)
    print(f"[algo_diff_reverify] {args.phase} done in {time.time() - t0:.2f}s (rc={rc})",
          flush=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())