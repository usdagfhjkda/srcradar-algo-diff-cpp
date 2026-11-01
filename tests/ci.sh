#!/usr/bin/env bash
#
# modules/public/algo_diff_cpp/tests/ci.sh — algo_diff_cpp 集成测试 (11 case)
#
# 覆盖 11 种 transition case (用户拍板 2026-10-09):
#   1:  00    a0b0                          → 0   (T3 fast)
#   2:  01    prev_body NULL                → 1   (T3 fast)
#   3:  0A0   a1b1 同 body                 → 0   (T3→10; T4 ratio=1.0)
#   4:  0A2   a1b1 diff body               → 2   (T3→10; T4 gray M.1)
#   5:  0B4   a0b1 复活 同 body            → 4   (T3→10; T4 ratio=1.0 + B=4)
#   6:  0B6   a0b1 复活 diff body          → 6   (T3→10; T4 gray M.1 + B=4)
#   7:  0C0   C mock 同 body               → 0   (T4 ratio=1.0, 跳过 T1)
#   8:  0C2   C mock diff body             → 2   (T4 gray M.1, 跳过 T1)
#   9:  0CD8  D no resp 终态                → 8   (跳过 T2)
#   10: 0CD0  D mock 同 body               → 0   (T4 ratio=1.0, 跳过 T2)
#   11: 0CD2  D mock diff body             → 2   (T4 gray M.1, 跳过 T2)
#
# 直接 mock: 不调 httpx, fixture 预置 T1/T2 跑完后的状态.
#
# 用法:
#   bash tests/ci.sh
#
# 退出码:
#   0 = 全部 case PASS
#   1 = 至少 1 case FAIL
#   2 = 编译产物缺失 (跑 install.sh 先)
#   3 = algo_diff_cpp 运行失败
#   4 = fixture 生成失败

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN="$MODULE_ROOT/bin/algo_diff_cpp"
FIXTURE="$MODULE_ROOT/tests/fixtures/test_ct_cases.db"
# pdtm.conf 路径由调用方提供 (srcradar 主仓的 config/pdtm.conf)。
# 不设默认值: 独立仓无法猜测主仓位置, 未设置时三包取样路径不可用,
# 其余用例仍可运行。
PDTMCONF="${PDTMCONF:-}"
HTTPX_BIN="${HTTPX_BIN:-/usr/local/bin/httpx}"

log()  { printf "[ci] %s\n" "$*"; }
err()  { printf "[ci][err] %s\n" "$*" >&2; }

# 1. 编译产物检查
if [ ! -x "$BIN" ]; then
    err "编译产物缺失: $BIN"
    err "  跑 install.sh: bash install.sh"
    exit 2
fi

# 2. 生成 fixture (11 case, 直接 mock, 无 httpx)
log "generate fixture (11 case, 直接 mock)..."
if ! python3 "$SCRIPT_DIR/fixtures/make_fixture.py" "$FIXTURE"; then
    err "fixture 生成失败"
    exit 4
fi

# 3. 跑 algo_diff_cpp (CWD = bin/, 让 ../lib/algo_diff_reverify.py 解析正确)
log "run algo_diff_cpp (CWD=bin/)..."
if ! (cd "$MODULE_ROOT/bin" && \
      PDTMCONF="$PDTMCONF" HTTPX_BIN="$HTTPX_BIN" \
      "./algo_diff_cpp" "../tests/fixtures/test_ct_cases.db" \
      --business-id 1 --run-id ci 2>&1 | tail -30); then
    err "algo_diff_cpp exit non-zero"
    exit 3
fi

# 4. 验证 11 case
log "verify 11 cases:"
N_PASS=0
N_FAIL=0
while IFS='|' read -r cid name expected; do
    actual=$(sqlite3 "$FIXTURE" "SELECT change_type FROM web_subdomains WHERE id=$cid")
    if [ "$actual" = "$expected" ]; then
        printf "  [PASS] id=%2s case=%-5s actual=%s expected=%s\n" "$cid" "$name" "$actual" "$expected"
        N_PASS=$((N_PASS+1))
    else
        printf "  [FAIL] id=%2s case=%-5s actual=%s expected=%s\n" "$cid" "$name" "$actual" "$expected"
        N_FAIL=$((N_FAIL+1))
    fi
done < <(sqlite3 "$FIXTURE" "SELECT id, case_name, expected_ct FROM _test_meta ORDER BY id")

log "PASS=$N_PASS FAIL=$N_FAIL"
if [ "$N_FAIL" -ne 0 ]; then
    exit 1
fi
exit 0
