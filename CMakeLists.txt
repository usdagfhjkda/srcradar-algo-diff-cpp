# modules/public/algo_diff_cpp/CMakeLists.txt
#
# C++17, 编译产物: ./bin/algo_diff_cpp
#
# system deps (apt): libsqlite3-dev zlib1g-dev libsimdjson-dev
# vendored:          vendor/rapidfuzz/ (rapidfuzz-cpp header-only)
#
# 编译: cmake -S . -B build && make -C build -j$(nproc)

cmake_minimum_required(VERSION 3.10)
project(algo_diff_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -O3)

# ---- system: SQLite ----
find_package(SQLite3 REQUIRED)
include_directories(${SQLite3_INCLUDE_DIRS})

# ---- system: zlib ----
find_package(ZLIB REQUIRED)

# ---- vendored: simdjson 3.9.5 (singleheader amalgamation) ----
# 为什么 vendored 而不用 apt:
#   Ubuntu 22.04 jammy 的 libsimdjson-dev 只有 1.0.2, 其 ondemand API 与
#   3.x 差异很大 (json_type vs value_type, get() 指针 vs 引用语义),
#   且 1.x 的 ondemand 缺 get_number_type() / raw_json_token(),
#   无法表达 Python json.loads 需要的 big_integer 与原始 token。
#   3.9.5 提供这些能力, 且 vendor 方式与本模块已有的 rapidfuzz 一致
#   (见 vendor/README.md)。
# simdjson 是 header + 一个 .cpp (amalgamation), 需要一起编译。
set(SIMDJSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/simdjson")
if(NOT EXISTS "${SIMDJSON_DIR}/simdjson.h" OR NOT EXISTS "${SIMDJSON_DIR}/simdjson.cpp")
    message(FATAL_ERROR "vendor/simdjson/ missing (run install.sh, or see vendor/README.md)")
endif()
include_directories(${SIMDJSON_DIR})

# ---- vendored: rapidfuzz-cpp (header-only) ----
set(RAPIDFUZZ_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/rapidfuzz")
if(NOT EXISTS "${RAPIDFUZZ_DIR}/rapidfuzz/fuzz.hpp")
    message(FATAL_ERROR "vendor/rapidfuzz/rapidfuzz/fuzz.hpp missing")
endif()
include_directories(${RAPIDFUZZ_DIR})

# ---- sources ----
file(GLOB ALGO_DIFF_SOURCES CONFIGURE_DEPENDS src/*.cpp)

# vendored simdjson 的 amalgamation 实现 (需单独编译, 不是 header-only)
add_executable(algo_diff_cpp ${ALGO_DIFF_SOURCES} ${SIMDJSON_DIR}/simdjson.cpp)

target_link_libraries(algo_diff_cpp
    PRIVATE
        SQLite::SQLite3
        ZLIB::ZLIB
        pthread
)

# No static init, since /var/cache may be needed at runtime by daemon wrapper.
# No install() rule — bin/algo_diff_cpp stays inside the project checkout.