// json_render.cpp — Python 数值渲染实现
#include "json_render.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace algo_diff_cpp {

std::string python_float_repr(double v) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";

    // Python repr 用 **最短往返** 表示: 找最小的有效位数使得解析回去
    // 等于原值。先用 %.17g 保证一定能往返, 再逐步降精度试探。
    //
    // 关键: 必须拿到 **最短** 那档的十进制数字串和指数, 而不是 %.17g 的
    // (0.1 在 %.17g 下是 0.10000000000000001, Python repr 是 0.1)。
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    std::string chosen(buf);

    for (int prec = 1; prec <= 17; ++prec) {
        char probe[64];
        std::snprintf(probe, sizeof(probe), "%.*g", prec, v);
        if (std::strtod(probe, nullptr) == v) {
            chosen = probe;
            break;
        }
    }

    // 拆成 (数字串, 十进制指数)
    size_t epos = chosen.find_first_of("eE");
    std::string digits = (epos == std::string::npos) ? chosen : chosen.substr(0, epos);
    int exp = 0;
    if (epos != std::string::npos) {
        exp = std::atoi(chosen.c_str() + epos + 1);
    }
    std::string mant;
    for (char c : digits) {
        if (c != '.') mant.push_back(c);
    }
    // 去前导零: 0.00123 -> mant="000123" -> "123", exp -= 3
    size_t nz = mant.find_first_not_of('0');
    if (nz == std::string::npos) {
        mant = "0";
        exp = 0;
    } else if (nz > 0) {
        exp -= static_cast<int>(nz);
        mant = mant.substr(nz);
    }
    // 去尾随零: 1.5000 -> "15" (指数形式下 %.Ng 已给出规范化数字)
    if (epos == std::string::npos) {
        size_t last = mant.find_last_not_of('0');
        if (last == std::string::npos) {
            mant = "0";
        } else {
            mant = mant.substr(0, last + 1);
        }
    }

    // 负号单独处理 (Python repr(-0.0) == "-0.0", repr(-2.5) == "-2.5")
    bool neg = false;
    if (!mant.empty() && mant[0] == '-') {
        neg = true;
        mant = mant.substr(1);
    }

    const int ndigits = static_cast<int>(mant.size());
    // 数值 = 0.mant * 10^decpt, decpt = exp + 1
    const int decpt = exp + 1;

    std::string out;
    // Python 的固定/指数切换规则: decpt <= -4 或 decpt > 16 用指数记法
    if (decpt <= -4 || decpt > 16) {
        out.push_back(mant[0]);
        if (ndigits > 1) {
            out.push_back('.');
            out.append(mant, 1, std::string::npos);
        }
        out.push_back('e');
        int e10 = decpt - 1;
        if (e10 < 0) {
            out.push_back('-');
            e10 = -e10;
        } else {
            out.push_back('+');
        }
        char eb[16];
        std::snprintf(eb, sizeof(eb), "%02d", e10);
        out.append(eb);
    } else if (decpt <= 0) {
        out = "0.";
        out.append(static_cast<size_t>(-decpt), '0');
        out.append(mant);
    } else if (decpt >= ndigits) {
        out = mant;
        out.append(static_cast<size_t>(decpt - ndigits), '0');
        out += ".0";
    } else {
        out = mant.substr(0, static_cast<size_t>(decpt));
        out.push_back('.');
        out.append(mant, static_cast<size_t>(decpt), std::string::npos);
    }

    if (neg) out.insert(out.begin(), '-');
    return out;
}

std::string python_int_repr(const std::string& raw_number_token) {
    // JSON 整数 token 原样就是 Python 任意精度 int 的十进制表示。
    // 唯一需要处理的是 "-0": Python json.loads("-0") -> int 0 -> str "0"
    if (raw_number_token == "-0") return "0";
    return raw_number_token;
}

}  // namespace algo_diff_cpp
