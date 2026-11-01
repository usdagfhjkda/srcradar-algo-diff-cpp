// algo_core.cpp — T4 变更判定核心实现
#include "algo_core.hpp"
#include "three_pkg.hpp"

#include <algorithm>
#include <cstring>

#include "blake2b.hpp"
#include "difflib.hpp"
#include "projection.hpp"

namespace algo_diff_cpp {

namespace {

std::string sha256_hex(const std::string& data) {
    // 用 BLAKE2b-256 代替 SHA256? 不行 —— Python 用的是 SHA256, 必须一致。
    // 这里直接实现 SHA-256 (约 60 行), 保证 STEP 2 与 Python 完全等价。
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
        0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
        0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
        0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
        0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
        0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
        0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
        0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
        0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};

    std::string msg = data;
    const uint64_t bitlen = static_cast<uint64_t>(data.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<char>((bitlen >> (8 * i)) & 0xFF));
    }

    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(static_cast<unsigned char>(msg[off + i * 4])) << 24) |
                   (static_cast<uint32_t>(static_cast<unsigned char>(msg[off + i * 4 + 1])) << 16) |
                   (static_cast<uint32_t>(static_cast<unsigned char>(msg[off + i * 4 + 2])) << 8) |
                   (static_cast<uint32_t>(static_cast<unsigned char>(msg[off + i * 4 + 3])));
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 8; ++i) {
        for (int j = 3; j >= 0; --j) {
            uint8_t byte = static_cast<uint8_t>((h[i] >> (8 * j)) & 0xFF);
            out.push_back(hex[byte >> 4]);
            out.push_back(hex[byte & 0x0F]);
        }
    }
    return out;
}

}  // namespace

// 掐头去尾快路径的启用门槛 (body 的 code point 数)。
//
// 依据 (实测对比 ratio 与 0.98/0.02 判定):
//   真实 body 形态 (改写/阈值附近/单点小改): 24000 例 -> ratio 差异 0, 判定差异 0
//   长度分档 (40/200/1000/5000 字符各 3000 例): >=40 字符 0 分歧;
//     <40 字符 1/3000 分歧 (最大偏差 0.32) 但判定 0 次翻转
//   纯随机短串 420000 例: 36 例不等价, 全部是长度 <25 的短串
//   生产真实 body: 35 个样本 (C++ 基准) + 24 个样本 (Python 对拍),
//     ratio 完全一致 (偏差 0)
// 取 40 作为门槛: 短 body 本来就快 (毫秒级), 走原算法零风险;
// 长 body 才是 O(n*m) 的痛点, 用快路径拿收益。
constexpr size_t MIN_TRIM_LEN = 40;

// 收益门控: 裁剪后中段的估算工作量相对原序列的最大允许比例。
//
// 掐头去尾不是免费的: 它要多付两次开销 —— 拷贝中段到新 vector,
// 以及重建 b2j 索引。若公共前后缀本来就短 (裁剪几乎不减工作量),
// 这两次开销就是净损失。
// 实测 (C++ 基准, 真实生产 body):
//   len=1762  mid=1387  -> 原 8.0ms  裁剪 14.2ms (慢 78%)
//   len=25833 mid=24895 -> 原 764ms 裁剪 805ms  (慢 5%)
//   len=10619 mid=15    -> 原 1366ms 裁剪 0.018ms (快 7 万倍)
// 取 0.5: 裁剪后工作量至少要降到原来的一半, 才值得付那两次开销。
constexpr double TRIM_MAX_WORK_RATIO = 0.5;

double ratio_trimmed_checked(const std::vector<CodePoint>& a,
                             const std::vector<CodePoint>& b,
                             SequenceMatcher& sm) {
    if (a.size() < MIN_TRIM_LEN || b.size() < MIN_TRIM_LEN) {
        return sm.ratio();
    }
    // 估算裁剪后的工作量比例 (n*m 上界), 太高说明裁剪没意义 -> 回退
    const double la = static_cast<double>(a.size());
    const double lb = static_cast<double>(b.size());
    const double total = la * lb;
    if (total <= 0) return sm.ratio();

    // 求公共前后缀长度 (与 ratio_trimmed 内部算法一致)
    const size_t lim = std::min(a.size(), b.size());
    size_t p = 0;
    while (p < lim && a[p] == b[p]) ++p;
    size_t s = 0;
    while (s < (lim - p) &&
           a[a.size() - 1 - s] == b[b.size() - 1 - s]) ++s;

    const double mid_a = static_cast<double>(a.size() - p - s);
    const double mid_b = static_cast<double>(b.size() - p - s);
    if ((mid_a * mid_b) / total > TRIM_MAX_WORK_RATIO) {
        return sm.ratio();
    }
    return sm.ratio_trimmed();
}

