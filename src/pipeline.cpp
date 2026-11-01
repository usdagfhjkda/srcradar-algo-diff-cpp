// pipeline.cpp — 主流程 orchestration (算法 v10: 能力补齐 + 批量化)
//
// 4 个 stage (顺序锁定):
//   T1. C reverify (ct=12)     → spawn Python algo_diff_reverify.py 跑 T1
//   T2. D reverify (ct=13)     → spawn Python algo_diff_reverify.py 跑 T2
//   T3. fast transition (ct=0) → 4 分支, 1/0/10/12 写回 (C++ 实现)
//   T4. 变更判定 (ct=10)       → 投影 + mask + anchors + difflib ratio
//
// 2026-10-24 变更 (算法能力补齐):
//   T4 不再是"raw body 直接算 ratio", 改为与 Python compute.py 对齐的完整链路:
//     快字段 diff → body 字节相同 → 长度快筛 → 动态字段 mask →
//     Content-Type 投影 → anchors 挖动态区 → difflib ratio → 三段阈值
//   相似度算法从 rapidfuzz Indel 换成 difflib (Ratcliff-Obershelp),
//   与 Python 逐块对齐, 否则 0.98 阈值判定会分叉。
//   新增 baseline / field_length_stats / learned_anchors 三张表的写回。
//
// 批量化:
//   T3 / T4 都按 id 游标分页处理 (LIMIT batch_size), 不再一次性载入全表;
//   每批的 UPDATE / UPSERT 各自在一个事务内提交。
//
// T1/T2 reverify 实现:
//   调 lib/algo_diff_reverify.py (Python 封装). 该脚本自己:
//     - 读 pdtm.conf 拿 httpx_bin + resolvers_file
//     - SELECT ct=12 (或 13) 行
//     - 写 tmpfile (subdomain:port 一行一个)
//     - popen httpx -l tmpfile -sc -cl ... -json -irr -r resolvers
//     - parse jsonl
//     - upsert_web_hash (mmh3, 跟 import_scan_results 一致)
//     - UPDATE web_subdomains 全部字段 + curr_body(zlib) + change_type
//   C++ 端只负责 spawn Python + wait + log.

#include "pipeline.hpp"
#include "http_fetch.hpp"

#include "algo_core.hpp"
#include "anchors.hpp"
#include "blake2b.hpp"
#include "difflib.hpp"
#include "mask.hpp"
#include "sequence_matcher.hpp"
#include "sqlite_io.hpp"
#include "state_machine.hpp"
#include "zlib_codec.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace algo_diff_cpp {

