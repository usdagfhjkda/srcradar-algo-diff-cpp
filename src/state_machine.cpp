// state_machine.cpp — 5 规则 + A 算法纯函数
//
// 设计原则:
//   - 纯函数, 无 SQLite / 无文件依赖 (主 call site 是 pipeline).
//   - 单元测试: gtest / assert 写, 单文件可独立跑 (tests/test_state_machine.cpp).
//   - 算法 v9 锁定: T1/T2 reverify, T3 fast transition 4 分支, T4 A 解耦。

#include "state_machine.hpp"
#include <cstdio>
#include <string>

namespace algo_diff_cpp {

// ============================================================
// T3 fast transition (ct = 0 → 0/1/10/12)
// ============================================================
CtState fast_transition(int8_t current_ct,
                        bool curr_is_active,
                        bool pre_is_active_exists,
                        bool pre_is_active) {
    // 当前不在 0: 这是调用方 bug, 走防御 no-op
    if (current_ct != 0) {
        return static_cast<CtState>(current_ct);
    }

    // 1) 新行: id 不在 web_subdomains_pre 里。
    //    旧逻辑额外检查 pre.curr_body IS NULL, 但旧 inactive 行的 curr_body
    //    被 reverify NULL 掉后, snapshot 复制 NULL 进 pre.curr_body, T3 fast
    //    每天把同一批 ~2949 旧行打成 ST_1, email daily +N 假阳性。
    if (!pre_is_active_exists) {
        return CtState::ST_1;
    }

    // 3) a0b0 (上次失活 + 这次失活) → 0, **不进 T4** (用户 2026-10-08)
    if (!pre_is_active && !curr_is_active) {
        return CtState::ST_0;
    }

    // 4) a1b0 (上次活 + 这次失活) → 12 (C, 走 T1 reverify)
    if (pre_is_active && !curr_is_active) {
        return CtState::ST_C;
    }

    // 5) curr=1 (a1b1 或 a0b1, 都标 10) → 走 T4 (SequenceMatcher)
    //   - pre.is_active 由 T4 读 + (1-pre.is_active)*4 公式决定是否 +4
    return CtState::ST_A;
}

// ============================================================
// T1 / T2 reverify 决策
// ============================================================
CtState reverify_decision(bool has_response, CtState reverify_source) {
    // 输入必须是 C 或 D (reverify_source sanity)
    if (reverify_source != CtState::ST_C && reverify_source != CtState::ST_D) {
        // 错误调用方: 防御返回 ST_0 不污染, pipeline 应负责 assert
        std::fprintf(stderr,
            "[state_machine] reverify_decision: bad source (=%s), returning ST_0\n",
            ct_state_name(reverify_source).c_str());
        return CtState::ST_0;
    }

    if (has_response) {
        // 有响应: 写 curr_body (caller 负责) + ct = 10 (走 T4)
        return CtState::ST_A;
    }
    // 没响应: C → D (13), D → 8 (失活事件)
    return reverify_source == CtState::ST_C ? CtState::ST_D : CtState::ST_8;
}

// ============================================================
// T4 A 算法决策 (SequenceMatcher + (1-pre.is_active)*4)
// ============================================================
CtState a_decision(double ratio, bool pre_is_active) {
    CtState base = base_ratio_to_ct(ratio);
    // 算法 v9 公式: ct = base + (1 - pre.is_active) * 4
    //   pre=1 → base + 0 = 0 or 2
    //   pre=0 → base + 4 = 4 or 6
    if (!pre_is_active) {
        return static_cast<CtState>(static_cast<int8_t>(base) + 4);
    }
    return base;
}

// ============================================================
// ct → kind 字符串 (对齐 compute.py 的 KIND_* 常量)
// ============================================================
std::string ct_to_kind(int8_t new_ct, double ratio, bool pre_is_active,
                       bool reverify_response) {
    // 数字终态 1 (新行) → cold_start (但实际算法不写, fast transition 路径)
    if (new_ct == 1) return "cold_start";
    // 数字终态 8 (D 没响应) → extinct_event
    if (new_ct == 8) return "extinct_event";

    // A 算法路径 (ct == 0/2/4/6)
    if (new_ct == 0 || new_ct == 2 || new_ct == 4 || new_ct == 6) {
        // ratio 分桶 (与 compute.py: KIND_BODY_IDENTICAL / _SIMILAR / _DIFFERENT
        //  对齐; compute.py 还有 _LENGTH_MISMATCH, 留给 sequence_matcher.cpp)
        if (ratio >= 0.95) return "body_identical";
        if (ratio >= 0.5)  return "body_structurally_different";
        return "body_ambiguous_drift";
    }

    // 字母中间态 10/12/13 → reverify 路径标记
    if (new_ct == 10 || new_ct == 12 || new_ct == 13) {
        return reverify_response ? "reverify_pending" : "extinct_jitter";
    }

    // unknown: 防御, 空串 (写库会被 std::nullify 跳过, pipeline 处理)
    return "";
}

// ============================================================
// CtState 名字
// ============================================================
std::string ct_state_name(CtState s) {
    switch (s) {
        case CtState::ST_0: return "ST_0";
        case CtState::ST_1: return "ST_1";
        case CtState::ST_2: return "ST_2";
        case CtState::ST_4: return "ST_4";
        case CtState::ST_6: return "ST_6";
        case CtState::ST_8: return "ST_8";
        case CtState::ST_A: return "ST_A";
        case CtState::ST_C: return "ST_C";
        case CtState::ST_D: return "ST_D";
    }
    return "ST_unknown";
}

}  // namespace algo_diff_cpp