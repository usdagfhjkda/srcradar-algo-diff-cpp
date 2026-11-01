#!/usr/bin/env bash
#
# srcradar-algo-diff-cpp/install.sh — 装本仓 (变更判定引擎 C++ 实现)
#
# 仓库组成:
#   - src/*.cpp / .hpp        C++ 算法实现
#   - vendor/rapidfuzz/       header-only vendored (rapidfuzz-cpp)
#   - vendor/simdjson/        vendored (simdjson 3.x singleheader amalgamation)
#   - CMakeLists.txt          编译配置
#   - bin/algo_diff_cpp       编译产物 (本脚本生成)
#   - lib/algo_diff_reverify.py  T1/T2 reverify 阶段的 Python 编排器
#                              (C++ 通过 --reverify-helper-path 定位)
#
# 装什么:
#   1. apt deps: libsqlite3-dev / zlib1g-dev / libsimdjson-dev / build-essential
#      (g++ + make, Ubuntu 默认装)
#   - cmake (build-essential 不带, 单独装)
#   2. vendor/rapidfuzz/ + vendor/simdjson/ 拉取 (git clone, tag pin)
#   3. cmake build: 在 ./build/ 子目录编译, 产物拷到 ./bin/algo_diff_cpp
#
# 用法:
#   bash install.sh              # 默认:check_deps + apt install -y + vendor + cmake build
#   bash install.sh --check      # 只 check_deps + verify_input.sh, 不编译
#   bash install.sh --uninstall  # 删 ./build + ./bin/algo_diff_cpp, 不动 apt
#   bash install.sh --rebuild    # 删 ./build 后重编 (vendor/rapidfuzz 变了等场景)
#   bash install.sh --vendor-reset # 删 ./vendor/{rapidfuzz,simdjson} 后重拉
#   bash install.sh --yes        # apt-get install 加 -y (非交互, CI 用)
#   bash install.sh --rebuild --yes  # 组合: 重建 + 非交互 apt
#
# 退出码:
#   0 = OK
#   2 = 缺 system dep
#   3 = apt install 失败
#   4 = cmake configure 失败
#   5 = make build 失败
#   6 = vendor clone 失败
#
# 依赖 system 包 (apt):
#   - libsqlite3-dev    SQLite C API
#   - zlib1g-dev        zlib compression (Python zlib.compress 同版本)
#   - cmake / build-essential
#
# 注意: simdjson 不走 apt —— Ubuntu 22.04 jammy 只有 1.0.2, 其 ondemand API
#   与 3.x 差异很大 (json_type vs value_type, get() 指针 vs 引用语义),
#   且缺 get_number_type() / raw_json_token(), 无法表达 Python json.loads
#   需要的 big_integer 与原始 token。改用 vendored 3.9.5 (见 vendor/README.md)。
#
# 不引入 (deliberately):
#   - nlohmann/json / libcurl / openssl / libzstd / spdlog / fmt / abseil /

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

BUILD_DIR="${SCRIPT_DIR}/build"
BIN_DIR="${SCRIPT_DIR}/bin"
TARGET_BIN="${BIN_DIR}/algo_diff_cpp"
VENDOR_DIR="${SCRIPT_DIR}/vendor/rapidfuzz"
RAPIDFUZZ_REPO="https://github.com/rapidfuzz/rapidfuzz-cpp.git"
RAPIDFUZZ_COMMIT="v3.3.2"  # pin 一次, 升级手动改这里

# simdjson: JSON 解析用的 vendored 3.x singleheader amalgamation
# (不是 header-only, 需要编 simdjson.cpp)
SIMDJSON_DIR="${SCRIPT_DIR}/vendor/simdjson"
SIMDJSON_REPO="https://github.com/simdjson/simdjson.git"
SIMDJSON_TAG="v3.9.5"  # pin 一次, 升级手动改这里

log()  { printf "[algo_diff_cpp] %s\n" "$*"; }
warn() { printf "[algo_diff_cpp][warn] %s\n" "$*" >&2; }
err()  { printf "[algo_diff_cpp][err]  %s\n" "$*" >&2; }

usage() {
    sed -n '2,30p' "${SCRIPT_DIR}/$(basename "${BASH_SOURCE[0]}")"
}

check_deps() {
    local miss=""
    command -v g++     >/dev/null 2>&1 || miss="$miss g++"
    command -v cmake   >/dev/null 2>&1 || miss="$miss cmake"
    command -v make    >/dev/null 2>&1 || miss="$miss make"
    command -v git     >/dev/null 2>&1 || miss="$miss git"
    [ -e /usr/include/sqlite3.h ]    || miss="$miss libsqlite3-dev"
    [ -e /usr/include/zlib.h ]       || miss="$miss zlib1g-dev"
    [ -e /usr/include/simdjson.h ]   || miss="$miss libsimdjson-dev"
    if [ -n "$miss" ]; then
        err "缺少 system dep:$miss"
        err "  sudo apt-get install -y$miss"
        return 2
    fi
    log "all system deps present"
    return 0
}

apt_install() {
    local y_flag=""
    if [ "${YES:-0}" = "1" ]; then y_flag="-y"; fi
    log "apt-get update + install deps (yes=${YES:-0})..."
    apt-get update -qq
    apt-get install $y_flag --no-install-recommends \
        build-essential cmake git \
        libsqlite3-dev zlib1g-dev
    log "apt install done"
}

