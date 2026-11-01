#!/usr/bin/env python3
"""
jsondiff_ref.py — Python 侧参考实现 (复刻 mask.py 的 _json_walk + json_project)

用法: python3 jsondiff_ref.py <case_file>
  case_file 每行一个 base64 编码的 JSON 文本。
  输出格式与 C++ 的 jsondiff_test 完全一致:
    <case_index>\t<OK|FAIL>\t<rendered>
  rendered 里的换行用 \x1f 分隔 (保持每 case 一行)。

差分测试: 两个输出逐行 diff, 应当完全一致。
"""
import base64
import json
import sys


def _json_walk(obj, path):
    """复刻 mask.py 的 _json_walk: dict 按 key 排序, list 产 __len__ 哨兵。"""
    if isinstance(obj, dict):
        for key in sorted(obj.keys()):
            yield from _json_walk(obj[key], f"{path}.{key}" if path else f".{key}")
    elif isinstance(obj, (list, tuple)):
        yield f"{path}.__len__", len(obj)
        for index, item in enumerate(obj):
            yield from _json_walk(item, f"{path}[{index}]")
    else:
        yield path, obj


def json_pairs(content):
    if not content or not content.strip():
        return []
    try:
        data = json.loads(content)
    except (ValueError, TypeError, RecursionError):
        return []
    return [(path, str(value)) for path, value in _json_walk(data, "")]


def render(content):
    pairs = json_pairs(content)
    lines = sorted(f"{path}={value}" for path, value in pairs)
    return "\n".join(lines)


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: jsondiff_ref.py <case_file>", file=sys.stderr)
        return 2
    idx = 0
    with open(sys.argv[1], "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            content = base64.b64decode(line).decode("utf-8", errors="replace")
            try:
                json.loads(content)
                status = "OK"
            except (ValueError, TypeError, RecursionError):
                status = "PARSE_FAIL"
            flat = render(content).replace("\n", "\x1f")
            print(f"{idx}\t{status}\t{flat}")
            idx += 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
