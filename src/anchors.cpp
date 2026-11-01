// anchors.cpp — 动态区锚点发现与应用实现
//
// 移植自 Python algo_diff/mask.py 的 find_anchors / apply_anchors。
//
// 一个重要细节: Python 版在 **str (code point)** 空间工作, 而 body 是
// UTF-8 字节。中文页面里一个汉字 3 字节, 若在字节空间切锚点, 边界可能
// 落在多字节字符中间, 与 Python 切出的锚点不一致。因此本模块内部一律
// 解码成 code point 序列再操作, 边界处再编码回 UTF-8。
#include "anchors.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "difflib.hpp"

namespace algo_diff_cpp {

namespace {

std::string encode_utf8(const std::vector<CodePoint>& cps) {
    std::string out;
    out.reserve(cps.size());
    for (CodePoint c : cps) {
        uint32_t v = static_cast<uint32_t>(c);
        if (v < 0x80) {
            out.push_back(static_cast<char>(v));
        } else if (v < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (v >> 6)));
            out.push_back(static_cast<char>(0x80 | (v & 0x3F)));
        } else if (v < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (v >> 12)));
            out.push_back(static_cast<char>(0x80 | ((v >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (v & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (v >> 18)));
            out.push_back(static_cast<char>(0x80 | ((v >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((v >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (v & 0x3F)));
        }
    }
    return out;
}

// 在 haystack 的 [from, haystack.size()) 区间内查找 needle 的首次出现。
// 返回 haystack.size() 表示未找到。
size_t find_from(const std::vector<CodePoint>& haystack,
                 const std::vector<CodePoint>& needle,
                 size_t from) {
    if (needle.empty()) return from;
    if (needle.size() > haystack.size()) return haystack.size();
    const size_t limit = haystack.size() - needle.size();
    for (size_t i = from; i <= limit; ++i) {
        if (std::equal(needle.begin(), needle.end(), haystack.begin() + i)) {
            return i;
        }
    }
    return haystack.size();
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

}  // namespace

std::vector<Anchor> find_anchors(const std::string& first,
                                  const std::string& second) {
    std::vector<Anchor> anchors;
    if (first.empty() || second.empty()) return anchors;

    std::vector<CodePoint> a = decode_utf8(first);
    std::vector<CodePoint> b = decode_utf8(second);

    // Python: SequenceMatcher(None, first, second) —— autojunk 默认 True
    SequenceMatcher sm(a, b, /*autojunk=*/true);
    std::vector<Match> blocks = sm.get_matching_blocks();

    // 过滤过短的匹配块 (Python: b.size > 2 * ANCHOR_MIN_LENGTH)
    std::vector<Match> kept;
    for (const Match& m : blocks) {
        if (m.size > 2 * ANCHOR_MIN_LENGTH) kept.push_back(m);
    }
    if (kept.size() < 2) return anchors;  // 需要至少两个稳定块才能夹住动态区

    // 前后各加一个哨兵 (0,0,0), 与 Python extended 数组一致
    Match sentinel;
    sentinel.a = sentinel.b = sentinel.size = 0;
    std::vector<Match> extended;
    extended.push_back(sentinel);
    for (const Match& m : kept) extended.push_back(m);
    extended.push_back(sentinel);

    for (size_t i = 0; i + 1 < extended.size(); ++i) {
        const Match& prev_b = extended[i];
        const Match& next_b = extended[i + 1];
        if (prev_b.size == 0 && next_b.size == 0) continue;

        std::vector<CodePoint> prefix_cp;
        std::vector<CodePoint> suffix_cp;
        bool have_prefix = false;
        bool have_suffix = false;

        if (prev_b.size > 0) {
            size_t tail_start = prev_b.a + (prev_b.size > ANCHOR_TAIL_LENGTH
                                                ? prev_b.size - ANCHOR_TAIL_LENGTH
                                                : 0);
            prefix_cp.assign(a.begin() + tail_start,
                             a.begin() + prev_b.a + prev_b.size);
            have_prefix = true;
        }
        if (next_b.size > 0) {
            size_t head_len = std::min(ANCHOR_TAIL_LENGTH, next_b.size);
            suffix_cp.assign(a.begin() + next_b.a,
                             a.begin() + next_b.a + head_len);
            have_suffix = true;
        }

        // 边缘括号: 前面没有稳定块且下一个块从 0 开始 -> 跳过
        if (!have_prefix && next_b.a == 0) continue;
        // 后面没有稳定块且当前块已到末尾 -> 跳过
        if (!have_suffix && (prev_b.a + prev_b.size >= a.size())) continue;

        if (have_prefix && have_suffix) {
            Anchor pair;
            pair.first = encode_utf8(prefix_cp);
            pair.second = encode_utf8(suffix_cp);
            if (std::find(anchors.begin(), anchors.end(), pair) == anchors.end()) {
                anchors.push_back(pair);
            }
        }
    }
    return anchors;
}

std::string apply_anchors(const std::string& content,
                           const std::vector<Anchor>& anchors) {
    if (content.empty() || anchors.empty()) return content;

    std::vector<CodePoint> cps = decode_utf8(content);

    for (const Anchor& anchor : anchors) {
        const std::string& p_str = anchor.first;
        const std::string& s_str = anchor.second;
        if (p_str.empty() && s_str.empty()) continue;

        std::vector<CodePoint> prefix = decode_utf8(p_str);
        std::vector<CodePoint> suffix = decode_utf8(s_str);

        if (!prefix.empty() && !suffix.empty()) {
            // Python: re.sub(re.escape(prefix) + ".*?" + re.escape(suffix),
            //               prefix + suffix, content, flags=re.DOTALL)
            // 语义: 逐个匹配 prefix...suffix 的最短跨度, 替换为 prefix+suffix。
            std::vector<CodePoint> out;
            size_t pos = 0;
            bool replaced_any = false;
            while (pos <= cps.size()) {
                size_t p = find_from(cps, prefix, pos);
                if (p == cps.size()) break;
                size_t s = find_from(cps, suffix, p + prefix.size());
                if (s == cps.size()) {
                    // 该 prefix 之后没有 suffix: Python 正则会继续尝试下一个位置
                    pos = p + 1;
                    continue;
                }
                out.insert(out.end(), cps.begin() + pos, cps.begin() + p);
                out.insert(out.end(), prefix.begin(), prefix.end());
                out.insert(out.end(), suffix.begin(), suffix.end());
                pos = s + suffix.size();
                replaced_any = true;
            }
            if (replaced_any) {
                out.insert(out.end(), cps.begin() + pos, cps.end());
                cps.swap(out);
            }
        } else if (!prefix.empty()) {
            // Python: re.sub(re.escape(prefix) + ".*$", prefix, content, DOTALL)
            // 语义: 首次出现 prefix 之后到结尾全部替换为 prefix。
            size_t p = find_from(cps, prefix, 0);
            if (p != cps.size()) {
                cps.resize(p);
                cps.insert(cps.end(), prefix.begin(), prefix.end());
            }
        } else {
            // Python: re.sub(r"^.*?" + re.escape(suffix), suffix, content, DOTALL)
            // 语义: 从开头到首次 suffix 出现处, 替换为 suffix。
            size_t s = find_from(cps, suffix, 0);
            if (s != cps.size()) {
                std::vector<CodePoint> out;
                out.insert(out.end(), suffix.begin(), suffix.end());
                out.insert(out.end(), cps.begin() + s + suffix.size(), cps.end());
                cps.swap(out);
            }
        }
    }
    return encode_utf8(cps);
}

std::string anchors_to_json(const std::vector<Anchor>& anchors) {
    std::string out = "[";
    for (size_t i = 0; i < anchors.size(); ++i) {
        if (i) out += ",";
        out += "[\"" + json_escape(anchors[i].first) + "\",\"" +
               json_escape(anchors[i].second) + "\"]";
    }
    out += "]";
    return out;
}

std::vector<Anchor> anchors_from_json(const std::string& json) {
    std::vector<Anchor> out;
    if (json.empty()) return out;
    size_t i = 0;
    const size_t n = json.size();
    auto skip_ws = [&]() {
        while (i < n && (json[i] == ' ' || json[i] == '\t' ||
                         json[i] == '\n' || json[i] == '\r')) ++i;
    };
    skip_ws();
    if (i >= n || json[i] != '[') return out;
    ++i;
    while (true) {
        skip_ws();
        if (i >= n) break;
        if (json[i] == ']') break;
        if (json[i] != '[') break;
        ++i;
        std::string parts[2];
        for (int k = 0; k < 2; ++k) {
            skip_ws();
            if (i < n && json[i] == '"') {
                ++i;
                std::string cur;
                while (i < n && json[i] != '"') {
                    if (json[i] == '\\' && i + 1 < n) {
                        ++i;
                        char c = json[i];
                        switch (c) {
                            case 'n': cur.push_back('\n'); break;
                            case 't': cur.push_back('\t'); break;
                            case 'r': cur.push_back('\r'); break;
                            case 'b': cur.push_back('\b'); break;
                            case 'f': cur.push_back('\f'); break;
                            case 'u': {
                                if (i + 4 < n) {
                                    std::string hex = json.substr(i + 1, 4);
                                    int cp = std::stoi(hex, nullptr, 16);
                                    i += 4;
                                    if (cp < 0x80) {
                                        cur.push_back(static_cast<char>(cp));
                                    } else if (cp < 0x800) {
                                        cur.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                                        cur.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                                    } else {
                                        cur.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                                        cur.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                                        cur.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                                    }
                                }
                                break;
                            }
                            default: cur.push_back(c);
                        }
                        ++i;
                    } else {
                        cur.push_back(json[i]);
                        ++i;
                    }
                }
                if (i < n) ++i;  // 跳过收尾引号
                parts[k] = cur;
            }
            skip_ws();
            if (i < n && json[i] == ',') ++i;
        }
        skip_ws();
        if (i < n && json[i] == ']') ++i;
        out.push_back(Anchor(parts[0], parts[1]));
        skip_ws();
        if (i < n && json[i] == ',') {
            ++i;
            continue;
        }
        break;
    }
    return out;
}

}  // namespace algo_diff_cpp
