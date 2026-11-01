// json_render.hpp — Python 数值渲染 (唯一需要手写的部分)
//
// 为什么这部分不能交给 JSON 库:
//   C++ 侧要与 Python 的 json.loads + str(value) 语义 **逐字节** 一致,
//   否则投影串不同, difflib 的 ratio 阈值判定就会与 Python 版分叉。
//   任何 C++ JSON 库都不提供 Python 的 repr() 格式规则:
//     1.0    -> "1.0"     (不是 "1")
//     1e16   -> "1e+16"
//     1e-05  -> "1e-05"
//     0.0001 -> "0.0001"  (不是 "1e-04")
//   短表示 + 指数切换阈值 + 尾随 ".0" 都是 Python 特有的。
//
// JSON 解析本身交给 simdjson 3.x (vendored), 不自己写解析器。
#pragma once

#include <cstdint>
#include <string>

namespace algo_diff_cpp {

// Python repr(float) 语义: 最短往返表示 + Python 的格式规则。
std::string python_float_repr(double v);

// Python str(int) 语义: 任意精度整数的十进制表示。
//   输入是 JSON 源码里的原始数字 token (例如 "1234567890123456789"),
//   Python 的 json.loads 会解析成任意精度 int, str() 原样输出该十进制。
//   负号与前导零不在 JSON 合法整数里, 原样保留即可。
std::string python_int_repr(const std::string& raw_number_token);

}  // namespace algo_diff_cpp
