#!/usr/bin/env python3
"""
gen_b2_cases.py — 生成 blake2b 差分测试用例 (含边界长度)

覆盖: 空串、1 字节、127/128/129 字节 (BLAKE2b 分块边界)、长串、UTF-8。
"""
import base64
import sys

CASES = [
    b"",
    b"a",
    b"ab",
    b"abc",
    b"\x00",
    b"\x00" * 127,
    b"\x00" * 128,
    b"\x00" * 129,
    b"\xff" * 128,
    b"A" * 255,
    b"A" * 256,
    b"A" * 257,
    b"the quick brown fox jumps over the lazy dog",
    "中文测试内容".encode("utf-8"),
    "emoji 😀 test".encode("utf-8"),
    b"0123456789" * 100,
]


def main() -> int:
    out = sys.argv[1] if len(sys.argv) > 1 else "b2cases.txt"
    with open(out, "w", encoding="ascii") as fh:
        for c in CASES:
            # 加 "B64 " 前缀: 否则空串用例编码后是空行, 会被读取端
            # 的 "跳过空行" 逻辑吃掉, 导致索引错位。
            fh.write("B64 " + base64.b64encode(c).decode("ascii") + "\n")
    print(f"wrote {len(CASES)} cases to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
