// blake2b.cpp — BLAKE2b-64 实现 (RFC 7693)
#include "blake2b.hpp"

#include <cstring>

namespace algo_diff_cpp {

namespace {

const uint64_t kIV[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
    0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};

const uint8_t kSigma[12][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
};

inline uint64_t rotr64(uint64_t x, int n) {
    return (x >> n) | (x << (64 - n));
}

inline uint64_t load64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

void compress(uint64_t h[8], const uint8_t block[128], uint64_t t,
              bool last) {
    uint64_t v[16];
    for (int i = 0; i < 8; ++i) v[i] = h[i];
    for (int i = 0; i < 8; ++i) v[i + 8] = kIV[i];
    v[12] ^= t;
    // v[13] ^= (t >> 64) 在 64 位平台上右移 64 位是 UB。
    // BLAKE2b 的计数器是 128 位, 但单次调用 (< 2^64 字节) 高位恒为 0,
    // 因此不需要 (Python hashlib 内部用两个 64 位字, 这里只用低位)。
    if (last) v[14] = ~v[14];

    for (int r = 0; r < 12; ++r) {
        const uint8_t* s = kSigma[r];
        // G 轮
        auto G = [&](int a, int b, int c, int d, uint64_t x, uint64_t y) {
            v[a] = v[a] + v[b] + x;
            v[d] = rotr64(v[d] ^ v[a], 32);
            v[c] = v[c] + v[d];
            v[b] = rotr64(v[b] ^ v[c], 24);
            v[a] = v[a] + v[b] + y;
            v[d] = rotr64(v[d] ^ v[a], 16);
            v[c] = v[c] + v[d];
            v[b] = rotr64(v[b] ^ v[c], 63);
        };
        G(0, 4, 8, 12, load64(block + s[0] * 8), load64(block + s[1] * 8));
        G(1, 5, 9, 13, load64(block + s[2] * 8), load64(block + s[3] * 8));
        G(2, 6, 10, 14, load64(block + s[4] * 8), load64(block + s[5] * 8));
        G(3, 7, 11, 15, load64(block + s[6] * 8), load64(block + s[7] * 8));
        G(0, 5, 10, 15, load64(block + s[8] * 8), load64(block + s[9] * 8));
        G(1, 6, 11, 12, load64(block + s[10] * 8), load64(block + s[11] * 8));
        G(2, 7, 8, 13, load64(block + s[12] * 8), load64(block + s[13] * 8));
        G(3, 4, 9, 14, load64(block + s[14] * 8), load64(block + s[15] * 8));
    }
    for (int i = 0; i < 8; ++i) h[i] ^= v[i] ^ v[i + 8];
}

}  // namespace

std::string blake2b_64(const std::string& data) {
    uint64_t h[8];
    for (int i = 0; i < 8; ++i) h[i] = kIV[i];

    // 参数块: digest_length=8, key_length=0, fanout=1, depth=1
    // (digest_size 参与参数块 -> digest_size=8 与 digest_size=64 是不同的
    //  哈希, 不是截断关系。必须与 Python hashlib.blake2b(d, digest_size=8)
    //  的参数完全一致, 否则哈希值不同。)
    h[0] ^= 0x01010000ULL ^ 0x08ULL;

    const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
    size_t len = data.size();
    size_t offset = 0;

    // 除最后一个块外, 全部 128 字节整块处理
    while (len - offset > 128) {
        offset += 128;
        compress(h, p + offset - 128, static_cast<uint64_t>(offset), false);
    }

    // 最后一个块: 剩余数据 + 0 填充。
    //
    // padding 规则 (与 Python hashlib.blake2b / libb2 一致, 实测确认):
    //   1. **不补 0x80** —— RFC 7693 2.6 的字面描述是 "1 bit + zeros",
    //      但参考实现 (libb2 / CPython _blake2) 实际不写 0x80, 直接补 0。
    //      差一个 0x80 整个哈希就不同, 必须跟实现走而不是跟 RFC 字面走。
    //   2. 消息长度恰为 128 的整数倍时 **不加额外的 padding 块** ——
    //      满块本身就是最后一块 (libb2 的 update 只在"后面还有数据"时才
    //      提前压缩满块)。
    //   3. 计数 t = 已处理的总字节数。
    uint8_t block[128];
    std::memset(block, 0, sizeof(block));
    size_t rest = len - offset;
    if (rest) std::memcpy(block, p + offset, rest);
    compress(h, block, static_cast<uint64_t>(len), true);

    // digest_size=8 -> 输出 8 字节 (h[0] 的低 64 位, 小端)
    std::string out(8, '\0');
    for (int j = 0; j < 8; ++j) {
        out[static_cast<size_t>(j)] = static_cast<char>((h[0] >> (8 * j)) & 0xFF);
    }
    return out;
}

int64_t blake2b_64_signed_int(const std::string& data) {
    std::string digest = blake2b_64(data);
    uint64_t v = 0;
    for (size_t i = 0; i < digest.size(); ++i) {
        v = (v << 8) | static_cast<uint64_t>(
                             static_cast<unsigned char>(digest[i]));
    }
    return static_cast<int64_t>(v);
}

}  // namespace algo_diff_cpp
