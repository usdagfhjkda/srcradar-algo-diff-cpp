// difflib.cpp — Python difflib.SequenceMatcher 的 C++ 移植实现
//
// 逐行对照 CPython 3.10 /usr/lib/python3.10/difflib.py 的 SequenceMatcher。
// 行为一致性是这个模块存在的唯一理由: C++ 与 Python 必须在同一输入上
// 给出同一个 ratio, 否则 0.98 阈值两侧的判定会分叉。
#include "difflib.hpp"

#include <algorithm>

namespace algo_diff_cpp {

std::vector<CodePoint> decode_utf8(const std::string& s) {
    std::vector<CodePoint> out;
    out.reserve(s.size());
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t extra = 0;
        CodePoint cp = 0;
        if (c < 0x80) {
            cp = c;
            extra = 0;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1Fu;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0Fu;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07u;
            extra = 3;
        } else {
            // 非法起始字节 -> U+FFFD (对应 Python errors="replace")
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (i + extra >= n) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        bool valid = true;
        for (size_t k = 1; k <= extra; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (!valid) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += extra + 1;
    }
    return out;
}


SequenceMatcher::SequenceMatcher(const std::vector<CodePoint>& a,
                                 const std::vector<CodePoint>& b,
                                 bool autojunk)
    : a_(a), b_(b), autojunk_(autojunk) {}

void SequenceMatcher::chain_b() {
    if (chained_) return;
    chained_ = true;

    // 第一遍: 建 b2j 索引 (元素 -> 在 b 中的位置列表)
    b2j_.clear();
    b2j_.reserve(b_.size() * 2 + 1);
    for (size_t i = 0; i < b_.size(); ++i) {
        b2j_[b_[i]].push_back(i);
    }

    // **不剔除 junk 元素** —— Python 3.x 的 SequenceMatcher(isjunk=None) 里
    // self.isjunk 就是 None (Python 2.x 才有模块级 junk() 函数把空白当 junk),
    // 所以 __chain_b 的 "if isjunk:" 分支不成立, bjunk 恒为空集,
    // b2j 保留全部元素。
    //
    // 这点很关键: 若这里把空白/换行当 junk 剔除, b2j 索引就与 Python 不同,
    // find_longest_match 找到的匹配块不同, ratio 数值分叉 (实测 0.3759 vs 0.3696),
    // 而 ratio 是 0.98/0.02 阈值判定的输入, 分叉会变成不同的 change_type。

    // 剔除 popular 元素 (Python: autojunk 且 len(b) >= 200)
    // ntest = n // 100 + 1, 出现次数 > ntest 的元素算 popular
    const size_t n = b_.size();
    if (autojunk_ && n >= 200) {
        const size_t ntest = n / 100 + 1;
        for (auto it = b2j_.begin(); it != b2j_.end();) {
            if (it->second.size() > ntest) {
                it = b2j_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

Match SequenceMatcher::find_longest_match(size_t alo, size_t ahi,
                                          size_t blo, size_t bhi) {
    chain_b();

    size_t besti = alo, bestj = blo, bestsize = 0;

    // j2len[j] = 以 a[i-1] 与 b[j] 结尾的最长 junk-free 匹配长度
    std::unordered_map<size_t, size_t> j2len;
    std::unordered_map<size_t, size_t> newj2len;

    for (size_t i = alo; i < ahi; ++i) {
        newj2len.clear();
        auto it = b2j_.find(a_[i]);
        if (it != b2j_.end()) {
            for (size_t j : it->second) {
                if (j < blo) continue;
                if (j >= bhi) break;
                size_t prev = 0;
                auto jm = j2len.find(j - 1);
                if (jm != j2len.end()) prev = jm->second;
                size_t k = prev + 1;
                newj2len[j] = k;
                if (k > bestsize) {
                    besti = i - k + 1;
                    bestj = j - k + 1;
                    bestsize = k;
                }
            }
        }
        j2len.swap(newj2len);
    }

    // 向两端扩展元素。
    // (Python 这里的条件是 "not isbjunk(b[...])", 但 Python 3.x 的
    //  isjunk=None -> bjunk 恒空 -> isbjunk 恒 False, 所以恒为真。)
    while (besti > alo && bestj > blo &&
           a_[besti - 1] == b_[bestj - 1]) {
        --besti; --bestj; ++bestsize;
    }
    while (besti + bestsize < ahi && bestj + bestsize < bhi &&
           a_[besti + bestsize] == b_[bestj + bestsize]) {
        ++bestsize;
    }

    // Python 的 junk 吸收扩展段在此省略 —— Python 3.x 的 isjunk=None 下
    // bjunk 恒为空, isbjunk() 恒 False, 这两段 while 永不执行。
    // 保留它们反而会与 Python 行为分叉 (把空白当 junk 吸收掉)。

    Match m;
    m.a = besti;
    m.b = bestj;
    m.size = bestsize;
    return m;
}

const std::vector<Match>& SequenceMatcher::get_matching_blocks() {
    if (blocks_ready_) return blocks_;

    const size_t la = a_.size();
    const size_t lb = b_.size();

    // 队列递归 (Python 用栈, 这里是同构的显式队列)
    struct Quad { size_t alo, ahi, blo, bhi; };
    std::vector<Quad> queue;
    queue.push_back({0, la, 0, lb});

    std::vector<Match> matching;
    while (!queue.empty()) {
        Quad q = queue.back();
        queue.pop_back();
        Match x = find_longest_match(q.alo, q.ahi, q.blo, q.bhi);
        if (x.size) {
            matching.push_back(x);
            if (q.alo < x.a && q.blo < x.b) {
                queue.push_back({q.alo, x.a, q.blo, x.b});
            }
            if (x.a + x.size < q.ahi && x.b + x.size < q.bhi) {
                queue.push_back({x.a + x.size, q.ahi, x.b + x.size, q.bhi});
            }
        }
    }

    std::sort(matching.begin(), matching.end(),
              [](const Match& l, const Match& r) {
                  if (l.a != r.a) return l.a < r.a;
                  if (l.b != r.b) return l.b < r.b;
                  return l.size < r.size;
              });

    // 合并相邻块 (Python 的 non_adjacent 逻辑)
    std::vector<Match> non_adjacent;
    size_t i1 = 0, j1 = 0, k1 = 0;
    for (const Match& m : matching) {
        if (i1 + k1 == m.a && j1 + k1 == m.b) {
            k1 += m.size;
        } else {
            if (k1) {
                non_adjacent.push_back({i1, j1, k1});
            }
            i1 = m.a;
            j1 = m.b;
            k1 = m.size;
        }
    }
    if (k1) {
        non_adjacent.push_back({i1, j1, k1});
    }
    // 末位哨兵
    non_adjacent.push_back({la, lb, 0});

    blocks_ = std::move(non_adjacent);
    blocks_ready_ = true;
    return blocks_;
}

double SequenceMatcher::ratio() {
    size_t matches = 0;
    for (const Match& m : get_matching_blocks()) {
        matches += m.size;
    }
    const size_t total = a_.size() + b_.size();
    if (total == 0) return 1.0;
    return (2.0 * static_cast<double>(matches)) / static_cast<double>(total);
}

double SequenceMatcher::ratio_trimmed() {
    const size_t la = a_.size();
    const size_t lb = b_.size();
    if (la + lb == 0) return 1.0;

    // 公共前缀: 从头逐元素比较直到不同
    const size_t lim = std::min(la, lb);
    size_t p = 0;
    while (p < lim && a_[p] == b_[p]) ++p;

    // 公共后缀: 从尾逐元素比较直到不同, 且不与前缀重叠
    size_t s = 0;
    while (s < (lim - p) && a_[la - 1 - s] == b_[lb - 1 - s]) ++s;

    const size_t mid_a = la - p - s;
    const size_t mid_b = lb - p - s;

    // 中段有一侧为空 -> 没有可比的中间内容, 匹配量就是公共部分
    if (mid_a == 0 || mid_b == 0) {
        const size_t matched = p + s;
        return (2.0 * static_cast<double>(matched)) /
               static_cast<double>(la + lb);
    }

    // 只对中段建匹配器
    const std::vector<CodePoint> va(a_.begin() + static_cast<long>(p),
                                    a_.begin() + static_cast<long>(p + mid_a));
    const std::vector<CodePoint> vb(b_.begin() + static_cast<long>(p),
                                    b_.begin() + static_cast<long>(p + mid_b));
    SequenceMatcher mid(va, vb, /*autojunk=*/false);

    size_t matches_mid = 0;
    for (const Match& m : mid.get_matching_blocks()) {
        matches_mid += m.size;
    }
    // 公共前后缀逐元素相等, 必然是匹配 -> 补回
    const size_t matches = matches_mid + p + s;
    return (2.0 * static_cast<double>(matches)) / static_cast<double>(la + lb);
}

}  // namespace algo_diff_cpp
