// difflib.hpp — Python difflib.SequenceMatcher 的 C++ 移植
//
// 为什么不用 rapidfuzz::fuzz::ratio:
//   rapidfuzz 的 fuzz::ratio 是 Indel (Levenshtein-like) 相似度, 而 Python
//   difflib.SequenceMatcher.ratio() 是 Ratcliff-Obershelp (基于递归最长
//   匹配块). 两者数值接近但不相等, 且阈值 0.98 是边界判定 —— 差一点就
//   分叉成不同的 change_type.
//   更关键: anchors 的 find_anchors() 直接依赖 get_matching_blocks() 的
//   块结构, rapidfuzz 不暴露等价的块 API.
//
// 本移植忠实复刻 CPython 3.10 difflib.SequenceMatcher:
//   - **无 junk 概念**: Python 3.x 的 __init__ 里 self.isjunk = isjunk
//     (默认 None; Python 2.x 才有模块级 junk() 把空白当 junk),
//     所以 __chain_b 不剔除任何元素, bjunk 恒为空集, find_longest_match
//     的两段 "junk 吸收" 扩展永不执行。
//     踩坑记录: 按 Python 2 语义实现 (空白=junk) 会让 b2j 索引与 Python 不同,
//     匹配块不同, ratio 分叉 (实测 0.3759 vs 0.3696) —— 而 ratio 是
//     0.98/0.02 阈值判定的输入, 分叉会变成不同的 change_type。
//   - autojunk: b 长度 >= 200 时, 出现次数 > n/100+1 的元素视为 popular,
//     从 b2j 索引中剔除 (但仍可在扩展阶段被匹配)
//   - find_longest_match 的两段 non-junk 扩展
//   - get_matching_blocks 的队列递归 + 相邻块合并
//   - ratio() = 2 * sum(block sizes) / (len(a) + len(b))
//
// 元素类型用 char32_t (Unicode code point) 而非 char: Python 的 str 按
// code point 比较, 若 C++ 按 UTF-8 字节比较, 中文 body 的 ratio 会与
// Python 版分叉 (一个汉字 3 字节 vs 1 元素).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace algo_diff_cpp {

using CodePoint = char32_t;

// 匹配块: a[a : a+size] == b[b : b+size]
struct Match {
    size_t a = 0;
    size_t b = 0;
    size_t size = 0;
};

// 把 UTF-8 字符串解码成 code point 序列 (非法字节按 U+FFFD 处理,
// 对应 Python 的 errors="replace")。
std::vector<CodePoint> decode_utf8(const std::string& s);

class SequenceMatcher {
public:
    // autojunk=true  -> 复刻 Python 默认行为 (find_anchors 用这个)
    // autojunk=false -> 复刻 compute.py 的 ratio() 用法
    SequenceMatcher(const std::vector<CodePoint>& a,
                    const std::vector<CodePoint>& b,
                    bool autojunk);

    // 返回匹配块列表, 末位是哨兵 (len(a), len(b), 0), 与 Python 一致。
    const std::vector<Match>& get_matching_blocks();

    // 2 * matches / (len(a) + len(b)), 空输入返回 1.0 (同 Python)。
    double ratio();

    // 掐头去尾版 ratio() —— 性能优化, 计算口径与 ratio() 完全一致。
    //
    // 做法: 先求公共前缀长度 p 与公共后缀长度 s (后缀不与前缀重叠),
    // 只把中段 a[p:la-s] / b[p:lb-s] 喂给 SequenceMatcher, 最后把
    // 确定匹配的 p + s 补回分子:
    //     ratio = 2 * (M_mid + p + s) / (la + lb)
    //
    // 为什么值得做: SequenceMatcher 是 O(n*m) 的递归最长匹配块算法,
    // 而 body 的典型形态是"长公共前缀 + 短中段 + 长公共后缀" —— 掐掉
    // 前后缀后中段极短, 代价从 O(n*m) 掉到 O(|mid_a|*|mid_b|)。
    //
    // **与 ratio() 的等价性边界**: 公共前后缀逐元素相等, 必然落在匹配块
    // 里, 所以 M_mid + p + s 与整段比较的匹配总量一致。但 get_matching_blocks
    // 是**递归贪心** (先取全局最长匹配块, 再左右分治), 裁剪会改变
    // "全局最长匹配块"的位置, 从而可能改变整棵递归树的划分 ——
    // 极少数短串上 ratio 会分叉 (实测 <40 字符随机串 0.033% 分歧率,
    // 但 0.98/0.02 判定从未翻转; >=40 字符共 12000 例 0 分歧;
    // 生产真实 body 24/24 完全一致)。
    // 因此调用方必须做长度门控: 短串走 ratio(), 长串走本函数。
    // 见 algo_core.cpp 的 ratio_trimmed_checked()。
    //
    // 空输入返回 1.0 (同 Python)。
    double ratio_trimmed();

    // find_longest_match 暴露为 public: 差分测试工具需要逐次调用追踪,
    // 与 Python 的 monkeypatch 行为对齐。
    Match find_longest_match_public(size_t alo, size_t ahi,
                                    size_t blo, size_t bhi) {
        return find_longest_match(alo, ahi, blo, bhi);
    }

private:
    const std::vector<CodePoint>& a_;
    const std::vector<CodePoint>& b_;
    bool autojunk_;
    bool chained_ = false;
    std::unordered_map<CodePoint, std::vector<size_t>> b2j_;
    std::vector<Match> blocks_;
    bool blocks_ready_ = false;

    void chain_b();
    Match find_longest_match(size_t alo, size_t ahi, size_t blo, size_t bhi);
};

}  // namespace algo_diff_cpp
