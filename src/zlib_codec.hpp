// zlib_codec.hpp — zlib inflate / deflate wrapper
//
// 用途:
//   - inflate(web_subdomains.curr_body)  -> bytes (HTTP body)
//   - deflate(body)                      -> bytes (写入 BLOB)
//   - 失败: 按 commit 9b79758 + 2026-10-08 算法 v3 用户拍板, 直接 exit (fail-fast)
//     不容错 (vs Python 的 zlib.decompress 抛 zlib.error 触发 try/except)。
//
// 协议:
//   - 输入是 zlib format (0x78 0x9C header for level=6), 不是 raw deflate / not gzip。
//   - 与 Python zlib.decompress(level=6 zlib.compress()) byte-compatible。

#pragma once

#include <string>
#include <vector>

namespace algo_diff_cpp {

// inflate 一个 zlib stream, 成功返回 true。
//   in:  zlib compressed bytes (BLOB from sqlite)
//   out: 解压后的 bytes (HTTP body)
//
// 失败时:
//   - 返回 false
//   - out 留空
//   - error_msg 写 libz 错误信息 (zError(rc))
//
// 调用方处理: pipeline.cpp 检测到 false -> exit(4) (按 README 退出码约定)。
bool inflate_zlib(const std::vector<unsigned char>& in,
                  std::vector<unsigned char>& out,
                  std::string& error_msg);

// deflate 一段 bytes 成 zlib stream (level=6, 与 Python zlib.compress(body, 6) 兼容)。
// 成功返回 true; 失败返回 false 并写 error_msg。
bool deflate_zlib(const std::vector<unsigned char>& in,
                  std::vector<unsigned char>& out,
                  std::string& error_msg);

}  // namespace algo_diff_cpp
