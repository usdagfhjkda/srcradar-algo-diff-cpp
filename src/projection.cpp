// projection.cpp — Content-Type 路由 + 结构化投影实现
//
// 移植自 Python algo_diff/mask.py, 目标是同一输入产出逐字节一致的投影串。
//
// JSON 解析用 simdjson (项目已有依赖), 不引新库。
// HTML/XML 扫描器手写而不用 std::regex: POSIX ERE 与 Python re 在
//   (?is) 内联标志、字符类语义、finditer 的非重叠推进规则上都有差异,
//   手写扫描器可以精确复刻 Python 的行为。
#include "projection.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "json_render.hpp"
#include "json_walk.hpp"

namespace algo_diff_cpp {

namespace {

// HTML 噪声剥离: <script.*?</script> | <!--.*?--> | <style.*?</style>
// Python 的 (?is) = DOTALL + IGNORECASE, 且非贪婪。
std::string strip_html_noise(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        // 找下一个候选起点
        size_t best_pos = std::string::npos;
        int best_kind = -1;  // 0=script 1=comment 2=style
        size_t p;
        if ((p = s.find("<script", i)) != std::string::npos) {
            best_pos = p; best_kind = 0;
        }
        if ((p = s.find("<!--", i)) != std::string::npos &&
            (best_pos == std::string::npos || p < best_pos)) {
            best_pos = p; best_kind = 1;
        }
        if ((p = s.find("<style", i)) != std::string::npos &&
            (best_pos == std::string::npos || p < best_pos)) {
            best_pos = p; best_kind = 2;
        }
        if (best_pos == std::string::npos) {
            out.append(s, i, n - i);
            break;
        }
        out.append(s, i, best_pos - i);
        if (best_kind == 1) {
            size_t end = s.find("-->", best_pos + 4);
            if (end == std::string::npos) {
                i = n;
            } else {
                i = end + 3;
            }
            out.push_back(' ');
        } else {
            const char* close_tag = (best_kind == 0) ? "</script>" : "</style>";
            size_t end = s.find(close_tag, best_pos);
            if (end == std::string::npos) {
                i = n;
            } else {
                i = end + std::strlen(close_tag);
            }
            out.push_back(' ');
        }
    }
    return out;
}

inline char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool is_space_c(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// 一个 HTML 标签: <tagname attrs>
// Python 的正则: (?is)<\s*([a-z][a-z0-9]*)((?:\s+[^<>]*)?)/?>
// group(1) = tag name, group(2) = 属性串 (可空)
struct HtmlTag {
    std::string name;   // 小写
    std::string attrs;  // 原始属性串
    size_t value_start = 0;  // attrs 在原文中的起点 (供 id/cls 取值)
};

// 在 s 中从 start 起找下一个 HTML 标签。找到返回 true。
bool find_next_tag(const std::string& s, size_t start, HtmlTag& out) {
    size_t i = start;
    const size_t n = s.size();
    while (i < n) {
        size_t lt = s.find('<', i);
        if (lt == std::string::npos) return false;
        size_t p = lt + 1;
        // Python 允许 < 后有空白: (?is)<\s*
        while (p < n && is_space_c(s[p])) ++p;
        if (p >= n) return false;
        char c0 = s[p];
        if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z'))) {
            i = lt + 1;
            continue;
        }
        size_t name_start = p;
        ++p;
        while (p < n) {
            char c = s[p];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9')) {
                ++p;
            } else {
                break;
            }
        }
        out.name.assign(s, name_start, p - name_start);
        for (char& ch : out.name) ch = lower(ch);
        out.attrs.clear();
        out.value_start = p;
        // 属性串: (?:\s+[^<>]*)?  —— 必须以空白开头, 且不含 '<' '>'
        if (p < n && is_space_c(s[p])) {
            size_t attr_start = p;
            size_t q = p;
            while (q < n && s[q] != '<' && s[q] != '>') ++q;
            out.attrs.assign(s, attr_start, q - attr_start);
            p = q;
        }
        // 允许可选的 '/' 与 '>'
        if (p < n && s[p] == '/') ++p;
        if (p < n && s[p] == '>') return true;
        // 不匹配, 继续找
        i = lt + 1;
    }
    return false;
}

