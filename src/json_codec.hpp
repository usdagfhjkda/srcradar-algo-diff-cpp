// json_codec.hpp — simdjson 提取 _pre.raw_json 的 body 字段
//
// 用途:
//   T4 A 算法需要 prev_body bytes。当前 db 真实存储 = _pre.raw_json (httpx JSONL
//   容器, 含 body 子串)。我们用 simdjson::ondemand 提取 body 字段的 bytes, 与
//   web_subdomains.curr_body (zlib 解出) 对比。
//
// 为什么不直接 zlib 解 web_subdomains.prev_body?
//   - 当前生产 9/7 schema 没有 prev_body 列 (10/4 commit 4dc5956 才加)
//   - _pre 表 schema 镜像 web_subdomains, 也没 prev_body
//   - "prev body" 语义在 _pre.raw_json 里 (是 "上次 cron 跑完时的 body 字段")
//   - 因此: prev = simdjson(_pre.raw_json) → 取 body 字段

#pragma once

#include <string>
#include <vector>

namespace algo_diff_cpp {

// 从 httpx JSON 容器 (raw_json column) 提取 body 字段 bytes。
//   raw_json: 数据库存的 JSON 文本 (TEXT)
//   body_out: 提取的 body bytes (escape " \n" 等)
//
// 失败 (JSON parse / body 字段不存在 / 不是 string):
//   - 返回 false
//   - body_out 留空
//   - error_msg 写原因
//
// 注意: 这是 A 算法的 prev 来源。失败时 T4 无法跑 body compare,
//   caller (ct=10 行) 处理 = exit 4 让 scheduler 决定。
bool extract_body_from_raw_json(const std::string& raw_json,
                                std::vector<unsigned char>& body_out,
                                std::string& error_msg);

}  // namespace algo_diff_cpp