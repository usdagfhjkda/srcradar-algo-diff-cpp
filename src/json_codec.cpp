// json_codec.cpp — simdjson wrapper
//
// 用 simdjson::ondemand 解析 httpx JSON 输出, 提取 body 字段。
//   httpx raw_json 结构: {"a": [...], "body": "...string...", ...}
//   body 字段是 string 类型, 内嵌 HTML。
//
// 算法 v9: 失败 → exit 4 (跟 zlib_codec 一致)。

#include "json_codec.hpp"
#include <simdjson.h>
#include <cstdio>
#include <string>

namespace algo_diff_cpp {

bool extract_body_from_raw_json(const std::string& raw_json,
                                std::vector<unsigned char>& body_out,
                                std::string& error_msg) {
    if (raw_json.empty()) {
        error_msg = "empty raw_json (column is NULL or empty)";
        return false;
    }

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(raw_json.data(), raw_json.size());

    auto doc_result = parser.iterate(padded);
    if (doc_result.error()) {
        error_msg = std::string("simdjson iterate: ") + simdjson::error_message(doc_result.error());
        return false;
    }
    auto& doc = doc_result.value();

    // 取 body 字段 (top-level string)
    auto body_result = doc["body"].get_string();
    if (body_result.error()) {
        // body 字段不存在 / 不是 string → httpx 默认不带 -irr flag 时, 没有
        // body 字段, 这是 schema drift 之前的状态。
        error_msg = std::string("raw_json['body']: ") + simdjson::error_message(body_result.error());
        return false;
    }

    // simdjson on-demand string 是 zero-copy view, 拿到 std::string_view
    std::string_view sv = body_result.value();
    body_out.assign(sv.begin(), sv.end());
    return true;
}

}  // namespace algo_diff_cpp