// 提取属性值: \b(class|id)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'<>]+))
// 三个捕获组对应双引号/单引号/无引号。
bool find_attr_value(const std::string& attrs, const std::string& attr,
                     std::string& out) {
    size_t i = 0;
    const size_t n = attrs.size();
    while (i < n) {
        // \b: 前面必须是词边界 (前一个字符不是 \w)
        size_t p = attrs.find(attr, i);
        while (p != std::string::npos) {
            bool boundary_ok = true;
            if (p > 0) {
                char prev = attrs[p - 1];
                if ((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') ||
                    (prev >= '0' && prev <= '9') || prev == '_') {
                    boundary_ok = false;
                }
            }
            // 词尾也不能是 \w
            bool tail_ok = true;
            size_t after = p + attr.size();
            if (after < n) {
                char nxt = attrs[after];
                if ((nxt >= 'a' && nxt <= 'z') || (nxt >= 'A' && nxt <= 'Z') ||
                    (nxt >= '0' && nxt <= '9') || nxt == '_') {
                    tail_ok = false;
                }
            }
            if (boundary_ok && tail_ok) break;
            p = attrs.find(attr, p + 1);
        }
        if (p == std::string::npos) return false;
        size_t q = p + attr.size();
        while (q < n && is_space_c(attrs[q])) ++q;
        if (q >= n || attrs[q] != '=') {
            i = p + attr.size();
            continue;
        }
        ++q;
        while (q < n && is_space_c(attrs[q])) ++q;
        if (q >= n) return false;
        char quote = attrs[q];
        if (quote == '"' || quote == '\'') {
            size_t end = attrs.find(quote, q + 1);
            if (end == std::string::npos) return false;
            out.assign(attrs, q + 1, end - q - 1);
            return true;
        }
        // 无引号: [^\s"'<>]+
        size_t end = q;
        while (end < n) {
            char c = attrs[end];
            if (is_space_c(c) || c == '"' || c == '\'' || c == '<' || c == '>') break;
            ++end;
        }
        if (end == q) return false;
        out.assign(attrs, q, end - q);
        return true;
    }
    return false;
}

}  // namespace

std::string normalize_content_type(const std::string& content_type) {
    size_t semi = content_type.find(';');
    std::string ct = (semi == std::string::npos)
                         ? content_type
                         : content_type.substr(0, semi);
    // strip + lower
    size_t b = 0, e = ct.size();
    while (b < e && is_space_c(ct[b])) ++b;
    while (e > b && is_space_c(ct[e - 1])) --e;
    std::string out;
    out.reserve(e - b);
    for (size_t i = b; i < e; ++i) out.push_back(lower(ct[i]));
    return out;
}

bool is_json_content_type(const std::string& ct) {
    return ct == "application/json" || ct == "text/json" ||
           ct == "application/javascript" || ct == "text/javascript" ||
           ct == "application/x-javascript" ||
           (ct.size() > 5 && ct.compare(ct.size() - 5, 5, "+json") == 0);
}

bool is_html_content_type(const std::string& ct) {
    return ct == "text/html" || ct == "application/xhtml+xml";
}

bool is_xml_content_type(const std::string& ct) {
    return ct == "text/xml" || ct == "application/xml" ||
           (ct.size() > 4 && ct.compare(ct.size() - 4, 4, "+xml") == 0);
}

std::vector<FieldPair> html_pairs(const std::string& body) {
    std::vector<FieldPair> out;
    if (body.empty()) return out;
    std::string content = strip_html_noise(body);

    std::set<std::string> seen;
    size_t pos = 0;
    HtmlTag tag;
    while (find_next_tag(content, pos, tag)) {
        // 推进到标签之后 (Python finditer 非重叠推进)
        size_t next = content.find('<', content.find('>', tag.value_start) + 1);
        if (next == std::string::npos) next = content.size();
        pos = next;

        std::string path_tag = "tag:" + tag.name;
        if (seen.find(path_tag) == seen.end() &&
            content.find(tag.name) != std::string::npos) {
            out.push_back({path_tag, tag.name});
            seen.insert(path_tag);
        }
        std::string v;
        // class
        if (find_attr_value(tag.attrs, "class", v)) {
            std::istringstream iss(v);
            std::string cls;
            while (iss >> cls) {
                std::string path_cls = "cls:" + tag.name;
                if (seen.find(path_cls) == seen.end() &&
                    content.find(cls) != std::string::npos) {
                    out.push_back({path_cls, cls});
                    seen.insert(path_cls);
                }
            }
        }
        // id
        if (find_attr_value(tag.attrs, "id", v)) {
            // Python: v.strip()
            size_t b = 0, e = v.size();
            while (b < e && is_space_c(v[b])) ++b;
            while (e > b && is_space_c(v[e - 1])) --e;
            std::string sv = v.substr(b, e - b);
            std::string path_id = "id:" + tag.name;
            if (seen.find(path_id) == seen.end() &&
                !sv.empty() &&
                content.find(sv) != std::string::npos) {
                out.push_back({path_id, sv});
                seen.insert(path_id);
            }
        }
    }
    return out;
}

