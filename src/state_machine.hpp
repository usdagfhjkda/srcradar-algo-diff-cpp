// state_machine.hpp — 5 规则 + A 算法声明
//
// 状态机(ct = 0/1/2/4/6/8 数字终态, 10/12/13 字母中间态):
//   见 README.md "算法核心 (5 规则 + A 解耦)" 段。
//
// 5 个 transition:
//   T1. C reverify (ct=12)        → httpx -rl 50, 有响应 ct=10, 没响应 ct=13
//   T2. D reverify (ct=13)        → httpx -rl 50, 有响应 ct=10, 没响应 ct=8
//   T3. fast transition (ct=0)    → 4 分支: not_in_pre=1 / a0b0=0 /
//                                        a1b0=12 / curr=1=10
//   T4. A 算法 (ct=10)            → SequenceMatcher + (1-pre.is_active)*4
//                                        → 0/2/4/6
//
// 字母→数字映射 (16 进制规则, 列 INTEGER):
//   0/1/2/4/6/8 = 数字终态 (default, added, changed, reactivated,
//                            reactivated+changed, extinct)
//   A = 10      = 持续活中间态 (走 T4 SequenceMatcher)
//   B = 11      = (reserved, 算法 v9 已消解, 兼容保留)
//   C = 12      = 失活中间态 (走 T1 httpx reverify)
//   D = 13      = 失活 + C 走完第二次 (= 走 T2 httpx reverify)
//
// pre.is_active 来源: web_subdomains_pre LEFT JOIN (commit 565e925 引入的快照)

#pragma once

#include <cstdint>
#include <string>

namespace algo_diff_cpp {

// 字母 + 数字终态, 列存 INTEGER (列定义 schema.sql: INTEGER NOT NULL DEFAULT 0)。
//   为什么 A/B/C/D 不用字母字符串: SQLite INTEGER 列, 字母字符串会被强转 0,
//   强转会丢 ct 信息。数字映射 (10/11/12/13) 是 ASCII 'A'..'D', 但存的就是数字。
enum class CtState : int8_t {
    ST_0  = 0,
    ST_1  = 1,
    ST_2  = 2,
    ST_4  = 4,
    ST_6  = 6,
    ST_8  = 8,
    ST_A  = 10,  // 走 T4 SequenceMatcher (算法 v9)
    ST_C  = 12,  // 走 T1 httpx reverify
    ST_D  = 13,  // 走 T2 httpx reverify
};

// 阈值: ratio >= 0.95 → no change (0), ratio < 0.95 → changed (2)。
// 算法 v9 锁定; ratio < 0.5 → changed 也是 same 2, 不另开分支。
constexpr double RATIO_THRESHOLD = 0.95;

// reverify 用 httpx 外部工具, 调用前写 .jsonl, 不在 C++ 里启 subprocess。
// reverify 调用由 daily_monitor 的 scheduler.sh 阶段拼接, 本模块只负责写文件
// 路径 + 写回结果。

// T3 fast transition 决策表 (纯函数, 单测无 SQLite 依赖)。
// 输入: cur.is_active, pre.is_active (LEFT JOIN pre.id), prev_body 是否 NULL。
// 输出: 新 ct (含 0/1/10/12, 10 = 走 T4 入口)。
//
// 备注: a0b0 直接写 0, 不进 T4, **不会** 走 +(1-pre.is_active)*4 公式。
//       a0b1 / a1b1 都标 10, 由 T4 用 pre.is_active 决定是否 +4。
CtState fast_transition(int8_t current_ct,
                        bool curr_is_active,
                        bool pre_is_active_exists,
                        bool pre_is_active);

// T1 / T2 reverify 决策: 有响应 / 没响应。输入是 reverify.jsonl 解析后的
// 一行记录 (id, body, status_code, content_type 等), 输出是新 ct。
//   - 有响应: curr_body = body, ct = 10 (走 T4)
//   - 无响应: curr_body 保留旧值
//      (T1 没响应 → 13 / T2 没响应 → 8)
CtState reverify_decision(bool has_response,
                          CtState reverify_source /* T1=ST_C, T2=ST_D */);

// T4 A 算法决策: 输入 ratio + pre.is_active, 输出 0/2/4/6。
// 算法 v9 公式: ct = (0 if ratio >= 0.95 else 2) + (1 - pre.is_active) * 4
//   pre=1, ratio>=0.95 → 0+0 = 0
//   pre=1, ratio<0.95  → 2+0 = 2
//   pre=0, ratio>=0.95 → 0+4 = 4
//   pre=0, ratio<0.95  → 2+4 = 6
// 注: 这是 stub 接口, P5 才实现 sequence_matcher.cpp 实际算 ratio。P3 阶段
//     函数体 = run_ratio_threshold(ratio, pre_is_active) 即可。
CtState a_decision(double ratio, bool pre_is_active);

// ratio → 0/2 (纯函数, 给 a_decision 用)
inline CtState base_ratio_to_ct(double ratio) {
    return ratio >= RATIO_THRESHOLD ? CtState::ST_0 : CtState::ST_2;
}

// ct 整数 → kind 字符串 (供 last_comparison_kind 字段, 跟 Python algo_diff
// compute.py 的 KIND_* 常量对齐)。
std::string ct_to_kind(int8_t new_ct, double ratio, bool pre_is_active,
                       bool reverify_response /* T1/T2 reverify 路径 */);

// 把 CtState 转成日志友好的名字 ("ST_0" / "ST_10" / "ST_12" 等)
std::string ct_state_name(CtState s);

}  // namespace algo_diff_cpp