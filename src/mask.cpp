// mask.cpp — 动态字段掩码实现
#include "mask.hpp"

#include <cstring>
#include <unordered_map>

namespace algo_diff_cpp {

const char* const MASKED_TOKEN = "__MASKED__";

bool should_mask(const std::string& value, const LengthStat& stat) {
    if (value.empty()) return false;
    if (value.size() > MAX_MASKABLE_LENGTH) return false;
    if (stat.hit_count < MASK_THRESHOLD) return false;
    if (stat.length != static_cast<int64_t>(value.size())) return false;
    return true;
}

std::string mask_dynamic_fields(const std::string& body,
                                 const std::string& content_type,
                                 const std::vector<LengthStat>& stats_rows) {
    if (stats_rows.empty()) return body;
    if (body.empty()) return body;

    std::vector<FieldPair> pairs = project_pairs(body, content_type);
    if (pairs.empty()) return body;

    // 按 path 建索引 (Python: by_path[path] = row, 后写覆盖前写)
    std::unordered_map<std::string, const LengthStat*> by_path;
    by_path.reserve(stats_rows.size() * 2 + 1);
    for (const LengthStat& st : stats_rows) {
        if (st.path.empty()) continue;
        by_path[st.path] = &st;
    }
    if (by_path.empty()) return body;

    // 单次左到右扫描, 游标保证不重匹配已改写区域
    std::string out;
    out.reserve(body.size());
    size_t cursor = 0;
    for (const FieldPair& fp : pairs) {
        auto it = by_path.find(fp.path);
        if (it == by_path.end()) continue;
        if (!should_mask(fp.value, *it->second)) continue;
        size_t idx = body.find(fp.value, cursor);
        if (idx == std::string::npos) continue;
        out.append(body, cursor, idx - cursor);
        out += MASKED_TOKEN;
        cursor = idx + fp.value.size();
    }
    out.append(body, cursor, body.size() - cursor);
    return out;
}

}  // namespace algo_diff_cpp
