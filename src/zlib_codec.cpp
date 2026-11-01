// zlib_codec.cpp — libz wrapper
//
// libz 调用是 C API, 不抛异常。我们用 z_streamInit + inflate + z_streamEnd 三步。
// 算法 v3 (2026-10-08 用户): 失败 → 错误退出 (exit 4), 不容错。

#include "zlib_codec.hpp"
#include <zlib.h>
#include <cstdio>
#include <cstring>

namespace algo_diff_cpp {

bool inflate_zlib(const std::vector<unsigned char>& in,
                  std::vector<unsigned char>& out,
                  std::string& error_msg) {
    if (in.empty()) {
        error_msg = "empty input (curr_body is NULL or 0-byte BLOB)";
        return false;
    }

    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));

    // wbits = MAX_WBITS = 15: 自动识别 zlib header (0x78 0x9C)
    //   与 Python zlib.decompress() 默认行为一致。
    int rc = inflateInit(&strm);
    if (rc != Z_OK) {
        error_msg = std::string("inflateInit: ") + zError(rc);
        return false;
    }

    strm.next_in  = const_cast<Bytef*>(in.data());
    strm.avail_in = static_cast<uInt>(in.size());

    // 动态扩容输出: 初始 64KB, 必要时 x2。
    constexpr size_t CHUNK = 64 * 1024;
    std::vector<unsigned char> tmp;
    tmp.resize(CHUNK);

    while (true) {
        if (strm.total_out >= tmp.size()) {
            tmp.resize(tmp.size() * 2);
        }
        strm.next_out  = tmp.data() + strm.total_out;
        strm.avail_out = static_cast<uInt>(tmp.size() - strm.total_out);

        rc = inflate(&strm, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) break;       // 成功
        if (rc != Z_OK) {
            error_msg = std::string("inflate: ") + zError(rc);
            inflateEnd(&strm);
            return false;
        }
        if (strm.avail_in == 0) break;       // 输入空, 正常结束
    }

    inflateEnd(&strm);
    out.assign(tmp.begin(), tmp.begin() + strm.total_out);
    return true;
}

// ---------------------------------------------------------------- deflate
// 2026-10-25: 3 包方案需要把 httpx 拿到的 body 压缩后写库
// (curr_body_1 / curr_body_2), 与现有 curr_body 的存储格式保持一致。
bool deflate_zlib(const std::vector<unsigned char>& in,
                  std::vector<unsigned char>& out,
                  std::string& error_msg) {
    out.clear();
    if (in.empty()) {
        // 空 body 压缩后是一个合法的空 zlib stream (8 字节)
        out.assign({0x78, 0x9c, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01});
        return true;
    }
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    int rc = deflateInit(&zs, Z_DEFAULT_COMPRESSION);
    if (rc != Z_OK) {
        error_msg = std::string("deflateInit failed: ") + zError(rc);
        return false;
    }
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = static_cast<uInt>(in.size());
    out.resize(deflateBound(&zs, static_cast<uLong>(in.size())) + 32);
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    rc = deflate(&zs, Z_FINISH);
    if (rc != Z_STREAM_END) {
        deflateEnd(&zs);
        error_msg = std::string("deflate failed: ") + zError(rc);
        out.clear();
        return false;
    }
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return true;
}

}  // namespace algo_diff_cpp
