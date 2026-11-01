// pipeline.hpp — 主流程 orchestration
//
// 4 stage (顺序锁定, 算法 v9):
//   T1. C reverify (ct=12)     → spawn Python algo_diff_reverify.py 跑 T1
//   T2. D reverify (ct=13)     → spawn Python algo_diff_reverify.py 跑 T2
//   T3. fast transition (ct=0) → 4 分支, 1/0/10/12 写回 (C++ 实现)
//   T4. P 算法 (ct=10)         → SequenceMatcher + (1-pre.is_active)*4 (C++ 实现, 当前 stub)
//
// T1/T2 reverify 调用: 调 lib/algo_diff_reverify.py (Python 封装),
//   该 Python 脚本自己读 pdtm.conf 拿 httpx_bin + resolvers_file, popen httpx,
//   parse jsonl, upsert_web_hash, 批量 UPDATE web_subdomains 全部字段 + ct.
//   C++ 端只负责 spawn Python + 等 exit + log.
//
// 退出:
//   0 = OK
//   3 = schema verify failed
//   4 = zlib/json inflate 失败 (T4 stub 时未用, commit 9b79758 决策: 直接 exit, 不容错)
//   5 = batch update 失败 (T3/T4 写 db 失败)
//   6 = Python helper 失败 (T1/T2 subprocess 非 0 退出)

#pragma once

#include <string>

namespace algo_diff_cpp {

struct PipelineOptions {
    std::string db_path;
    std::string run_id;        // 用于日志/stamp (e.g. 20261008-030001)
    int business_id = 0;       // 0 = 全表, >0 = 单业务
    bool dry_run = false;      // 只 print + 不写 db
    bool skip_reverify = false; // 测试: 跳过 T1/T2 reverify (test only)
    std::string reverify_helper_path = "../lib/algo_diff_reverify.py";
                                // 相对 binary 位置. lib/run_algo_diff.sh 调 C++
                                // 时 CWD = <module>/bin/, 所以 ../lib/ 解析成
                                // <module>/lib/. 也可用 --reverify-helper-path 显式覆盖.
    std::string httpx_path = "httpx";  // httpx binary 路径 (3 包方案直接调它)
    // 3 包方案 (2026-10-25): 同一时刻连发 2 次取额外 body 样本。
    //   - resolvers: 可选, 空 = 不传 -r
    //   - fetch_samples: 连发次数 (0 = 不做 3 包, 走原 2 包路径)
    //   - tmp_dir: 临时目录 (URL 列表 / jsonl 输出)
    std::string resolvers_path = "";
    int         fetch_samples = 2;
    std::string tmp_dir = "/tmp";
    size_t batch_size = 5000;  // UPDATE batch 尺寸
};

// 主入口. 返回 exit_code, 任何阶段出错都打 stderr + return 非 0.
int run_pipeline(const PipelineOptions& opts);

}  // namespace algo_diff_cpp