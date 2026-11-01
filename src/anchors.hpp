// anchors.hpp — 动态区锚点发现与应用 (跨天累积)
//
// 移植自 Python algo_diff/mask.py 的 find_anchors / apply_anchors。
//
// 机制: 两个 body 的差异区被稳定的前后缀夹住。用 SequenceMatcher 的匹配块
// 找出稳定区, 相邻稳定块之间的空隙就是动态区。取动态区左边稳定块的尾部
// (prefix) 与右边稳定块的头部 (suffix) 作为锚点对, 下次访问时直接用锚点
// 把动态区挖掉, 不必重新做匹配块分析。
//
// 锚点是 **字符串切片** 而非字节偏移, 所以周边内容的小幅插入/重排不会让
// 锚点失效 —— 这是跨天累积能工作的前提。
//
// 跨天累积: 锚点存在 web_hashes.learned_anchors 列 (JSON 数组
// [[prefix, suffix], ...]), 下次比较时先读 learned_anchors; 没有才现场
// find_anchors, 并把新学到的写回该列。
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace algo_diff_cpp {

// 锚点对 (prefix, suffix)
using Anchor = std::pair<std::string, std::string>;

// 锚点长度阈值 (与 Python 一致)
constexpr size_t ANCHOR_MIN_LENGTH = 20;
constexpr size_t ANCHOR_TAIL_LENGTH = 20;

// 从 first/second 的差异中找出稳定锚点对。
//   过滤掉长度 <= 2 * ANCHOR_MIN_LENGTH 的匹配块;
//   少于两个稳定块时返回空 (无法夹住动态区)。
std::vector<Anchor> find_anchors(const std::string& first,
                                  const std::string& second);

// 用锚点把动态区从 content 中挖掉, 只留稳定的前缀+后缀片段。
std::string apply_anchors(const std::string& content,
                           const std::vector<Anchor>& anchors);

// anchors 的 JSON 序列化 / 反序列化 (存 web_hashes.learned_anchors 列)。
//   格式: [[prefix, suffix], ...]
std::string anchors_to_json(const std::vector<Anchor>& anchors);
std::vector<Anchor> anchors_from_json(const std::string& json);

}  // namespace algo_diff_cpp
