// three_pkg.hpp — 3 包噪声提取 (2026-10-25)
//
// 背景: 灰区 (ratio 落在 0.02~0.98) 只有 pre + cur 两点, 无法区分
// "动态噪声" 与 "同长度的真实变更"。三包 (同一时刻连发 2 次 + 原扫描)
// 提供了判定噪声的充分条件: 同一位置在三包中都变 -> 必然是动态噪声。
//
// 判定流程:
//   1. stable = matching_blocks(pkg1, pkg2) ∩ matching_blocks(pkg1, pkg3)
//      (位置集合, 不是单块 —— 单块会漏掉落在块外的真实变更)
//   2. pos_cov = |{i ∈ stable : pre[i] == pkg1[i]}| / |stable|
//      位置对应法: stable 位置在 pre 的同一位置内容相同 -> 该段未变。
//      不能用 "stable_text 与 pre 的字符串 ratio" —— 实测 id=138 会得到
//      0.000 (stable 明明是 pre 的子串), 因为拼接后位置关系已丢失。
//   3. pos_cov >= THRESHOLD -> 未变 (ct=0); 否则 -> 真实变更 (ct=2)

#ifndef ALGO_DIFF_CPP_THREE_PKG_HPP
#define ALGO_DIFF_CPP_THREE_PKG_HPP

#include <string>
#include <vector>

namespace algo_diff_cpp {

// 位置对应覆盖率阈值 (用户拍板 2026-10-25)
constexpr double THREE_PKG_POS_COV_THRESHOLD = 0.95;

// 三包分支的 body 大小上限 (2026-10-26)。
// 三包位置交集要做 3 次 SequenceMatcher (O(n*m)), 成本是 2 包路径的 3 倍。
// 实测单行耗时: 4KB -> 296ms, 8KB -> 913ms, 16KB -> 5.0s, 64KB -> >60s。
// 生产库里三包候选 body 最大 239KB —— 不设限会把整条 T4 拖垮。
// 超过此上限的行判 not applicable, 落回 2 包路径 (不影响正确性: 这么大的
// body 上位置交集本身也不可靠)。
// 注意: 这是三包**分支内部**的保护, 与全局 MAX_BODY_BYTES (超长降级) 无关,
//       后者按用户决定保持不变。
constexpr size_t THREE_PKG_MAX_BODY_BYTES = 8 * 1024;

struct ThreePkgResult {
    bool   applicable = false;   // 三包是否齐备 (缺包 -> 不适用)
    size_t stable_count = 0;     // 稳定位置数
    size_t total_count = 0;      // pkg1 总长度
    double pos_cov = 0.0;        // 位置对应覆盖率
    bool   unchanged = false;    // pos_cov >= 阈值
};

// 三包噪声提取判定。
//   pkg1: 原扫描 body (已解压)
//   pkg2: 同一时刻连发第 1 次 body (已解压)
//   pkg3: 同一时刻连发第 2 次 body (已解压)
//   pre : 上一轮 body (已解压)
// 三包长度必须一致 (pkg1 == pkg2 == pkg3), 否则位置对应无意义 -> not applicable。
ThreePkgResult evaluate_three_pkg(const std::string& pkg1,
                                   const std::string& pkg2,
                                   const std::string& pkg3,
                                   const std::string& pre);

}  // namespace algo_diff_cpp

#endif  // ALGO_DIFF_CPP_THREE_PKG_HPP