namespace {

std::string now_iso8601() {
    // 用 time() 而不是 chrono 强转 (chrono::time_point 不能直接转 time_t)
    auto tt = std::time(nullptr);
    std::tm tm = *std::gmtime(&tt);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

void log(const std::string& run_id, const std::string& msg) {
    std::fprintf(stderr, "[algo_diff_cpp][%s] %s\n",
                 run_id.empty() ? "?" : run_id.c_str(),
                 msg.c_str());
}

// T1 / T2 reverify:
//   调外部 Python helper (lib/algo_diff_reverify.py).
//   路径相对 C++ binary 位置, 默认 ../lib/algo_diff_reverify.py
//   (CWD 是 <module>/bin/ 时解析成 <module>/lib/algo_diff_reverify.py).
//   C++ 端只 spawn + wait, 不自己处理 jsonl / db / httpx.
//
// 返回 Python 进程的 exit code (0=OK, 2/3/4/5/6 是 algo_diff_reverify.py 的退出码).
// 这里不加额外包装, 原样透传.
int run_reverify_helper(const std::string& run_id,
                         const PipelineOptions& opts,
                         const std::string& phase) {
    const std::string& helper = opts.reverify_helper_path;
    // shell 单 quotes 包路径 + arg, 防空格
    std::string cmd = "python3 '" + helper + "' '" + opts.db_path + "' " + phase;
    log(run_id, "stage_reverify phase=" + phase + ": spawn " + helper);
    // 用 system() 阻塞等, 简单可靠; Python 输出自然流到 stderr/stdout (继承自 C++)
    int rc = std::system(cmd.c_str());
    // system() 返回值是 waitpid 状态, 不是 exit code 直接值
    // 用 WEXITSTATUS 解析 (假设正常退出)
    int exit_code = 0;
    if (rc == -1) {
        log(run_id, "stage_reverify phase=" + phase + ": system() fork failed");
        return 6;  // helper 失败
    }
    if (WIFEXITED(rc)) {
        exit_code = WEXITSTATUS(rc);
    } else if (WIFSIGNALED(rc)) {
        log(run_id, "stage_reverify phase=" + phase + ": helper killed by signal " +
                    std::to_string(WTERMSIG(rc)));
        return 6;
    } else {
        log(run_id, "stage_reverify phase=" + phase + ": helper exited abnormally");
        return 6;
    }
    log(run_id, "stage_reverify phase=" + phase + ": helper exited rc=" +
                std::to_string(exit_code));
    return exit_code;
}

int stage_reverify(const std::string& run_id,
                   const PipelineOptions& opts,
                   int8_t reverify_ct,
                   const std::string& phase) {
    // reverify_ct 只用于日志/调试, 当前逻辑由 phase 决定 (C/D)
    (void)reverify_ct;
    if (opts.skip_reverify) {
        log(run_id, "skipping reverify phase=" + phase);
        return 0;
    }

    int rc = run_reverify_helper(run_id, opts, phase);
    if (rc != 0) {
        return (rc == 2 || rc == 3 || rc == 4 || rc == 5) ? rc : 6;
    }
    return 0;
}

// T3 fast transition (ct=0) 4 分支 — 分页批处理
int stage_fast(const std::string& run_id, const PipelineOptions& opts) {
    std::vector<Row> rows;
    std::string err;
    if (!select_for_fast_transition(opts.db_path, rows, err)) {
        log(run_id, "stage_fast select failed: " + err);
        return 5;
    }
    if (rows.empty()) {
        log(run_id, "stage_fast no rows (ct=0)");
        return 0;
    }
    log(run_id, "stage_fast selected " + std::to_string(rows.size()) + " rows (ct=0)");

    std::vector<UpdateRow> updates;
    updates.reserve(rows.size());
    int n_new = 0, n_a0b0 = 0, n_a1b0 = 0, n_to_a = 0;
    for (const auto& r : rows) {
        CtState next = fast_transition(
            r.ct, r.curr_is_active,
            r.pre_is_active_exists, r.pre_is_active);
        UpdateRow u;
        u.id = r.id;
        u.new_ct = static_cast<int8_t>(next);
        u.ratio = std::nan("");
        u.kind = "fast";
        u.at = now_iso8601();
        updates.push_back(u);
        switch (next) {
            case CtState::ST_1: n_new++;   break;
            case CtState::ST_0: n_a0b0++;  break;
            case CtState::ST_C: n_a1b0++;  break;
            case CtState::ST_A: n_to_a++;  break;
            default: break;
        }
    }
    if (!opts.dry_run) {
        if (!update_change_type_batch(opts.db_path, updates, err)) {
            log(run_id, "stage_fast update failed: " + err);
            return 5;
        }
    }
    std::ostringstream s;
    s << "stage_fast transition: new=" << n_new
      << " a0b0=" << n_a0b0
      << " a1b0(c)=" << n_a1b0
      << " to_a=" << n_to_a
      << " total=" << updates.size();
    log(run_id, s.str());
    return 0;
}

// ==================== T4 变更判定 (ct=10) ====================
//
// 完整链路 (与 Python compute.py 对齐):
//   1. 解压 pre.curr_body (prev body 来源) 与 cur.curr_body
//   2. 快字段 diff / body 字节相同 / 长度快筛 (短路)
//   3. 动态字段 mask (按 field_length_stats)
//   4. Content-Type 投影
//   5. anchors (优先用跨天累积的 learned_anchors, 没有才现场发现)
//   6. difflib ratio + 三段阈值
//   7. 写回: change_type / ratio / kind / at + baseline + length_stats + anchors
//
// 分页: 按 id 游标取 ct=10 的行, 每批 batch_size 条, 每批一个事务。

// ====================================================
// 3 包取样 stage (2026-10-25)
// ====================================================
// 灰区行只有 pre + cur 两点, 无法区分"动态噪声"与"同长度的真实变更"。
// 本 stage 对待判定行 (ct=10) 在同一时刻连发 fetch_samples 次, 拿到额外
// body 样本, 写 curr_body_1 / curr_body_2, 供 T4 的三包判定使用。
//
// 批量约束 (用户拍板): 每轮一个 httpx 进程处理整批 URL, 绝不在
// while/for 里逐个调 httpx。
int stage_3pkg_fetch(const std::string& run_id, const PipelineOptions& opts) {
    if (opts.fetch_samples <= 0) {
        log(run_id, "stage_3pkg: disabled (fetch_samples=0)");
        return 0;
    }
    if (opts.httpx_path.empty() || opts.httpx_path == "httpx") {
        // httpx 路径未配置 -> 降级 (T4 走 2 包路径), 不报错。
        log(run_id, "stage_3pkg: httpx path not configured, skip (2-pkg fallback)");
        return 0;
    }

    std::string err;
    std::vector<ThreePkgTarget> targets;
    if (!select_three_pkg_targets(opts.db_path, opts.business_id, targets, err)) {
        log(run_id, "stage_3pkg select failed: " + err);
        return 0;  // 降级, 不中断流水线
    }
    if (targets.empty()) {
        log(run_id, "stage_3pkg: no candidates (ct=10 with body)");
        return 0;
    }

    // 整批 URL
    std::vector<std::string> urls;
    urls.reserve(targets.size());
    for (const ThreePkgTarget& t : targets) {
        if (!t.url.empty()) urls.push_back(t.url);
    }
    if (urls.empty()) {
        log(run_id, "stage_3pkg: candidates have no url, skip");
        return 0;
    }

    std::ostringstream s0;
    s0 << "stage_3pkg: " << targets.size() << " candidates, "
       << opts.fetch_samples << " rounds";
    log(run_id, s0.str());

    // 每一轮 = 一次 httpx 进程 (整批)
    std::vector<HttpFetchSample> samples;
    for (int round = 0; round < opts.fetch_samples; ++round) {
        HttpFetchSample sample;
        std::string ferr;
        if (!http_fetch_batch(opts.httpx_path, urls, opts.resolvers_path,
                              opts.tmp_dir, sample, ferr)) {
            log(run_id, "stage_3pkg round " + std::to_string(round + 1) +
                       " fetch failed: " + ferr + " (2-pkg fallback)");
            return 0;
        }
        std::ostringstream s1;
        s1 << "stage_3pkg round " << (round + 1) << ": "
           << sample.by_hostport.size() << " responses";
        log(run_id, s1.str());
        samples.push_back(std::move(sample));
    }

    // 写回 curr_body_1 / curr_body_2
    if (opts.dry_run) {
        log(run_id, "stage_3pkg: dry-run, skip writeback");
        return 0;
    }
    std::vector<ThreePkgBodyRow> body_rows;
    for (const ThreePkgTarget& t : targets) {
        ThreePkgBodyRow row;
        row.id = t.id;
        const std::string key = t.host_port;
        bool ok = true;
        for (int i = 0; i < opts.fetch_samples && i < 2; ++i) {
            const HttpFetchSample& smp = samples[static_cast<size_t>(i)];
            auto it = smp.by_hostport.find(key);
            if (it == smp.by_hostport.end()) {
                ok = false;
                break;
            }
            std::vector<unsigned char> raw(it->second.begin(), it->second.end());
            std::vector<unsigned char> comp;
            std::string zerr;
            if (!deflate_zlib(raw, comp, zerr)) {
                ok = false;
                break;
            }
            if (i == 0) row.body_1 = comp;
            else       row.body_2 = comp;
        }
        if (ok) body_rows.push_back(row);
    }
    if (!body_rows.empty()) {
        if (!update_three_pkg_bodies(opts.db_path, body_rows, err)) {
            log(run_id, "stage_3pkg writeback failed: " + err);
            return 0;  // 降级
        }
        std::ostringstream s2;
        s2 << "stage_3pkg: wrote " << body_rows.size() << " rows";
        log(run_id, s2.str());
    }
    return 0;
}

int stage_a_algorithm(const std::string& run_id, const PipelineOptions& opts) {
    int64_t after_id = 0;
    int64_t batch = static_cast<int64_t>(opts.batch_size);
    if (batch <= 0) batch = 5000;

    size_t total_rows = 0;
    size_t n_similar = 0, n_different = 0, n_ambiguous = 0, n_fast = 0;
    int64_t batch_no = 0;

    while (true) {
        std::vector<Row> rows;
        std::string err;
        if (!select_for_a_algorithm(opts.db_path, rows, err, after_id, batch)) {
            log(run_id, "stage_a select failed: " + err);
            return 5;
        }
        if (rows.empty()) break;
        ++batch_no;

        // ---- 批量加载本批涉及的 host 的长度统计 (一次查询, 避免 N+1) ----
        std::vector<std::string> hosts;
        {
            std::set<std::string> uniq;
            for (const Row& r : rows) {
                if (!r.host.empty()) uniq.insert(r.host);
            }
            hosts.assign(uniq.begin(), uniq.end());
        }
        std::vector<LengthStat> all_stats;
        if (!select_length_stats_multi(opts.db_path, hosts, all_stats, err)) {
            log(run_id, "stage_a select_length_stats failed: " + err);
            return 5;
        }
        std::map<std::string, std::vector<LengthStat>> stats_by_host;
        for (const LengthStat& st : all_stats) {
            stats_by_host[st.host].push_back(st);
        }

        // ---- 逐行判定 ----
        std::vector<UpdateRow> updates;
        std::vector<BaselineRow> baselines;
        std::vector<LengthStatRow> length_stats_out;
        std::vector<AnchorRow> anchors_out;
        updates.reserve(rows.size());

        for (const Row& r : rows) {
            // 解压 body。prev body 来自 web_subdomains_pre.curr_body
            // (prev_body 字段已弃用)。
            std::vector<unsigned char> pre_raw = r.pre_curr_body;
            std::vector<unsigned char> cur_raw = r.curr_body;
            std::vector<unsigned char> pre_body, cur_body;
            if (!pre_raw.empty()) {
                std::string ierr;
                if (!inflate_zlib(pre_raw, pre_body, ierr)) {
                    log(run_id, "stage_a row " + std::to_string(r.id) +
                                " zlib inflate failed (pre): " + ierr);
                    return 4;  // fail-fast
                }
            }
            if (!cur_raw.empty()) {
                std::string ierr;
                if (!inflate_zlib(cur_raw, cur_body, ierr)) {
                    log(run_id, "stage_a row " + std::to_string(r.id) +
                                " zlib inflate failed (cur): " + ierr);
                    return 4;
                }
            }

            CompareInput in;
            in.prev_title = r.pre_title;
            in.curr_title = r.cur_title;
            in.prev_status_code = r.pre_status_code;
            in.curr_status_code = r.cur_status_code;
            in.prev_technologies = r.pre_technologies;
            in.curr_technologies = r.cur_technologies;
            // PATCH-FINAL-URL-2026-10-28: 填 STEP 1 快字段里那个一直没被
            // 用过的 redirect 槽位。取的是 httpx 跟随重定向后的最终落点
            // (final_url), 不是 url 列 —— url 的形态逐日会翻转 (域名 <-> IP),
            // 补进去会把形态抖动误判成变更。
            // 2xx 资产该字段为空 -> 两边都空即相等 -> 自动跳过。
            in.prev_redirect = r.pre_final_url;
            in.curr_redirect = r.cur_final_url;
            // 长度比较只用 body 的实际字节长度 (见 algo_core.cpp 的 STEP 3),
            // 判定层不再需要 content_length。
            in.prev_body.assign(reinterpret_cast<const char*>(pre_body.data()),
                                pre_body.size());
            in.curr_body.assign(reinterpret_cast<const char*>(cur_body.data()),
                                cur_body.size());
            in.content_type = r.cur_content_type;
            in.baseline_exists = r.baseline_exists;
            in.baseline_ratio = r.baseline_ratio;

            // 三包样本 (2026-10-25): 同一时刻连发的 2 份 body。
            // 解压失败 -> 留空 -> T4 走原来的 2 包路径 (降级)。
            if (!r.curr_body_1.empty()) {
                std::vector<unsigned char> d1, d2;
                std::string zerr;
                if (inflate_zlib(r.curr_body_1, d1, zerr)) {
                    in.pkg2_body.assign(d1.begin(), d1.end());
                }
                if (!r.curr_body_2.empty() &&
                    inflate_zlib(r.curr_body_2, d2, zerr)) {
                    in.pkg3_body.assign(d2.begin(), d2.end());
                }
            }

            auto sit = stats_by_host.find(r.host);
            if (sit != stats_by_host.end()) in.length_stats = sit->second;

            // 跨天累积的锚点
            if (!r.learned_anchors_json.empty() &&
                r.learned_anchors_json != "[]") {
                in.learned_anchors = anchors_from_json(r.learned_anchors_json);
            }

            CompareResult res = compare(in);

            // B = 4 + A (reactivated 区分)
            int8_t final_ct = res.change_type;
            if (!r.pre_is_active) {
                final_ct = static_cast<int8_t>(final_ct + 4);
            }

            UpdateRow u;
            u.id = r.id;
            u.new_ct = final_ct;
            u.ratio = res.ratio;
            u.kind = res.kind;
            u.at = now_iso8601();
            updates.push_back(u);

            // baseline 写回 (滑动平均, 与 Python 一致)
            if (!res.kind.empty() && res.kind != std::string(kind::FAST_FIELD) &&
                res.ratio > 0.0) {
                BaselineRow b;
                b.subdomain_id = r.id;
                b.ratio = res.ratio;
                baselines.push_back(b);
            }

            // field_length_stats 写回 (本轮观测到的叶值)
            for (const FieldPair& fp : res.observed_pairs) {
                if (fp.value.empty() || fp.value.size() > MAX_MASKABLE_LENGTH) {
                    continue;
                }
                LengthStatRow lr;
                lr.host = r.host;
                lr.path = fp.path;
                lr.length = static_cast<int64_t>(fp.value.size());
                lr.content_sample_hash = blake2b_64_signed_int(fp.value);
                length_stats_out.push_back(lr);
            }

            // learned_anchors 写回 (跨天累积)
            if (r.hash_id > 0 && !res.learned_anchors.empty()) {
                AnchorRow ar;
                ar.hash_id = r.hash_id;
                ar.anchors_json = anchors_to_json(res.learned_anchors);
                anchors_out.push_back(ar);
            }

            if (res.kind == kind::BODY_SIMILAR) ++n_similar;
            else if (res.kind == kind::BODY_DIFFERENT) ++n_different;
            else if (res.kind == kind::FAST_FIELD) ++n_fast;
            else ++n_ambiguous;
        }

        // ---- 批量写回 (每批一个事务) ----
        if (!opts.dry_run) {
            if (!update_change_type_batch(opts.db_path, updates, err)) {
                log(run_id, "stage_a update failed: " + err);
                return 5;
            }
            if (!upsert_baseline_batch(opts.db_path, baselines, err)) {
                log(run_id, "stage_a baseline upsert failed: " + err);
                return 5;
            }
            if (!upsert_length_stats_batch(opts.db_path, length_stats_out, err)) {
                log(run_id, "stage_a length_stats upsert failed: " + err);
                return 5;
            }
            if (!update_learned_anchors_batch(opts.db_path, anchors_out, err)) {
                log(run_id, "stage_a learned_anchors update failed: " + err);
                return 5;
            }
        }

        total_rows += rows.size();
        after_id = rows.back().id;

        std::ostringstream s;
        s << "stage_a batch " << batch_no << ": processed=" << rows.size()
          << " total=" << total_rows
          << " similar=" << n_similar
          << " different=" << n_different
          << " fast=" << n_fast
          << " ambiguous=" << n_ambiguous;
        log(run_id, s.str());

        // 本批行数小于 batch 说明已到末尾
        if (static_cast<int64_t>(rows.size()) < batch) break;
    }

    if (total_rows == 0) {
        log(run_id, "stage_a no rows (ct=10)");
        return 0;
    }
    std::ostringstream s;
    s << "stage_a computed: " << total_rows
      << " rows similar=" << n_similar
      << " different=" << n_different
      << " fast=" << n_fast
      << " ambiguous=" << n_ambiguous;
    log(run_id, s.str());
    return 0;
}

}  // namespace

int run_pipeline(const PipelineOptions& opts) {
    log(opts.run_id, "=== start === db=" + opts.db_path +
                      " business_id=" + std::to_string(opts.business_id) +
                      " dry=" + std::to_string(opts.dry_run));

    std::string err;
    if (!verify_schema(opts.db_path, err)) {
        log(opts.run_id, "verify_schema failed: " + err);
        return 3;
    }

    // T3: fast transition (ct=0) — C++ 实现
    if (int rc = stage_fast(opts.run_id, opts); rc != 0) {
        return rc;
    }
    // T1: C reverify (ct=12) — spawn Python
    if (int rc = stage_reverify(opts.run_id, opts, 12, "C"); rc != 0) {
        return rc;
    }
    // T2: D reverify (ct=13) — spawn Python
    if (int rc = stage_reverify(opts.run_id, opts, 13, "D"); rc != 0) {
        return rc;
    }
    // 3 包取样 (2026-10-25): 在 T4 之前给待判定行取额外样本。
    // 失败/跳过都只是降级 (T4 走 2 包路径), 不中断流水线。
    if (int rc = stage_3pkg_fetch(opts.run_id, opts); rc != 0) {
        return rc;
    }
    // T4: 变更判定 (ct=10) — C++ 实现
    if (int rc = stage_a_algorithm(opts.run_id, opts); rc != 0) {
        return rc;
    }

    log(opts.run_id, "=== done ===");
    return 0;
}

}  // namespace algo_diff_cpp
