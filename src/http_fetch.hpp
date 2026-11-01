// http_fetch.hpp — 批量 HTTP 取样 (2026-10-25)
//
// 用途: 3 包噪声提取方案需要在同一时刻对同一批 URL 连发 2 次, 拿到
// 两份独立 body 样本 (curr_body_1 / curr_body_2)。
//
// 批量约束 (用户拍板): httpx 必须在 while/for 循环之外调用 —— 每轮
// 一个 httpx 进程处理整批 URL (`httpx -l batch_file`), 不能对每个
// 子域单独 `echo url | httpx` (那本质上等价于 for 循环)。
//
// 实现:
//   - 一轮 = 一次 system() 调用 httpx, 输入是写好的 URL 列表文件
//   - 输出是 jsonl, 逐行用 simdjson 解析
//   - 2 轮 -> 两份样本, 按 (host, port) 归并

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace algo_diff_cpp {

// 一次取样的结果: (host:port) -> body
struct HttpFetchSample {
    std::map<std::string, std::string> by_hostport;
};

// 批量取一轮样本。
//   httpx_bin : httpx 可执行文件路径
//   urls      : 要取的 URL 列表 (整批)
//   resolvers : 可选 resolvers 文件
//   tmp_dir   : 临时目录 (写 URL 列表 / jsonl 输出)
//   out       : 取到的 body 样本
//   err       : 错误信息
// 返回 false 表示 httpx 调用失败 (调用方应降级, 不中断整条流水线)。
bool http_fetch_batch(const std::string& httpx_bin,
                      const std::vector<std::string>& urls,
                      const std::string& resolvers,
                      const std::string& tmp_dir,
                      HttpFetchSample& out,
                      std::string& err);

}  // namespace algo_diff_cpp
