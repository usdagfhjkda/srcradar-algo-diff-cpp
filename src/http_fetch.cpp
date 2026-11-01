// http_fetch.cpp — 批量 HTTP 取样实现

#include "http_fetch.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "simdjson.h"

namespace algo_diff_cpp {

namespace {

// shell 单引号包裹, 防路径里的空格/特殊字符
std::string sh_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

}  // namespace

// 把一个 URL 归一化成 host:port (去 scheme / 去 path / 去结尾斜杠)。
// 必须与 sqlite_io.cpp 里 select_three_pkg_targets 的 SQL 归一化规则一致。
std::string normalize_hostport(const std::string& url) {
    std::string s = url;
    size_t p = s.find("://");
    if (p != std::string::npos) s = s.substr(p + 3);
    size_t slash = s.find('/');
    if (slash != std::string::npos) s = s.substr(0, slash);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

bool http_fetch_batch(const std::string& httpx_bin,
                      const std::vector<std::string>& urls,
                      const std::string& resolvers,
                      const std::string& tmp_dir,
                      HttpFetchSample& out,
                      std::string& err) {
    out.by_hostport.clear();
    if (urls.empty()) return true;

    const std::string in_path = tmp_dir + "/3pkg_urls.txt";
    const std::string out_path = tmp_dir + "/3pkg_out.jsonl";

    // 写 URL 列表 (整批)
    {
        std::ofstream f(in_path, std::ios::binary | std::ios::trunc);
        if (!f) {
            err = "cannot write " + in_path;
            return false;
        }
        for (const std::string& u : urls) {
            f << u << "\n";
        }
    }

    // 一轮 = 一次 httpx 进程 (整批), 不在循环里逐个调
    //
    // URL 列表走 **stdin** 而不是 "-l <file>": 本机 httpx 二进制
    // (pdtm.conf 的 httpx_bin) 在 "-l" 模式下不返回任何输出 (实测 0 行),
    // 同一二进制的 stdin 模式正常。走 "-l" 会让三包静默取不到样本。
    // 两种模式 httpx 都支持, stdin 是标准用法。
    std::ostringstream cmd;
    cmd << sh_quote(httpx_bin)
        << " -sc -cl -title -hash mmh3,simhash"
        << " -random-agent -follow-redirects -max-redirects 3"
        << " -td -rate-limit 50 -timeout 5"
        << " -silent -no-color -json -irr";
    if (!resolvers.empty()) {
        cmd << " -r " << sh_quote(resolvers);
    }
    cmd << " < " << sh_quote(in_path)
        << " > " << sh_quote(out_path) << " 2>/dev/null";

    int rc = std::system(cmd.str().c_str());
    if (rc == -1) {
        err = "httpx spawn failed";
        return false;
    }

    // 解析 jsonl
    std::ifstream fin(out_path, std::ios::binary);
    if (!fin) {
        err = "cannot read " + out_path;
        return false;
    }
    simdjson::dom::parser parser;
    std::string line;
    while (std::getline(fin, line)) {
        if (line.empty()) continue;
        simdjson::dom::element doc;
        auto error = parser.parse(line.c_str(), line.size()).get(doc);
        if (error) continue;

        // failed 的记录没有 body
        bool failed = false;
        if (doc["failed"].is_bool()) {
            failed = doc["failed"].get_bool().value();
        }
        if (failed) continue;

        std::string host;
        if (doc["host"].is_string()) {
            host = std::string(doc["host"].get_string().value());
        }
        if (host.empty()) continue;

        int64_t port = 0;
        if (doc["port"].is_int64()) {
            port = doc["port"].get_int64();
        } else if (doc["port"].is_uint64()) {
            port = static_cast<int64_t>(doc["port"].get_uint64());
        } else if (doc["port"].is_string()) {
            port = std::atoll(std::string(doc["port"].get_string().value()).c_str());
        }

        std::string body;
        if (doc["body"].is_string()) {
            body = std::string(doc["body"].get_string().value());
        } else {
            continue;
        }

        // 匹配键: httpx 的 input 字段 (我们传进去的 url 原文) 归一化成
        // host:port —— 必须与 select_three_pkg_targets 的 url 归一化一致。
        // 用 input 而不是 host: 后者会因重定向变成别的地址。
        std::string key;
        if (doc["input"].is_string()) {
            key = normalize_hostport(std::string(doc["input"].get_string().value()));
        }
        if (key.empty()) {
            key = host + ":" + std::to_string(port);
        }
        out.by_hostport[key] = body;
    }
    return true;
}

}  // namespace algo_diff_cpp
