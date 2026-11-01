#!/usr/bin/env python3
"""
gen_cases.py — 生成 JSON 差分测试用例集 (base64 编码, 每行一个)

覆盖 Python json.loads 的各种边界语义, 用来验证
"vendored simdjson 3.9.5 + python 数值渲染" 是否完全等价。
"""
import base64
import sys

CASES = [
    # ---- 基本类型 ----
    '{}',
    '[]',
    '{"a":1}',
    '{"a":1.5}',
    '{"a":"hello"}',
    '{"a":true}',
    '{"a":false}',
    '{"a":null}',

    # ---- key 排序 (Python sorted(obj.keys())) ----
    '{"b":2,"a":1}',
    '{"z":1,"y":2,"x":3,"w":4}',
    '{"B":1,"a":2,"_":3,"0":4}',

    # ---- 嵌套 ----
    '{"a":{"b":{"c":1}}}',
    '{"a":[{"b":1},{"b":2}]}',
    '{"a":[[1,2],[3,4]]}',
    '{"a":{"b":[1,{"c":2}]}}',

    # ---- 数组 __len__ 哨兵 ----
    '{"r":[1,2,3]}',
    '{"r":[1,2,3,4]}',
    '{"r":[]}',
    '{"r":[[1],[2],[3]]}',

    # ---- 浮点 repr 语义 (Python 特有) ----
    '{"a":1.0}',
    '{"a":0.1}',
    '{"a":3.14159}',
    '{"a":1e16}',
    '{"a":1e15}',
    '{"a":1e-05}',
    '{"a":0.0001}',
    '{"a":1.5e16}',
    '{"a":-2.5}',
    '{"a":1e100}',
    '{"a":1e-100}',
    '{"a":2.675}',
    '{"a":0.30000000000000004}',
    '{"a":100.0}',
    '{"a":1e300}',
    '{"a":-0.0}',

    # ---- 整数 ----
    '{"a":0}',
    '{"a":-1}',
    '{"a":9223372036854775807}',
    '{"a":9223372036854775808}',
    '{"a":-9223372036854775808}',
    '{"a":-9223372036854775809}',
    '{"a":123456789012345678901234567890}',
    '{"a":-0}',

    # ---- 字符串转义 ----
    '{"a":"line\\nbreak"}',
    '{"a":"tab\\there"}',
    '{"a":"quote\\"inside"}',
    '{"a":"back\\\\slash"}',
    '{"a":"\\u4e2d\\u6587"}',
    '{"a":"\\ud83d\\ude00"}',
    '{"a":"\\u0000"}',
    '{"a":"mixed \\u4e2d and ascii"}',

    # ---- Unicode ----
    '{"a":"中文"}',
    '{"a":"emoji 😀 test"}',
    '{"名":"值"}',

    # ---- 非 JSON (应两边都 PARSE_FAIL) ----
    'not json',
    '{',
    '{"a":}',
    '[1,2',
    '{"a":1,}',
    "{'a':1}",
    '{"a":01}',
    '{"a":+1}',
    '{"a":.5}',
    '{"a":1.}',
    '{"a":tru}',
    '',
    '   ',
    '{"a":1} trailing',

    # ---- Python 特殊: NaN / Infinity ----
    '{"a":NaN}',
    '{"a":Infinity}',
    '{"a":-Infinity}',

    # ---- 深层嵌套 (递归深度) ----
    '{"a":' + '{"b":' * 30 + '1' + '}' * 30 + '}',
    '{"a":' + '[' * 60 + ']' * 60 + '}',

    # ---- 大数组 ----
    '{"a":[' + ','.join(str(i) for i in range(100)) + ']}',
]


def main() -> int:
    out = sys.argv[1] if len(sys.argv) > 1 else "cases.txt"
    with open(out, "w", encoding="ascii") as fh:
        for c in CASES:
            fh.write(base64.b64encode(c.encode("utf-8")).decode("ascii") + "\n")
    print(f"wrote {len(CASES)} cases to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
