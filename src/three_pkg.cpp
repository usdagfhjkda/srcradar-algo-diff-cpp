// three_pkg.cpp — 3 包噪声提取实现 (2026-10-25)

#include "three_pkg.hpp"

#include <algorithm>

#include "difflib.hpp"

namespace algo_diff_cpp {

namespace {

// a 中被 b 的 matching blocks 覆盖的位置 (bool 数组, 长度 a.size())
std::vector<bool> covered_positions(const std::string& a, const std::string& b) {
    std::vector<bool> out(a.size(), false);
    if (a.empty() || b.empty()) return out;
    std::vector<CodePoint> ca = decode_utf8(a);
    std::vector<CodePoint> cb = decode_utf8(b);
    SequenceMatcher sm(ca, cb, /*autojunk=*/false);
    std::vector<Match> blocks = sm.get_matching_blocks();
    for (const Match& m : blocks) {
        if (m.size == 0) continue;
        for (size_t i = m.a; i < m.a + m.size && i < out.size(); ++i) {
            out[i] = true;
        }
    }
    return out;
}

}  // namespace

ThreePkgResult evaluate_three_pkg(const std::string& pkg1,
                                   const std::string& pkg2,
                                   const std::string& pkg3,
                                   const std::string& pre) {
    ThreePkgResult res;
    res.total_count = pkg1.size();

    // 三包长度必须一致 —— 位置对应法的前提。
    // 不一致说明服务端返回了不同的响应 (不是同一资源的重复采样),
    // 此时 3 包语义不成立, 退回 2 包路径。
    if (pkg1.empty() || pkg2.size() != pkg1.size() || pkg3.size() != pkg1.size()) {
        res.applicable = false;
        return res;
    }

    // 大小上限: 三次 O(n*m) 的 SequenceMatcher, body 太大时成本失控
    // (实测 16KB -> 5s, 64KB -> >60s)。超限直接退回 2 包路径。
    if (pkg1.size() > THREE_PKG_MAX_BODY_BYTES) {
        res.applicable = false;
        return res;
    }

    std::vector<bool> c12 = covered_positions(pkg1, pkg2);
    std::vector<bool> c13 = covered_positions(pkg1, pkg3);

    // 稳定位置 = 两个覆盖集的交集
    size_t same = 0;
    size_t checked = 0;
    for (size_t i = 0; i < pkg1.size(); ++i) {
        if (!c12[i] || !c13[i]) continue;
        ++res.stable_count;
        if (i >= pre.size()) continue;   // pre 短于此位置, 不参与比对
        ++checked;
        if (pre[i] == pkg1[i]) ++same;
    }

    if (checked == 0) {
        // 没有可比的稳定位置 (pre 太短 / 位置全错位) -> 不适用
        res.applicable = false;
        return res;
    }

    res.applicable = true;
    res.pos_cov = static_cast<double>(same) / static_cast<double>(checked);
    res.unchanged = (res.pos_cov >= THREE_PKG_POS_COV_THRESHOLD);
    return res;
}

}  // namespace algo_diff_cpp