clone_vendor() {
    log "vendor/rapidfuzz/ clone ($RAPIDFUZZ_COMMIT)..."
    if [ -d "$VENDOR_DIR/.git" ]; then
        log "  vendor already cloned, checking pin..."
        local cur
        cur="$(git -C "$VENDOR_DIR" describe --tags --always 2>/dev/null || echo unknown)"
        if [ "$cur" != "$RAPIDFUZZ_COMMIT" ]; then
            warn "  vendor pin mismatch (have=$cur want=$RAPIDFUZZ_COMMIT), reset"
            rm -rf "$VENDOR_DIR"
        else
            log "  vendor pin ok"
            return 0
        fi
    fi
    git clone --branch "$RAPIDFUZZ_COMMIT" --depth 1 "$RAPIDFUZZ_REPO" "$VENDOR_DIR" \
        || { err "vendor clone failed"; return 6; }
    log "  vendor cloned to $VENDOR_DIR"

    log "vendor/simdjson/ clone ($SIMDJSON_TAG)..."
    if [ -d "$SIMDJSON_DIR/.git" ]; then
        log "  vendor already cloned, checking pin..."
        local cur
        cur="$(git -C "$SIMDJSON_DIR" describe --tags --always 2>/dev/null || echo unknown)"
        if [ "$cur" != "$SIMDJSON_TAG" ]; then
            warn "  vendor pin mismatch (have=$cur want=$SIMDJSON_TAG), reset"
            rm -rf "$SIMDJSON_DIR"
        else
            log "  vendor pin ok"
            return 0
        fi
    fi
    git clone --branch "$SIMDJSON_TAG" --depth 1 "$SIMDJSON_REPO" "$SIMDJSON_DIR" \
        || { err "simdjson vendor clone failed"; return 6; }
    # 只取 singleheader (amalgamation: 一个 .h + 一个 .cpp), 其余源码用不到。
    #
    # 注意: cp 的源和目标是同一文件时返回非 0 ("are the same file"),
    # 在 set -euo pipefail 下会中断整个脚本 —— 每次 clone 都会命中
    # (simdjson 仓库根目录的 LICENSE 已经在目标位置), 所以每步都要兜底。
    if [ ! -f "$SIMDJSON_DIR/singleheader/simdjson.h" ] || \
       [ ! -f "$SIMDJSON_DIR/singleheader/simdjson.cpp" ]; then
        err "simdjson singleheader missing (clone 不完整)"
        return 6
    fi
    cp -f "$SIMDJSON_DIR/singleheader/simdjson.h" "$SIMDJSON_DIR/simdjson.h" \
        || warn "  simdjson.h copy skipped (already in place)"
    cp -f "$SIMDJSON_DIR/singleheader/simdjson.cpp" "$SIMDJSON_DIR/simdjson.cpp" \
        || warn "  simdjson.cpp copy skipped (already in place)"
    log "  vendor cloned to $SIMDJSON_DIR"
}

do_build() {
    log "cmake configure..."
    mkdir -p "$BUILD_DIR"
    (cd "$BUILD_DIR" && cmake .. >/dev/null) \
        || { err "cmake configure failed"; return 4; }
    log "make -j$(nproc)..."
    (cd "$BUILD_DIR" && make -j"$(nproc)") \
        || { err "make build failed"; return 5; }
    mkdir -p "$BIN_DIR"
    if [ ! -f "$BUILD_DIR/algo_diff_cpp" ]; then
        err "make 产物 $BUILD_DIR/algo_diff_cpp 不存在"
        return 5
    fi
    cp -f "$BUILD_DIR/algo_diff_cpp" "$TARGET_BIN"
    chmod +x "$TARGET_BIN"
    log "build done: $TARGET_BIN"
}

do_uninstall() {
    log "removing $BUILD_DIR and $TARGET_BIN (apt packages untouched)"
    rm -rf "$BUILD_DIR" "$TARGET_BIN"
    log "uninstall done"
}

do_check() {
    check_deps || return 2
    if [ ! -f "$VENDOR_DIR/rapidfuzz/fuzz.hpp" ]; then
        err "vendor/rapidfuzz/rapidfuzz/fuzz.hpp 不存在 (跑 install.sh 会拉)"
        return 0
    fi
    if [ ! -f "$SIMDJSON_DIR/simdjson.h" ] || [ ! -f "$SIMDJSON_DIR/simdjson.cpp" ]; then
        err "vendor/simdjson/{simdjson.h,simdjson.cpp} 不存在 (跑 install.sh 会拉)"
        return 0
    fi
    log "check ok (vendor ok; not built)"
}

main() {
    local action="install"
    YES="${YES:-0}"
    while [ $# -gt 0 ]; do
        case "$1" in
            --check)         action="check";;
            --uninstall)     action="uninstall";;
            --rebuild)       action="rebuild";;
            --vendor-reset)  action="vendor-reset";;
            --yes)           YES=1;;
            -h|--help)       usage; return 0;;
            *) err "unknown arg: $1"; usage; return 2;;
        esac
        shift
    done

    case "$action" in
        check)
            do_check
            ;;
        uninstall)
            do_uninstall
            ;;
        vendor-reset)
            rm -rf "$VENDOR_DIR" "$SIMDJSON_DIR"
            log "vendor reset"
            clone_vendor
            ;;
        rebuild)
            rm -rf "$BUILD_DIR"
            check_deps || { apt_install || return 3; check_deps || return 2; }
            clone_vendor || return 6
            do_build
            ;;
        install)
            check_deps || { apt_install || return 3; check_deps || return 2; }
            clone_vendor || return 6
            do_build
            ;;
    esac
}

main "$@"