std::string html_project(const std::string& body) {
    if (body.empty()) return std::string();
    std::string content = strip_html_noise(body);

    std::set<std::string> tokens;
    size_t pos = 0;
    HtmlTag tag;
    while (find_next_tag(content, pos, tag)) {
        size_t gt = content.find('>', tag.value_start);
        size_t next = (gt == std::string::npos) ? content.size()
                                                : content.find('<', gt + 1);
        if (next == std::string::npos) next = content.size();
        pos = next;

        tokens.insert("tag:" + tag.name);
        std::string v;
        if (find_attr_value(tag.attrs, "class", v)) {
            std::istringstream iss(v);
            std::string cls;
            while (iss >> cls) {
                tokens.insert("cls:" + tag.name + "." + cls);
            }
        }
        if (find_attr_value(tag.attrs, "id", v)) {
            size_t b = 0, e = v.size();
            while (b < e && is_space_c(v[b])) ++b;
            while (e > b && is_space_c(v[e - 1])) --e;
            std::string sv = v.substr(b, e - b);
            if (!sv.empty()) {
                tokens.insert("id:" + tag.name + "#" + sv);
            }
        }
    }
    std::vector<std::string> vec(tokens.begin(), tokens.end());
    std::sort(vec.begin(), vec.end());
    std::string out;
    for (size_t i = 0; i < vec.size(); ++i) {
        if (i) out.push_back('\n');
        out += vec[i];
    }
    return out;
}

