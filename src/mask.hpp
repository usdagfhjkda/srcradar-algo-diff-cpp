// mask.hpp — 动态字段掩码 (按长度统计学习)
//
// 移植自 Python algo_diff/mask.py 的 mask_dynamic_fields / _should_mask。
//
// 机制: field_length_stats 表按 (host, path) 记录每个叶值的观测长度与
// 命中次数。当某个 path 的长度在多次访问间稳定 (hit_count >= 3) 且当前
// 观测长度与记录一致时, 该叶值被视为动态字段, 替换成固定 token
// __MASKED__, 让两侧 body 折叠到同一形态, ratio 才能拉到 1.0。
//
// 与 Python 版的顺序: mask 在 project 之前 (mask 改原 body, project 再把
// 改过的 body 投影成结构串)。C++ 版沿用同一顺序。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "projection.hpp"

namespace algo_diff_cpp {

// 单条长度统计 (对应 field_length_stats 一行)
struct LengthStat {
    std::string host;
    std::string path;
    int64_t length = 0;
    int64_t hit_count = 0;
    int64_t content_sample_hash = 0;
};

// 掩码阈值 (与 Python 一致)
constexpr int64_t MASK_THRESHOLD = 3;
constexpr size_t MAX_MASKABLE_LENGTH = 64;
extern const char* const MASKED_TOKEN;

// 判断某个叶值是否应被掩码。三条条件全满足才掩:
//   1. byte 长度 <= MAX_MASKABLE_LENGTH
//   2. stat.hit_count >= MASK_THRESHOLD
//   3. stat.length == 当前值长度 (长度未漂移)
bool should_mask(const std::string& value, const LengthStat& stat);

// 把 body 中命中掩码条件的叶值替换成 MASKED_TOKEN。
//   用单次左到右扫描 + 游标, 保证不重复匹配已改写过的位置。
std::string mask_dynamic_fields(const std::string& body,
                                 const std::string& content_type,
                                 const std::vector<LengthStat>& stats_rows);

}  // namespace algo_diff_cpp
