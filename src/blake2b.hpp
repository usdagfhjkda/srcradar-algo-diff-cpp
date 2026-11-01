// blake2b.hpp — BLAKE2b 摘要 (仅 64-bit 输出)
//
// 用途: field_length_stats.content_sample_hash 的计算, 必须与 Python 的
// hashlib.blake2b(value, digest_size=8).digest() 逐字节一致 —— 该列与
// Python 版 algo_diff 共用, 哈希算法不同会让两边写入的 stat 行不可比。
#pragma once

#include <cstdint>
#include <string>

namespace algo_diff_cpp {

// 返回 8 字节 BLAKE2b 摘要 (digest_size=8), 与 Python
// hashlib.blake2b(data, digest_size=8).digest() 等价。
std::string blake2b_64(const std::string& data);

// 同上, 但返回 Python int.from_bytes(digest, "big", signed=True) 的语义:
// 8 字节大端有符号整数。
int64_t blake2b_64_signed_int(const std::string& data);

}  // namespace algo_diff_cpp
