// sqlite_io.hpp — SQLite C API 封装
//
// 模块职责: 4 个 stage 的 SELECT / UPDATE 操作, 不做算法判断.
//   退出码: 5 (db 操作失败).
//
// 必检 schema: web_subdomains + web_hashes + (web_subdomains_pre, _pre 不参与算法)
//   (T1/T2 reverify 由 lib/algo_diff_reverify.py 自己 verify, 本模块不参与.)
//
// 不在本模块:
//   - ReverifyResult struct (之前 mock jsonl 模式用, 现在删)
//   - load_reverify_results (mock jsonl 解析, 删)
//   - select_for_reverify (T1/T2 reverify SELECT, 移到 Python helper)
//
// 2026-10-24 扩展 (算法能力补齐):
//   - Row 增补 T4 新算法需要的字段: pre/cur content_type, learned_anchors,
//     以及 baseline 写回所需信息
//   - 新增批量接口: baseline upsert / field_length_stats upsert /
//     learned_anchors upsert, 全部走单个事务
//   - select_for_a_algorithm 支持按 id 游标分页 (batch), 不再一次性载入全表
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "anchors.hpp"
#include "mask.hpp"

namespace algo_diff_cpp {

struct Row {
    int64_t id;
    int8_t  ct;         // 0/1/2/4/6/8/10/12/13
    bool    curr_is_active;
    bool    pre_is_active_exists;
    bool    pre_is_active;
    std::vector<unsigned char> curr_body;
    std::string raw_json;
    // T4 A 算法新增字段:
    std::vector<unsigned char> pre_curr_body;
    std::string pre_web_hash;
    bool        pre_web_hash_exists;
    // 2026-10-12 fix: T4 Step 1 之前用 cur.web_hash 算 pre_total_hash
    // 是临时方案, 让 body change 永远判 body_identical. 现在 pre 跟 cur
    // 各自拿自己的 web_hash.
    std::string cur_web_hash;
    bool        cur_web_hash_exists;
    bool        baseline_exists;
    double      baseline_ratio;
    int32_t     pre_status_code;
    int64_t     pre_content_length;
    std::string pre_title;
    std::string pre_technologies;
    std::string pre_content_type;
    // T4 Step 1/2 需要: cur 端的快速字段 (否则 placeholder 让算法永远走 Step 3/4)
    int32_t     cur_status_code;
    int64_t     cur_content_length;
    std::string cur_title;
    std::string cur_technologies;
    std::string cur_content_type;
    // PATCH-FINAL-URL-2026-10-28: httpx 跟随重定向后的最终落点。
    // 喂给 STEP 1 快字段里那个一直没被用过的 redirect 槽位。
    // 2xx 资产为空串 -> 两边都空即相等 -> 自动跳过。
    std::string pre_final_url;
    std::string cur_final_url;

    // ---- 2026-10-24 新增: 算法能力补齐所需 ----
    // host 键 (subdomain:port), field_length_stats 的主键组成部分
    std::string host;
    // web_hashes.learned_anchors (JSON 字符串), 跨天累积的锚点
    std::string learned_anchors_json;
    // web_hashes.id (learned_anchors 的宿主行)
    int64_t     hash_id;