std::vector<FieldPair> xml_pairs(const std::string& body) {
    // 移植 Python xml_pairs: 栈式扫描, 深度上限 32。
    const size_t MAX_XML_DEPTH = 32;
    std::vector<FieldPair> out;
    if (body.empty()) return out;
    bool all_space = true;
    for (char c : body) {
        if (!is_space_c(c)) { all_space = false; break; }
    }
    if (all_space) return out;

    // stack: (tag, start_offset_of_inner)
    std::vector<std::pair<std::string, size_t>> stack;
    size_t i = 0;
    const size_t n = body.size();
    while (i < n) {
        size_t lt = body.find('<', i);
        if (lt == std::string::npos) break;
        if (lt + 1 < n && body[lt + 1] == '/') {
            size_t gt = body.find('>', lt + 1);
            if (gt == std::string::npos) break;
            // tag = content[lt+2:gt].strip().split()[0].lower()
            std::string raw = body.substr(lt + 2, gt - lt - 2);
            size_t b = 0, e = raw.size();
            while (b < e && is_space_c(raw[b])) ++b;
            while (e > b && is_space_c(raw[e - 1])) --e;
            raw = raw.substr(b, e - b);
            size_t sp = raw.find_first_of(" \t\n\r\f\v");
            if (sp != std::string::npos) raw = raw.substr(0, sp);
            for (char& ch : raw) ch = lower(ch);
            // 找栈中匹配的 tag
            for (int j = static_cast<int>(stack.size()) - 1; j >= 0; --j) {
                if (stack[static_cast<size_t>(j)].first == raw) {
                    size_t start = stack[static_cast<size_t>(j)].second;
                    std::string inner = body.substr(start, lt - start);
                    if (!raw.empty() && inner.find('<') == std::string::npos) {
                        size_t ib = 0, ie = inner.size();
                        while (ib < ie && is_space_c(inner[ib])) ++ib;
                        while (ie > ib && is_space_c(inner[ie - 1])) --ie;
                        std::string trimmed = inner.substr(ib, ie - ib);
                        if (!trimmed.empty()) {
                            std::string path = ".";
                            for (int k = 0; k < j; ++k) {
                                path += stack[static_cast<size_t>(k)].first;
                                path += ".";
                            }
                            path += raw;
                            out.push_back({path, trimmed});
                        }
                    }
                    stack.resize(static_cast<size_t>(j));
                    break;
                }
            }
            i = gt + 1;
            continue;
        }
        // 开标签: Python _XML_TAG_RE = (?is)<\s*([a-z][a-z0-9_:-]*)((?:\s+[^<>]*)?)/?>
        size_t p = lt + 1;
        while (p < n && is_space_c(body[p])) ++p;
        if (p >= n) { i = lt + 1; continue; }
        char c0 = body[p];
        if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z'))) {
            i = lt + 1;
            continue;
        }
        size_t name_start = p;
        ++p;
        while (p < n) {
            char c = body[p];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == ':' || c == '-') {
                ++p;
            } else {
                break;
            }
        }
        std::string tagname = body.substr(name_start, p - name_start);
        for (char& ch : tagname) ch = lower(ch);
        // 属性部分 (跳过, 只关心自闭合判定)
        size_t q = p;
        if (q < n && is_space_c(body[q])) {
            while (q < n && body[q] != '<' && body[q] != '>') ++q;
        }
        bool self_closing = false;
        if (q > p && body[q - 1] == '/') self_closing = true;
        if (q < n && body[q] == '/') self_closing = true;
        size_t after = q;
        if (after < n && body[after] == '>') ++after;
        if (self_closing) {
            std::string path = ".";
            for (const auto& st : stack) {
                path += st.first;
                path += ".";
            }
            path += tagname;
            out.push_back({path, std::string()});
        } else if (stack.size() < MAX_XML_DEPTH) {
            stack.push_back({tagname, after});
        }
        i = after;
    }
    return out;
}

std::vector<FieldPair> raw_pairs(const std::string& body) {
    std::vector<FieldPair> out;
    if (body.empty()) return out;
    std::set<std::string> seen;
    size_t pos = 0;
    HtmlTag tag;
    while (find_next_tag(body, pos, tag)) {
        size_t gt = body.find('>', tag.value_start);
        size_t next = (gt == std::string::npos) ? body.size()
                                                : body.find('<', gt + 1);
        if (next == std::string::npos) next = body.size();
        pos = next;

        out.push_back({"tag:" + tag.name, tag.name});
        std::string v;
        if (find_attr_value(tag.attrs, "class", v)) {
            std::istringstream iss(v);
            std::string cls;
            while (iss >> cls) {
                out.push_back({"cls:" + tag.name, cls});
            }
        }
        if (find_attr_value(tag.attrs, "id", v)) {
            size_t b = 0, e = v.size();
            while (b < e && is_space_c(v[b])) ++b;
            while (e > b && is_space_c(v[e - 1])) --e;
            std::string sv = v.substr(b, e - b);
            if (!sv.empty()) out.push_back({"id:" + tag.name, sv});
        }
    }
    // Python raw_pairs: 按 path 去重 (保留首次出现的 value)
    std::vector<FieldPair> deduped;
    std::set<std::string> dseen;
    for (const FieldPair& p : out) {
        if (dseen.find(p.path) != dseen.end()) continue;
        dseen.insert(p.path);
        deduped.push_back(p);
    }
    return deduped;
}

std::vector<FieldPair> project_pairs(const std::string& body,
                                     const std::string& content_type) {
    std::string ct = normalize_content_type(content_type);
    if (is_json_content_type(ct)) return json_pairs(body);
    if (is_html_content_type(ct)) return html_pairs(body);
    if (is_xml_content_type(ct)) return xml_pairs(body);
    return raw_pairs(body);
}

std::string project(const std::string& body, const std::string& content_type) {
    std::string ct = normalize_content_type(content_type);
    if (is_json_content_type(ct)) return json_project(body);
    if (is_html_content_type(ct)) return html_project(body);
    // XML 与未知类型: 原样透传 (Python 版行为一致)
    return body;
}

}  // namespace algo_diff_cpp
