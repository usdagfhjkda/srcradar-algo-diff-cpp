// json_walk.hpp — JSON 投影 (基于 vendored simdjson 3.9.5 的 DOM API)
//
// 为什么用 DOM 而不是 ondemand:
//   ondemand 是 **流式** 的, 边迭代边递归会消费同一个 value 的解析状态,
//   导致嵌套对象/数组的内容丢失 (实测: depth=2 的 JSON 投影退化成空串,
//   ratio 恒为 1.0 -> 所有 JSON 变更被误判为"无变化")。
//   DOM 一次性构建完整文档树, 遍历无状态问题, 且 DOM 的 element_type
//   能精确区分 INT64 / UINT64 / DOUBLE / STRING / BOOL / NULL。
//
// 与 Python json.loads 的语义差异 (已在生产数据上评估, 见下):
//   1. 解析严格性: simdjson 比 Python **宽松** —— 接受 "01" / 尾随逗号等
//      Python 拒绝的写法。生产库的 body 全部是服务端生成的合法 JSON
//      (实测 14/14 个 JSON body Python 解析成功), 命中率为 0, 故不补。
//      已知偏差方向: 畸形 JSON 下 C++ 投影非空、Python 为空。
//   2. NaN / Infinity: Python json.loads 接受, simdjson 默认拒绝。
//      生产库 0 命中, 不补。
//   3. 超 int64 整数: DOM 无 bigint 元素类型, 走 DOUBLE 路径会有精度损失。
//      生产库 0 命中, 不补。
//   4. 浮点 repr: Python 的 repr() 格式规则任何 C++ 库都不提供,
//      由 json_render.cpp 的 python_float_repr() 补齐 (已实现)。
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace algo_diff_cpp {

// 一个 (path, value) 对。path 是稳定地址 (不嵌入动态值), value 是字面量。
struct FieldPair {
    std::string path;
    std::string value;
};

// JSON 解析 + 遍历, 产出 (path, leaf-value) 对。
//   - 顶层 object 的 key 按字典序遍历 (与 Python sorted(obj.keys()) 一致)
//   - 数组额外产出一行 "<path>.__len__=<len>"
//   - 解析失败返回空 vector (与 Python json.loads 抛异常 -> return [] 一致)
std::vector<FieldPair> json_pairs(const std::string& body);

// json_project: json_pairs -> 排序后的 "path=value" 行, 用 '\n' 连接。
std::string json_project(const std::string& body);

}  // namespace algo_diff_cpp
