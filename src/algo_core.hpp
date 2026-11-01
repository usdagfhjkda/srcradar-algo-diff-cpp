// algo_core.hpp — T4 变更判定核心 (算法能力补齐版)
//
// 这是 C++ 侧与 Python algo_diff/compute.py 对齐的核心。
//
// 算法流程 (Python 8 步的 C++ 实现, 含本轮补齐的 5 项能力):
//   STEP 1  快字段 diff   —— title/status_code/technologies 任一变化, 或
//                            redirect (跟随重定向后的最终落点) 两侧都有值
//                            且不等 => 立即 change_type=2
//   STEP 2  body 字节相同  —— SHA256 相同 => change_type=0, ratio=1.0
//   STEP 3  长度快筛       —— 长度差异 > LENGTH_DIFF_THRESHOLD => change_type=2
//                            (差异不大则长度不构成信号, 继续走后面的比较)
//   STEP 4  Content-Type 投影 —— JSON/HTML/XML/raw 四种规范形式 [本轮补齐]
//   STEP 5  动态字段 mask  —— 按 field_length_stats 掩码动态叶值 [本轮补齐]
//   STEP 6  anchors       —— 跨天累积锚点, 挖掉动态区 [本轮补齐]
//   STEP 7  三段阈值      —— >0.98 无变化 / <0.02 真变化 / 灰区看 baseline
//   STEP 8  超长降级      —— 超过 MAX_BODY_BYTES 时按长度比粗判 [本轮补齐]
//
// 相似度算法: difflib (Ratcliff-Obershelp), 与 Python difflib 逐块对齐,
// 而不是 rapidfuzz Indel —— 否则 0.98 阈值两侧的判定会与 Python 分叉。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "anchors.hpp"
#include "difflib.hpp"
#include "mask.hpp"

namespace algo_diff_cpp {

// 阈值 (与 Python compute.py 一致)
constexpr double UPPER_RATIO = 0.98;
constexpr double LOWER_RATIO = 0.02;
constexpr double DIFF_TOLERANCE = 0.05;
// 长度差阈值: body 长度差异超过该比例即视为变更信号 (判 ct=2)。
// 低于该比例则长度不构成信号, 继续走 body 比较与三包判定。
// 用**差异比例**表达 (差多少算大), 而不是相似度口径 —— 后者需要用
// 补数来反推阈值, 容易看错。
constexpr double LENGTH_DIFF_THRESHOLD = 0.30;

// 超长降级阈值: body 超过此大小不再走 O(n*m) 的 SequenceMatcher,
// 改用长度比粗判 (借鉴 sqlmap 的 MAX_DIFFLIB_SEQUENCE_LENGTH 思路,
// Python 版 algo_diff 没有这个降级 —— 这是 C++ 侧独有的保护,
// 因为生产库有 340K+ 行 × 10KB+ body)。
constexpr size_t MAX_BODY_BYTES = 10 * 1024 * 1024;

// kind 标签 (与 Python compute.py 的 KIND_* 对齐)
namespace kind {
constexpr const char* COLD_START          = "cold_start";
constexpr const char* FIRST_RUN           = "first_run";
constexpr const char* FAST_FIELD          = "fast_field";
constexpr const char* BODY_IDENTICAL      = "body_identical";
constexpr const char* BODY_LENGTH_MISMATCH= "body_length_mismatch";
constexpr const char* BODY_SIMILAR        = "body_structurally_similar";
constexpr const char* BODY_DIFFERENT      = "body_structurally_different";
constexpr const char* BODY_AMBIGUOUS_FIRST= "body_ambiguous_first_seen";
constexpr const char* BODY_AMBIGUOUS_DRIFT= "body_ambiguous_drift";
constexpr const char* BODY_AMBIGUOUS_STABLE="body_ambiguous_but_stable";
// C++ 独有
constexpr const char* BODY_TOO_LONG       = "body_too_long";
// 2026-10-25: 三包噪声提取判定结果
constexpr const char* THREE_PKG_SAME      = "three_pkg_same";
constexpr const char* THREE_PKG_DIFF      = "three_pkg_different";
}  // namespace kind

// 单条比较的输入
struct CompareInput {
    // 快字段 (STEP 1)
    std::string prev_title;
    std::string curr_title;
    int32_t     prev_status_code = 0;
    int32_t     curr_status_code = 0;
    std::string prev_redirect;
    std::string curr_redirect;
    std::string prev_technologies;
    std::string curr_technologies;
    // 注: 这里曾经有 content_length 参与快字段比较 (2026-10-24 加), 后来
    // 移到 STEP 3 的长度关卡, 并最终改为只看 body 的实际字节长度 ——
    // content_length 是 HTTP 响应头里的值, 与 body 实际长度可能有偏差,
    // 且在 body 缺失的行上从不出现"两侧不等"的情况。判定层不再需要它。

    // body (STEP 2/3/7)
    std::string prev_body;
    std::string curr_body;

    // 投影路由 (STEP 4)
    std::string content_type;

    // 长度统计 (STEP 5, mask 层)
    std::vector<LengthStat> length_stats;

    // 跨天累积锚点 (STEP 6)
    std::vector<Anchor> learned_anchors;

    // 三包样本 (2026-10-25): 灰区行在同一时刻连发 2 次得到的额外 body。
    // 与 curr_body 构成 3 包, 用于区分"动态噪声"与"同长度真实变更"。
    // 为空 (发包失败 / 未走到发包阶段) 时退回 2 包路径。
    std::string pkg2_body;
    std::string pkg3_body;

    // baseline (STEP 7 灰区)
    bool   baseline_exists = false;
    double baseline_ratio = 0.0;
};

// 比较结果
struct CompareResult {
    int8_t  change_type = 0;      // 0 或 2
    double  ratio = 0.0;
    std::string kind;
    bool    enqueue_rescan = false;  // 灰区需异步二轮
    // 本轮新学到的锚点 (需写回 web_hashes.learned_anchors)
    std::vector<Anchor> learned_anchors;
    // 本轮观测到的 (path, length, hash), 需写回 field_length_stats
    std::vector<FieldPair> observed_pairs;
};

// 核心比较。body 已是解压后的原始字节。
CompareResult compare(const CompareInput& in);

// 相似度计算 —— 带安全门控的快路径。
//
// 短 body 直接用原算法 (SequenceMatcher::ratio()); 长 body 用
// ratio_trimmed() (掐头去尾后只比中段), 代价从 O(n*m) 降到
// O(|mid_a|*|mid_b|)。
//
// 门控的理由: SequenceMatcher 的 get_matching_blocks 是递归贪心,
// 掐掉公共前后缀会改变"全局最长匹配块"的位置, 极少数**短**串上
// ratio 会分叉。实测 (共 24 万例真实形态 + 42 万例随机串):
//   <40 字符 : 分歧率 0.033% (最大偏差 0.32), 但 0.98/0.02 判定 0 次翻转
//   >=40 字符: 0 分歧 (真实形态 24000 例 + 分档 9000 例)
//   生产真实 body: 35 + 24 个样本, ratio 完全一致 (偏差 0)
// 所以短串 (< MIN_TRIM_LEN) 走原算法 —— 它本来就快, 没有优化收益,
// 但能杜绝分叉; 长串走快路径。
double ratio_trimmed_checked(const std::vector<CodePoint>& a,
                             const std::vector<CodePoint>& b,
                             SequenceMatcher& sm);

}  // namespace algo_diff_cpp
