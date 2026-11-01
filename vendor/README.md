# vendor/ — 上游 fork 速查

## rapidfuzz-cpp (header-only C++ 模糊字符串算法)

### 项目地址

https://github.com/rapidfuzz/rapidfuzz-cpp

### 版本

v3.x (commit pin 在 install.sh 的 `RAPIDFUZZ_COMMIT`)

### 引入方式

install.sh 跑 `git clone https://github.com/rapidfuzz/rapidfuzz-cpp.git vendor/rapidfuzz/`
后,把 `vendor/rapidfuzz/include/` 软链 (或直接复制) 到 `vendor/rapidfuzz/include/`。

### 为什么 vendored 不直接 commit

- rapidfuzz-cpp 是 header-only (~200KB header), 不像二进制那么大, **理论上
  可以 commit**。
- 但按 srcradar 现有 vendor 风格 (db_align/ENScan_GO/ 不入 git, install.sh 拉),
  本模块也保持 "vendor 不入 git, install.sh 拉取" 的约定。
- 保持约定一致, 减少 git diff 噪声, 便于版本固定 + 跨机器拉取时同步。

### vendored?

include 目录在 vendor/rapidfuzz/include/rapidfuzz/。 CMakeLists.txt 直接
`-I vendor/rapidfuzz/include` 拿, 无 CMake `FetchContent` (避免网络依赖
到 build 时)。

### 为什么不用 apt 包

仓库首选 system apt (libsqlite3-dev / zlib1g-dev / libsimdjson-dev 都 apt),
但 rapidfuzz-cpp 不在 Ubuntu 22.04 jammy 主仓库。vendored 是最快路径。

### 未来可选

- 若 Ubuntu 26+ 引入 rapidfuzz-cpp apt 包 → 改用 apt, vendor 退场。
- 若使用频次 > 1 / 跨项目复用 → 提到 `modules/cmake/rapidfuzz.cmake` 共享。

## simdjson-cpp (SIMD JSON parser, 3.x singleheader amalgamation)

### 项目地址

https://github.com/simdjson/simdjson

### 版本

v3.9.5 (tag pin 在 install.sh 的 `SIMDJSON_TAG`)

### 引入方式

install.sh 跑 `git clone --branch v3.9.5 --depth 1 https://github.com/simdjson/simdjson.git vendor/simdjson/`
后,把 `singleheader/simdjson.h` + `singleheader/simdjson.cpp` 拷到 `vendor/simdjson/`
(amalgamation: 一个头文件 + 一个实现文件, 其余源码用不到)。

### 为什么要 vendored 而不用 apt 包

Ubuntu 22.04 jammy 的 `libsimdjson-dev` 只有 **1.0.2**, 其 ondemand API 与 3.x
差异很大 (`json_type` vs `value_type`, `get()` 指针 vs 引用语义), 且 1.x 缺
`get_number_type()` / `raw_json_token()` —— 后两者是 JSON 投影与 Python
`json.loads` 语义对齐所必需 (区分 big_integer / 取原始数字 token)。

按 srcradar 现有 vendor 风格 (vendor 不入 git, install.sh 拉), 本模块保持一致。

### vendored?

`vendor/simdjson/` 在 .gitignore 里, 由 install.sh 拉取。
CMakeLists.txt 直接 `-I vendor/simdjson` 拿头文件, 并把 `simdjson.cpp` 加进
编译单元 (它不是 header-only, 需要单独编一遍)。

### 未来可选

- 若 Ubuntu 26+ 提供 3.x 的 apt 包 → 改用 apt, vendor 退场。