CompareResult compare(const CompareInput& in) {
    CompareResult res;

    // ---- STEP 1: 快字段 diff ----
    // 比 title / status_code / technologies / redirect 四个字段。
    //
    // redirect (装的是 httpx 跟随重定向后的最终落点) 有特殊语义:
    // 它**可以缺失** —— 2xx 响应里 httpx 没有这个字段; 快照表的该列来自
    // 上一轮, 而快照跑在 import 之前, 复制的是还没被本轮填充的旧值,
    // 可能整列为空。若按"两侧不等就判变更"处理, 缺失值与有效值不等会
    // 直接命中 -> 每一行都被判 fast_field -> 假变更 (其他快字段全同、
    // body 逐字节相同也照样被误判)。所以: 任一侧缺失即跳过该字段,
    // 两侧都有值才比较。状态码是独立的快字段, 2xx <-> 3xx 的变化由它
    // 捕获, 不会漏。
    //
    // content_length **不**在这里比: 它与 STEP 3 的长度快筛判据完全重合
    // (同一个量、同一个 body 实际长度、同一个阈值), 在这里再判一遍是纯
    // 冗余。统一由 STEP 3 负责长度关卡 —— 差异够大才判变更, 差异不大的
    // 留给后面的 body 比较与三包分支 (判"动态噪声 vs 真实变更")。
    if (in.prev_title != in.curr_title ||
        in.prev_status_code != in.curr_status_code ||
        in.prev_technologies != in.curr_technologies ||
        (!in.prev_redirect.empty() && !in.curr_redirect.empty() &&
         in.prev_redirect != in.curr_redirect)) {
        res.change_type = 2;
        res.ratio = 0.0;
        res.kind = kind::FAST_FIELD;
        return res;
    }

    // ---- STEP 2: body 字节相同? ----
    if (!in.prev_body.empty() && !in.curr_body.empty() &&
        sha256_hex(in.prev_body) == sha256_hex(in.curr_body)) {
        res.change_type = 0;
        res.ratio = 1.0;
        res.kind = kind::BODY_IDENTICAL;
        return res;
    }

    // ---- STEP 3: 长度快筛 ----
    // 长度差异超过阈值 (30%) 即判为变更; 低于阈值则长度不构成信号, 继续
    // 走后面的 body 比较与三包判定 —— 动态噪声由三包的位置对应覆盖率判。
    //
    // 长度只看 body 的实际字节长度 (后续所有比较都基于它)。body 任一为空
    // 时不做长度判断, 直接往下走 —— content_length 是 HTTP 响应头里的值,
    // 与 body 实际长度可能有偏差, 且在 body 缺失的行上从不出现"两侧不
    // 等"的情况, 用它判断没有意义。
    //
    // 本步骤之后不再有任何 content_length 参与判定, 余下的行一律交给
    // body 比较与三包判定。
    if (!in.prev_body.empty() && !in.curr_body.empty()) {
        size_t mx = std::max(in.prev_body.size(), in.curr_body.size());
        size_t mn = std::min(in.prev_body.size(), in.curr_body.size());
        if (mx > 0) {
            double diff_ratio = static_cast<double>(mx - mn) / static_cast<double>(mx);
            if (diff_ratio > LENGTH_DIFF_THRESHOLD) {
                res.change_type = 2;
                res.ratio = 1.0 - diff_ratio;   // 仍是相似度口径, 供下游展示
                res.kind = kind::BODY_LENGTH_MISMATCH;
                return res;
            }
        }
    }

    // ---- STEP 8 (前置): 超长降级 ----
    // body 过大时不再做 O(n*m) 的 SequenceMatcher, 直接按长度比粗判。
    // 这是 C++ 侧独有的保护 (Python 版没有), 生产库 body 可达 10KB+。
    if (in.prev_body.size() > MAX_BODY_BYTES ||
        in.curr_body.size() > MAX_BODY_BYTES) {
        double ratio_len = 0.0;
        size_t mx = std::max(in.prev_body.size(), in.curr_body.size());
        if (mx > 0) {
            ratio_len = static_cast<double>(std::min(in.prev_body.size(),
                                                     in.curr_body.size())) /
                        static_cast<double>(mx);
        }
        res.ratio = ratio_len;
        res.change_type = (ratio_len > UPPER_RATIO) ? 0 : 2;
        res.kind = kind::BODY_TOO_LONG;
        return res;
    }

    // ---- STEP 5 (先于投影): 动态字段 mask ----
    // 2026-10-25: 整段停用 (用户决策)。原因:
    //   1) mask 按 (host, path, length) 启发式匹配, 有 MAX_MASKABLE_LENGTH=64
    //      上限, 处理不了更长的动态块 —— 实测某 XML 响应里 884 字节的
    //      诊断块每次都变, 永远不会被 mask。
    //   2) mask 掉 RequestId 之类的短动态字段后, 剩余差异区变成"唯一一大块",
    //      而 find_anchors 需要 >= 2 个稳定块才能夹住动态区 -> 返回空 ->
    //      差异整段裸露 -> ratio 从 1.000 掉到 0.472 -> 误判为变更。
    //      mask 与 anchors 是两条并行降噪路径, 且互相破坏。
    //   3) 动态噪声改由三包 (同一时刻连发 2 次) 判定, 见 STEP 7 前的 3 包分支。
    // 保留代码以便回滚; 需要时把下面两行取消注释即可。
    std::string prev_masked = in.prev_body;
    std::string curr_masked = in.curr_body;
    // std::string prev_masked = mask_dynamic_fields(in.prev_body, in.content_type,
    //                                                in.length_stats);
    // std::string curr_masked = mask_dynamic_fields(in.curr_body, in.content_type,
    //                                                in.length_stats);

    // ---- STEP 4: Content-Type 路由投影 ----
    std::string prev_proj = project(prev_masked, in.content_type);
    std::string curr_proj = project(curr_masked, in.content_type);

    // 记录本轮观测到的叶值 (供 field_length_stats 写回)
    res.observed_pairs = project_pairs(in.curr_body, in.content_type);

    // ---- STEP 6: anchors (跨天累积) ----
    std::vector<Anchor> anchors = in.learned_anchors;
    if (anchors.empty()) {
        // 现场发现: 优先在 **未 mask** 的投影上找锚点
        // (Python: find_anchors(prev_proj, curr_proj), prev_proj 此时已 mask)
        anchors = find_anchors(prev_proj, curr_proj);
    }
    res.learned_anchors = anchors;
    if (!anchors.empty()) {
        prev_proj = apply_anchors(prev_proj, anchors);
        curr_proj = apply_anchors(curr_proj, anchors);
    }

    // ---- STEP 7 (前置): 三包噪声提取 ----
    // 2026-10-25: 灰区行若拿到了三包 (curr_body + 同一时刻连发的 2 次),
    // 用位置对应覆盖率判定"动态噪声 vs 真实变更", 不再依赖 ratio 阈值。
    if (!in.pkg2_body.empty() && !in.pkg3_body.empty()) {
        ThreePkgResult tp = evaluate_three_pkg(in.curr_body, in.pkg2_body,
                                                in.pkg3_body, in.prev_body);
        if (tp.applicable) {
            res.ratio = tp.pos_cov;
            res.observed_pairs = project_pairs(in.curr_body, in.content_type);
            if (tp.unchanged) {
                res.change_type = 0;
                res.kind = kind::THREE_PKG_SAME;
                return res;
            }
            res.change_type = 2;
            res.kind = kind::THREE_PKG_DIFF;
            return res;
        }
        // 三包不齐 (长度不一致等) -> 落到下面的 2 包逻辑
    }

    // ---- STEP 7: 相似度 + 三段阈值 ----
    std::vector<CodePoint> a = decode_utf8(prev_proj);
    std::vector<CodePoint> b = decode_utf8(curr_proj);
    // Python compute.py 用 autojunk=False
    SequenceMatcher sm(a, b, /*autojunk=*/false);
    double ratio = ratio_trimmed_checked(a, b, sm);

    if (ratio > UPPER_RATIO) {
        res.change_type = 0;
        res.ratio = ratio;
        res.kind = kind::BODY_SIMILAR;
        return res;
    }
    if (ratio < LOWER_RATIO) {
        res.change_type = 2;
        res.ratio = ratio;
        res.kind = kind::BODY_DIFFERENT;
        return res;
    }

    // 灰区: 依赖 baseline
    if (!in.baseline_exists) {
        res.change_type = 2;
        res.ratio = ratio;
        res.kind = kind::BODY_AMBIGUOUS_FIRST;
        res.enqueue_rescan = true;
        return res;
    }
    double drift = std::abs(ratio - in.baseline_ratio);
    if (drift > DIFF_TOLERANCE) {
        res.change_type = 2;
        res.ratio = ratio;
        res.kind = kind::BODY_AMBIGUOUS_DRIFT;
        res.enqueue_rescan = true;
        return res;
    }
    res.change_type = 0;
    res.ratio = ratio;
    res.kind = kind::BODY_AMBIGUOUS_STABLE;
    return res;
}

}  // namespace algo_diff_cpp
