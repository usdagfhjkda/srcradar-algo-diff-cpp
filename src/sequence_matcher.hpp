// sequence_matcher.hpp — rapidfuzz wrapper
//
// 这是 P5 stub: A 算法 (T4) 的 SequenceMatcher 接口, 等 P5 才实现。
// 本阶段写 skeleton + 单一接口 compute_ratio, pipeline.cpp 调用时不报错。
//
// 接口设计:
//   - 输入: prev_body bytes, curr_body bytes (均已 inflate 解压)
//   - 输出: ratio ∈ [0.0, 1.0]
//   - P5 实现: rapidfuzz::fuzz::Ratio (C++ Ratcliff-Obershelp)
//   - 当前 stub: 抛 std::runtime_error (pipeline 检测, exit 4)
//
// 注意: T1/T2 reverify 路径不用 SequenceMatcher, 只 T4 用。

#pragma once

#include <vector>

namespace algo_diff_cpp {

// compute SequenceMatcher.ratio() between prev and curr bytes.
// 返回 ∈ [0.0, 1.0] (1.0 = 完全相同, 0.0 = 无共同 block)。
//
// P5 实现: rapidfuzz::fuzz::Ratio 同步版本, 与 Python difflib.SequenceMatcher
//   在 Ratcliff-Obershelp 算法上同结果 (Python 的 quick_ratio() 更快但不等价)。
//
// P3 stub: throw runtime_error (P5 才填)。
double compute_ratio(const std::vector<unsigned char>& prev,
                     const std::vector<unsigned char>& curr);

}  // namespace algo_diff_cpp