    // ---- 2026-10-25 新增: 三包样本 ----
    // curr_body_1 / curr_body_2: 同一时刻连发的 2 份 body (zlib 压缩 BLOB)。
    // 与 curr_body 构成 3 包, 用于区分"动态噪声"与"同长度真实变更"。
    std::vector<unsigned char> curr_body_1;
    std::vector<unsigned char> curr_body_2;
};

// 三包取样的目标行 (stage_3pkg_fetch 用)
struct ThreePkgTarget {
    int64_t     id;
    std::string url;        // httpx 输入
    std::string host_port;  // "host:port", 与 httpx 输出的 host/port 对齐
};

// 三包样本写回行
struct ThreePkgBodyRow {
    int64_t id;
    std::vector<unsigned char> body_1;
    std::vector<unsigned char> body_2;
};

struct UpdateRow {
    int64_t id;
    int8_t  new_ct;
    double  ratio;        // NaN if not from P 算法 (P fast path)
    std::string kind;     // empty if not set
    std::string at;       // ISO 8601, empty = use NOW()
};

// baseline 写回 (web_subdomain_baseline)
struct BaselineRow {
    int64_t subdomain_id;
    double  ratio;
};

// field_length_stats 写回
struct LengthStatRow {
    std::string host;
    std::string path;
    int64_t     length;
    int64_t     content_sample_hash;
};

// learned_anchors 写回 (web_hashes)
struct AnchorRow {
    int64_t     hash_id;
    std::string anchors_json;
};

// 可选表存在性 (verify_schema 填充):
//   field_length_stats / web_subdomain_baseline / web_hashes.learned_anchors
//   这三张表缺失时, 对应能力降级为"不生效", 但 pipeline 正常完成。
extern bool g_has_length_stats;
extern bool g_has_baseline;
extern bool g_has_learned_anchors;

// 验证 schema: 启动时调用, 失败返回 false + 写 error_msg.
bool verify_schema(const std::string& db_path, std::string& error_msg);

// T3 fast transition: SELECT ct=0 行
//   rows 包含 pre_is_active (LEFT JOIN web_subdomains_pre); '新行' = id 不在 pre。
bool select_for_fast_transition(const std::string& db_path,
                                std::vector<Row>& out,
                                std::string& error_msg);

// T4 A 算法: SELECT ct=10 行
//   after_id > 0 时只取 id > after_id 的行 (分页游标), LIMIT batch_size。
bool select_for_a_algorithm(const std::string& db_path,
                            std::vector<Row>& out,
                            std::string& error_msg,
                            int64_t after_id = 0,
                            int64_t batch_size = 0);

// 批量 UPDATE web_subdomains.change_type (T3 / T4 用)
bool update_change_type_batch(const std::string& db_path,
                              const std::vector<UpdateRow>& updates,
                              std::string& error_msg);

// 批量 UPSERT web_subdomain_baseline (T4 用)
//   与 Python compute.py 的 _write_back 一致: baseline_ratio 用滑动平均,
//   sample_count 递增。baseline 不存在则插入 (ratio, sample_count=1)。
bool upsert_baseline_batch(const std::string& db_path,
                            const std::vector<BaselineRow>& rows,
                            std::string& error_msg);

// 批量 UPSERT field_length_stats (T4 用)
//   长度漂移时 hit_count 重置为 1 (与 Python 一致)。
bool upsert_length_stats_batch(const std::string& db_path,
                               const std::vector<LengthStatRow>& rows,
                               std::string& error_msg);

// 批量 UPDATE web_hashes.learned_anchors (T4 用, 跨天累积)
bool update_learned_anchors_batch(const std::string& db_path,
                                  const std::vector<AnchorRow>& rows,
                                  std::string& error_msg);

// 批量 SELECT field_length_stats (按 host 取), 供 mask 层使用
bool select_length_stats(const std::string& db_path,
                          const std::string& host,
                          std::vector<LengthStat>& out,
                          std::string& error_msg);

// 批量 SELECT 多个 host 的 field_length_stats (一次查询, 避免 N+1)
// 三包取样: 查待判定的候选行 (ct=10 且 curr_body 非空)
bool select_three_pkg_targets(const std::string& db_path,
                               int business_id,
                               std::vector<ThreePkgTarget>& out,
                               std::string& error_msg);

// 三包取样: 批量写 curr_body_1 / curr_body_2
bool update_three_pkg_bodies(const std::string& db_path,
                             const std::vector<ThreePkgBodyRow>& rows,
                             std::string& error_msg);

bool select_length_stats_multi(const std::string& db_path,
                               const std::vector<std::string>& hosts,
                               std::vector<LengthStat>& out,
                               std::string& error_msg);

}  // namespace algo_diff_cpp
