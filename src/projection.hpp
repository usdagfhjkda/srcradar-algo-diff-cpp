// projection.hpp — Content-Type 路由 + 结构化投影
//
// 移植自 Python algo_diff/mask.py 的 project() / json_project() / html_project(),
// 让 C++ 与 Python 版在同一输入上产出逐字节一致的投影串 (ratcliff-oberhelp
// 的输入必须完全相同, 否则 ratio 阈值 0.98 的语义就分叉了)。
//
// 四种投影 (与 Python project() 的路由顺序一致):
//   JSON (application/json / text/json / application/javascript /
//         text/javascript / application/x-javascript / *+json)
//       -> json_project: 排序后的 "path=value" 行, path 是 leaf dot-path,
//          数组额外产出一行 "<path>.__len__=<n>"
//   HTML (text/html / application/xhtml+xml)
//       -> html_project: 排序后的 "tag:x" / "cls:tag.x" / "id:tag#x" 骨架 token,
//          丢弃所有文本内容
//   XML  (text/xml / application/xml / *+xml)
//       -> 原样透传 (Python 版也是透传: XML 走 text, 让 length-stats 层
//          能按 dot-path 定位叶值)
//   其他 -> 原样透传 (raw)
//
// 注意: 投影是"结构相似度"的输入, 与 mask 层 (mask.hpp) 的先后顺序是
//   mask -> project, 见 mask.hpp 注释。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "json_walk.hpp"

namespace algo_diff_cpp {

// FieldPair 定义在 json_walk.hpp (与 JSON 投影实现放在一起)。

// Content-Type 归一化: 取 ';' 前的 mime, 去空白, 转小写。
std::string normalize_content_type(const std::string& content_type);

// JSON 判定 (与 Python project() 的条件集合逐条对齐)。
bool is_json_content_type(const std::string& normalized_ct);
bool is_html_content_type(const std::string& normalized_ct);
bool is_xml_content_type(const std::string& normalized_ct);

// json_pairs: 解析 JSON, 按 key 排序走 leaf。
//   - 解析失败返回空 vector (Python: json.loads 抛异常 -> return [])。
//   - 数组额外产出一行 "<path>.__len__=<len>"。
//   - 标量值用 Python 的 str() 语义渲染: bool -> "True"/"False",
//     None -> "None", int -> 十进制, float -> repr 语义。
std::vector<FieldPair> json_pairs(const std::string& body);

// html_pairs: 抽取 (tag:<t>, <t>) / (cls:<t>, <class>) / (id:<t>, <id>)。
//   path 不嵌入动态值 (只用 tag 名), 保证跨次访问稳定。
//   与 Python html_pairs 一致: 同一 path 只出现一次 (seen 去重)。
std::vector<FieldPair> html_pairs(const std::string& body);

// html_project: 排序后的骨架 token, 用 '\n' 连接。
std::string html_project(const std::string& body);

// xml_pairs: 栈式扫描器, 产出 (dot-path, inner text)。
//   深度上限 32 (与 Python MAX_XML_DEPTH 一致)。
std::vector<FieldPair> xml_pairs(const std::string& body);

// raw_pairs: 未分类 body 的兜底 —— 抽 HTML tag 名与 class/id 值。
std::vector<FieldPair> raw_pairs(const std::string& body);

// project_pairs: 按 Content-Type 路由到上面四个之一。
std::vector<FieldPair> project_pairs(const std::string& body,
                                     const std::string& content_type);

// project: Content-Type 路由, 返回用于相似度比较的规范形式。
//   注意: 本函数 **不做 mask** —— Python 版的 mask 在 project() 内部先做,
//   C++ 版由 pipeline 显式按 mask -> project 顺序调用 (见 mask.hpp)。
std::string project(const std::string& body, const std::string& content_type);

}  // namespace algo_diff_cpp
