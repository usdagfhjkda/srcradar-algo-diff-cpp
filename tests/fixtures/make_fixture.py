#!/usr/bin/env python3
"""
make_fixture.py — 生成 algo_diff_cpp CI 测试 fixture (11 case)

11 种 transition case:
  1: 00    a0b0 (T3 fast)              → 0
  2: 01    prev_body NULL (T3 fast)    → 1
  3: 0A0   a1b1 同 body                → T3→10; T4 ratio=1.0 → 0
  4: 0A2   a1b1 diff body              → T3→10; T4 gray M.1 → 2
  5: 0B4   a0b1 复活 同 body           → T3→10; T4 ratio=1.0 → 0 + B=4 = 4
  6: 0B6   a0b1 复活 diff body         → T3→10; T4 gray M.1 → 2 + B=4 = 6
  7: 0C0   C mock 同 body              → T4 ratio=1.0 → 0
  8: 0C2   C mock diff body            → T4 gray M.1 → 2
  9: 0CD8  D no resp 终态               → 8
  10: 0CD0 D mock 同 body              → T4 ratio=1.0 → 0
  11: 0CD2 D mock diff body            → T4 gray M.1 → 2

直接 mock: 不调 httpx, fixture 预置 T1/T2 跑完后的状态.
raw_json 写死 + 加时间戳/随机数干扰 (T.1).
S.1: fixture 起始 body == cur body (mock T1/T2 后).
U.1: 不要 mock server.
V.1: case 1 pre.is_active=0, case 2 pre.is_active=1.
W.1: case 7/8/10/11 pre.is_active 都 1.

用法:
  python3 make_fixture.py <db_path>
"""
import os
import random
import sqlite3
import sys
import time
import zlib


