// json_walk.cpp — JSON 投影实现 (simdjson DOM)
#include "json_walk.hpp"

#include <simdjson.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "json_render.hpp"

namespace algo_diff_cpp {

namespace {

// 收集一个 DOM element 的所有 leaf (path, python-str)
void collect(simdjson::dom::element el, const std::string& path,
              std::vector<FieldPair>& out) {
    switch (el.type()) {
        case simdjson::dom::element_type::OBJECT: {
            // 收集 (key, element) 后按 key 排序 (Python: sorted(obj.keys()))
            std::vector<std::pair<std::string, simdjson::dom::element>> items;
                        for (auto field : el.get_object()) {
                            // DOM 的 key_value_pair.key 是 std::string_view 直接值 (非 result)
                            items.emplace_back(std::string(field.key), field.value);
                        }
            std::sort(items.begin(), items.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& it : items) {
                std::string cp = path.empty() ? ("." + it.first)
                                              : (path + "." + it.first);
                collect(it.second, cp, out);
            }
            return;
        }
        case simdjson::dom::element_type::ARRAY: {
            size_t count = 0;
            for (auto item : el.get_array()) {
                std::string cp = path + "[" + std::to_string(count) + "]";
                ++count;
                collect(item, cp, out);
            }
            out.push_back({path + ".__len__", std::to_string(count)});
            return;
        }
        case simdjson::dom::element_type::STRING: {
            std::string_view sv;
            if (el.get_string().get(sv)) return;
            out.push_back({path, std::string(sv)});
            return;
        }
        case simdjson::dom::element_type::BOOL: {
            bool b = false;
            if (el.get_bool().get(b)) return;
            out.push_back({path, b ? "True" : "False"});
            return;
        }
        case simdjson::dom::element_type::NULL_VALUE:
            out.push_back({path, "None"});
            return;
        case simdjson::dom::element_type::INT64: {
            int64_t v = 0;
            if (el.get_int64().get(v)) return;
            out.push_back({path, std::to_string(v)});
            return;
        }
        case simdjson::dom::element_type::UINT64: {
            uint64_t v = 0;
            if (el.get_uint64().get(v)) return;
            out.push_back({path, std::to_string(v)});
            return;
        }
        case simdjson::dom::element_type::DOUBLE: {
            double v = 0.0;
            if (el.get_double().get(v)) return;
            out.push_back({path, python_float_repr(v)});
            return;
        }
        default:
            return;
    }
}

}  // namespace

std::vector<FieldPair> json_pairs(const std::string& body) {
    std::vector<FieldPair> out;
    if (body.empty()) return out;
    // 空白检查 (Python: not content.strip())
    bool all_space = true;
    for (unsigned char c : body) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' &&
            c != '\v') {
            all_space = false;
            break;
        }
    }
    if (all_space) return out;

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    auto err = parser.parse(body.c_str(), body.size()).get(doc);
    if (err) return out;  // 解析失败 -> 空 (与 Python 一致)

    collect(doc, "", out);
    return out;
}

std::string json_project(const std::string& body) {
    std::vector<FieldPair> pairs = json_pairs(body);
    std::vector<std::string> lines;
    lines.reserve(pairs.size());
    for (const FieldPair& p : pairs) {
        lines.push_back(p.path + "=" + p.value);
    }
    std::sort(lines.begin(), lines.end());
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) out.push_back('\n');
        out += lines[i];
    }
    return out;
}

}  // namespace algo_diff_cpp
