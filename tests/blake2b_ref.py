#!/usr/bin/env python3
"""
blake2b_ref.py — Python 侧参考 (hashlib.blake2b(d, digest_size=8))

用法: python3 blake2b_ref.py <case_file>
  case_file 每行一个 base64 编码的待哈希文本。
  输出: "<index>\t<hex>", 与 C++ 的 blake2b_check 输出格式一致。
"""
import base64
import hashlib
import sys


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: blake2b_ref.py <case_file>", file=sys.stderr)
        return 2
    idx = 0
    with open(sys.argv[1], "r", encoding="ascii") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line.startswith("B64 "):
                continue
            data = base64.b64decode(line[4:])
            digest = hashlib.blake2b(data, digest_size=8).digest()
            print(f"{idx}\t{digest.hex()}")
            idx += 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