def make_fixture(db_path: str) -> None:
    if os.path.exists(db_path):
        os.remove(db_path)

    conn = sqlite3.connect(db_path)
    c = conn.cursor()

    # ---- tables ----
    c.execute('''CREATE TABLE web_subdomains (
        id INTEGER PRIMARY KEY,
        business_id INTEGER NOT NULL,
        hash_id INTEGER NOT NULL,
        subdomain TEXT NOT NULL,
        port INTEGER NOT NULL,
        url TEXT,
        status_code INTEGER,
        content_length INTEGER,
        title TEXT,
        technologies TEXT,
        first_seen TEXT NOT NULL,
        last_seen TEXT NOT NULL,
        fetched_at TEXT NOT NULL,
        is_active INTEGER NOT NULL DEFAULT 1,
        raw_json TEXT,
        change_type INTEGER NOT NULL DEFAULT 0,
        ips TEXT DEFAULT '[]',
        prev_body BLOB,
        curr_body BLOB,
        content_type TEXT,
        last_comparison_ratio REAL,
        last_comparison_kind TEXT,
        last_comparison_at TEXT
    )''')

    c.execute('''CREATE TABLE IF NOT EXISTS web_subdomain_baseline (
        subdomain_id INTEGER PRIMARY KEY,
        baseline_ratio REAL NOT NULL,
        sample_count INTEGER NOT NULL DEFAULT 1,
        created_at TEXT NOT NULL,
        updated_at TEXT NOT NULL
    )''')

    c.execute('''CREATE TABLE web_subdomains_pre (
        id INTEGER PRIMARY KEY,
        business_id INTEGER NOT NULL,
        hash_id INTEGER NOT NULL,
        subdomain TEXT NOT NULL,
        port INTEGER NOT NULL,
        url TEXT,
        status_code INTEGER,
        content_length INTEGER,
        title TEXT,
        technologies TEXT,
        first_seen TEXT NOT NULL,
        last_seen TEXT NOT NULL,
        fetched_at TEXT NOT NULL,
        is_active INTEGER NOT NULL DEFAULT 1,
        raw_json TEXT,
        change_type INTEGER NOT NULL DEFAULT 0,
        ips TEXT DEFAULT '[]',
        prev_body BLOB,
        curr_body BLOB,
        content_type TEXT,
        last_comparison_ratio REAL,
        last_comparison_kind TEXT,
        last_comparison_at TEXT,
        _snapshot_at TEXT NOT NULL DEFAULT (datetime('now'))
    )''')

    c.execute('''CREATE TABLE _test_meta (
        id INTEGER PRIMARY KEY,
        case_name TEXT NOT NULL,
        expected_ct INTEGER NOT NULL,
        note TEXT
    )''')

    # 跟 production web_hashes schema 对齐 (modules/main/db/schema.sql):
    #   created_at (不是 first_seen) / subdomain_count (不是 hit_count)
    #   + url_count / score / description / score_initialized_at /
    #     change_type / learned_anchors 都有默认值
    c.execute('''CREATE TABLE web_hashes (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        business_id INTEGER NOT NULL,
        response_hash TEXT NOT NULL,
        subdomain_count INTEGER NOT NULL DEFAULT 0,
        url_count INTEGER NOT NULL DEFAULT 0,
        score INTEGER DEFAULT NULL,
        description TEXT NOT NULL DEFAULT '',
        score_initialized_at TEXT DEFAULT NULL,
        created_at TEXT NOT NULL,
        updated_at TEXT NOT NULL,
        fetched_at TEXT NOT NULL,
        change_type INTEGER NOT NULL DEFAULT 0,
        learned_anchors TEXT NOT NULL DEFAULT '[]',
        UNIQUE (business_id, response_hash)
    )''')

    # ---- bodies + 干扰 ----
    body_a = b'<!DOCTYPE html><html><body>Service A</body></html>'
    body_a_zlib = zlib.compress(body_a, 6)

    ts = int(time.time())
    rand = random.randint(0, 1000000)
    body_b = f'<!DOCTYPE html><html><body>Service B ts={ts} rand={rand}</body></html>'.encode()
    body_b_zlib = zlib.compress(body_b, 6)

    raw_a = f'{{"hash": {{"body_mmh3": "1234567890"}}, "ts": "{ts}", "rand": {rand}, "body": "Service A"}}'
    raw_b = f'{{"hash": {{"body_mmh3": "9876543210"}}, "ts": "{ts}", "rand": {rand}, "body": "Service B"}}'

    c.execute('''INSERT INTO web_hashes
        (id, business_id, response_hash, created_at, updated_at, fetched_at)
        VALUES (1, 1, '1234567890', '2026-10-08T00:00:00Z', '2026-10-08T00:00:00Z', '2026-10-08T00:00:00Z')''')
    c.execute('''INSERT INTO web_hashes
        (id, business_id, response_hash, created_at, updated_at, fetched_at)
        VALUES (2, 1, '9876543210', '2026-10-08T00:00:00Z', '2026-10-08T00:00:00Z', '2026-10-08T00:00:00Z')''')

    pre_status_code = 200
    pre_content_length = len(body_a)
    pre_title = 'Pre Title A'
    pre_technologies = ''
    pre_content_type = 'text/html'

    NONE = None

    # cur 端 5 字段模板 (cur_body=body_a / body_b / NONE 各一套)
    # T4 Step 1/2 拿 cur 跟 pre 端 5 字段真比较, 不能 hardcode 让两边总相等.
    cur_a_status_code = 200
    cur_a_content_length = len(body_a)
    cur_a_title = 'Pre Title A'
    cur_a_technologies = ''
    cur_a_content_type = 'text/html'

    cur_b_status_code = 200
    cur_b_content_length = len(body_b)
    cur_b_title = 'Service B Title'   # 跟 body_b 内容对应, 跟 pre_title 不同
    cur_b_technologies = ''
    cur_b_content_type = 'text/html'

    # 18 列: (id, name, start_ct, cur_body_zlib, hash_id, pre_body_zlib_or_None,
    #         cs, ccl, ct_t, ctech, cct,   # cur 端 5 字段
    #         ps, pcl, pt, ptech, pct,      # pre 端 5 字段
    #         pia, cia, expected, note)
    cases = [
        (1,  '00',   0, body_a_zlib, 1, body_a_zlib,
         cur_a_status_code, cur_a_content_length, cur_a_title, cur_a_technologies, cur_a_content_type,
         pre_status_code, pre_content_length, pre_title, pre_technologies, pre_content_type,
         0, 0, 0, 'a0b0 T3 fast → 0 (cur=0 pre=0)'),
        (2,  '01',   0, NONE, 1, NONE,
         0, 0, '', '', '',
         0, 0, '', '', '',
         1, 1, 1, 'id NOT in pre (true new row) → ST_1 via pre_is_active_exists'),
        (3,  '0A0',  0, body_a_zlib, 1, body_a_zlib,
         cur_a_status_code, cur_a_content_length, cur_a_title, cur_a_technologies, cur_a_content_type,
         pre_status_code, pre_content_length, pre_title, pre_technologies, pre_content_type,
         1, 1, 0, 'a1b1 同 body T4 ratio=1.0 → 0'),
        (4,  '0A2',  0, body_b_zlib, 2, body_a_zlib,
         cur_b_status_code, cur_b_content_length, cur_b_title, cur_b_technologies, cur_b_content_type,
         pre_status_code, pre_content_length, pre_title, pre_technologies, pre_content_type,
         1, 1, 2, 'a1b1 diff body T4 gray M.1 → 2 (title+body 变)'),
        (5,  '0B4',  0, body_a_zlib, 1, body_a_zlib,
         cur_a_status_code, cur_a_content_length, cur_a_title, cur_a_technologies, cur_a_content_type,
         pre_status_code, pre_content_length, pre_title, pre_technologies, pre_content_type,
         0, 1, 4, 'a0b1 复活 同 body T4 ratio=1.0 + B=4 → 4 (cur=1 pre=0)'),
        (6,  '0B6',  0, body_b_zlib, 2, body_a_zlib,
         cur_b_status_code, cur_b_content_length, cur_b_title, cur_b_technologies, cur_b_content_type,
         pre_status_code, pre_content_length, pre_title, pre_technologies, pre_content_type,
         0, 1, 6, 'a0b1 复活 diff body T4 gray M.1 + B=4 → 6'),
        (7,  '0C0',  10, body_a_zlib, 1, body_a_zlib,
         cur_a_status_code, cur_a_content_length, cur_a_title, cur_a_technologies, cur_a_content_type,
         pre_status_code, pre_content_length, pre_title, pre_technologies, pre_content_type,
         1, 1, 0, 'C mock 同 body T4 ratio=1.0 → 0'),
        (8,  '0C2',  10, body_b_zlib, 2, body_a_zlib,
         cur_b_status_code, cur_b_content_length, cur_b_title, cur_b_technologies, cur_b_content_type,
         pre_status_code, pre_content_length, pre_title, pre_technologies, pre_content_type,
         1, 1, 2, 'C mock diff body T4 gray M.1 → 2'),
        (9,  '0CD8', 8, NONE, 1, body_a_zlib,
         0, 0, '', '', '',
         pre_status_code, pre_content_length, pre_title, pre_technologies, pre_content_type,
         1, 1, 8, 'D mock no resp 终态 → 8'),
        (10, '0CD0', 10, body_a_zlib, 1, body_a_zlib,
         cur_a_status_code, cur_a_content_length, cur_a_title, cur_a_technologies, cur_a_content_type,
         pre_status_code, pre_content_length, pre_title, pre_technologies, pre_content_type,
         1, 1, 0, 'D mock 同 body T4 ratio=1.0 → 0'),
        (11, '0CD2', 10, body_b_zlib, 2, body_a_zlib,
         cur_b_status_code, cur_b_content_length, cur_b_title, cur_b_technologies, cur_b_content_type,
         pre_status_code, pre_content_length, pre_title, pre_technologies, pre_content_type,
         1, 1, 2, 'D mock diff body T4 gray M.1 → 2'),
    ]

    # The fixture rows carry a placeholder host that resolves to
    # 127.0.0.1 (a public loopback test domain). No assertion depends
    # on the host value itself; the rows are mocked straight into
    # SQLite and the fixture does no DNS resolution.
    for (cid, name, ct, cur_body, hid, pre_body,
         cs, ccl, ct_t, ctech, cct,
         ps, pcl, pt, ptech, pct,
         pia, cia, expected, _note) in cases:
        c.execute('''INSERT INTO web_subdomains
            (id, business_id, hash_id, subdomain, port, status_code, content_length,
             title, technologies, content_type,
             first_seen, last_seen, fetched_at, is_active,
             raw_json, curr_body, prev_body, change_type)
            VALUES (?, 1, ?, 'www.localtest.me', 18000,
                    ?, ?, ?, ?, ?,
                    '2026-10-08T00:00:00Z', '2026-10-08T00:00:00Z', '2026-10-08T00:00:00Z',
                    ?, ?, ?, ?, ?)''',
            (cid, hid, cs, ccl, ct_t, ctech, cct,
             cia,
             raw_a if hid == 1 else raw_b,
             cur_body, cur_body, ct))
        # Case 2 (id NOT in pre) intentionally has NO pre row — exercises the
        # pre_is_active_exists=False → ST_1 path after the proxy removal (was prev_body_null check in C++ state_machine.cpp).
        if cid != 2:
            c.execute('''INSERT INTO web_subdomains_pre
                (id, business_id, hash_id, subdomain, port, status_code, content_length,
                 title, technologies, content_type,
                 first_seen, last_seen, fetched_at, is_active,
                 raw_json, curr_body, prev_body, change_type)
                VALUES (?, 1, ?, 'www.localtest.me', 18000,
                        ?, ?, ?, ?, ?,
                        '2026-10-08T00:00:00Z', '2026-10-08T00:00:00Z', '2026-10-08T00:00:00Z',
                        ?, ?, ?, ?, 0)''',
                (cid, hid, ps, pcl, pt, ptech, pct,
                 pia,
                 raw_a if hid == 1 else raw_b,
                 pre_body, body_a_zlib))
        c.execute('''INSERT INTO _test_meta
            (id, case_name, expected_ct, note) VALUES (?, ?, ?, ?)''',
            (cid, name, expected, _note))

    conn.commit()
    conn.close()
    print(f"[make_fixture] created {db_path} with 11 cases")
    print(f"  body_a (固定): {len(body_a)}B")
    print(f"  body_b (干扰): {len(body_b)}B ts={ts} rand={rand}")


if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else 'test_ct_cases.db'
    make_fixture(out)
