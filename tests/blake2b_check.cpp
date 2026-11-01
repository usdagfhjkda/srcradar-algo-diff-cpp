// blake2b_check.cpp — 验证 C++ blake2b_64 与 Python hashlib.blake2b(d, digest_size=8) 一致。
//
// 用法: ./blake2b_check <case_file>
//   case_file 每行一个 base64 编码的待哈希文本。
//   输出: 每行 "<index>\t<hex>"。
//   python 侧用 hashlib 算同样的值, 逐行 diff。
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "blake2b.hpp"

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

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <case_file>\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1]);
    if (!in) return 2;

    std::string line;
    int idx = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // "B64 " 前缀: 空串用例编码后是空行, 前缀让索引不错位
        if (line.compare(0, 4, "B64 ") != 0) continue;
        std::string data = b64_decode(line.substr(4));
        std::string digest = algo_diff_cpp::blake2b_64(data);
        std::string hex;
        char buf[4];
        for (unsigned char c : digest) {
            std::snprintf(buf, sizeof(buf), "%02x", c);
            hex += buf;
        }
        std::printf("%d\t%s\n", idx, hex.c_str());
        ++idx;
    }
    return 0;
}
