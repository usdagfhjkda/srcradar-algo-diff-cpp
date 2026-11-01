// sequence_matcher.cpp — P5 implementation: rapidfuzz::fuzz::ratio (字符级)
//
// 跟 compute.py 的 difflib.SequenceMatcher.ratio() 行为接近:
//   - rapidfuzz::fuzz::ratio 内部是 Indel (Levenshtein-like) 算法
//   - Python difflib.SequenceMatcher.ratio() 是 Ratcliff-Obershelp
//   - 数值范围不同: rapidfuzz 返回 [0, 100], SequenceMatcher 返回 [0, 1]
//   - 本实现除以 100, 跟 compute.py 行为对齐 (返回值 ∈ [0.0, 1.0])
//
// 注: rapidfuzz 跟 SequenceMatcher 不完全等价, 数值接近, 不完全相同.
//
// 输入: pre_body bytes (zlib inflated), cur_body bytes (zlib inflated)
// 输出: double ∈ [0.0, 1.0], 1.0 = 完全相同
//
// fail-fast 策略 (commit 9b79758 决策): zlib / json inflate 失败, 不容错, exit 4.

#include "sequence_matcher.hpp"
#include <rapidfuzz/fuzz.hpp>
#include <string>
#include <string_view>

namespace algo_diff_cpp {

double compute_ratio(const std::vector<unsigned char>& prev,
                     const std::vector<unsigned char>& curr) {
    if (prev.empty() && curr.empty()) return 1.0;
    if (prev.empty() || curr.empty()) return 0.0;

    std::string_view prev_sv(reinterpret_cast<const char*>(prev.data()),
                             prev.size());
    std::string_view curr_sv(reinterpret_cast<const char*>(curr.data()),
                             curr.size());
    // rapidfuzz 返回 [0, 100], 除以 100 跟 compute.py 对齐
    return rapidfuzz::fuzz::ratio(prev_sv, curr_sv) / 100.0;
}

}  // namespace algo_diff_cpp
