// jsondiff_test.cpp — C++ (vendored simdjson 3.9.5) 与 Python json.loads
// 的差分测试工具。
//
// 关键设计: 本测试 **直接调用生产代码** src/json_walk.cpp 的 json_pairs(),
// 而不是在测试文件里复制一份实现 —— 否则测的是副本, 不是产品代码。
//
// 用法: ./jsondiff_test <case_file>
//   case_file 每行一个 base64 编码的 JSON 文本。
//   输出: 每行 "<case_index>\t<OK|PARSE_FAIL>\t<rendered>", rendered 里
//   换行用 \x1f 分隔。python 侧 jsondiff_ref.py 输出格式完全一致。
#include <simdjson.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "json_walk.hpp"

using namespace algo_diff_cpp;

// base64 decode (测试工具用, 无外部依赖)
static std::string b64_decode(const std::string& in) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<int> rev(256, -1);
    for (int i = 0; i < 64; ++i) rev[static_cast<unsigned char>(tbl[i])] = i;
    std::string out;
    int buf = 0, bits = 0;
    for (unsigned char c : in) {
        if (c == '=') break;
        int v = rev[c];
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// 独立的状态探测: 用 vendored simdjson 的 DOM 严格解析, 判断这份输入
// 到底能不能当 JSON 解析。这只是给差分日志用的状态标记, 不参与判定。
static bool parse_ok(const std::string& text) {
    // 复用生产实现的空/空白判定, 其余交给 DOM 解析
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    return !parser.parse(text.c_str(), text.size()).get(doc);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <case_file>\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1]);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    std::string line;
    int idx = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::string json_text = b64_decode(line);

        // 直接调用生产实现
        std::string rendered = json_project(json_text);
        std::string status = parse_ok(json_text) ? "OK" : "PARSE_FAIL";

        std::string flat;
        for (char c : rendered) {
            if (c == '\n') flat += "\x1f";
            else flat.push_back(c);
        }
        std::printf("%d\t%s\t%s\n", idx, status.c_str(), flat.c_str());
        ++idx;
    }
    return 0;
}
