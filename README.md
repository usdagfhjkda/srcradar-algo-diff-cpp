# srcradar-algo-diff-cpp — 变更判定算法核心 (C++ 实现)

> **模块定位**:srcradar 主仓的独立插件仓。对每个资产 (子域 + 端口) 决定
> `change_type` (ct), 即"跟昨天相比变了没有"。主仓在 daily_monitor 阶段通过
> CLI 调用本模块产出的二进制, 两者不共享源码。

## 导航

- **本文档**:§安装 / §调用方式 / §命令行参数 / §算法流水线 / §已知限制 / §License
- **源码**:`src/algo_core.cpp` (8 步比对) · `src/state_machine.cpp` (T1–T4 状态机) · `src/pipeline.cpp` (编排)
- **上游声明**:逐项来源与许可见 [NOTICE](./NOTICE); 许可证全文见 [LICENSE](./LICENSE)

## 存在意义

- 大规模生产库 (数十万行 × 10KB+ body) 下, SequenceMatcher + zlib inflate
  用 C++ + 快速 JSON parser 替代 Python 的 difflib + json.loads
- 与 srcradar 主仓的 Python 参考实现字节级等价的输出
  (body 的 zlib 格式由 Python 侧写入: `zlib.compress(level=6)`; C++ 侧只
  做 inflate, ct = 0/1/2/4/6/8, kind 字符串一致)
- Python 版在 daily_monitor 里已停用 (调用被注释), 仅作参考实现保留

## 安装

一次性本机编译:

```bash
cd srcradar-algo-diff-cpp
bash install.sh                # 装 apt deps + cmake build
bash install.sh --check        # 只查依赖 + 自检, 不编译
bash install.sh --rebuild      # 强制重新编译
bash install.sh --uninstall    # 删除 build/ + bin/ (不动 apt packages)
bash install.sh --vendor-reset # 重新拉 vendor (rapidfuzz / simdjson)
```

## 调用方式

```bash
./bin/algo_diff_cpp <db_path>
./bin/algo_diff_cpp <db_path> --business-id 1 --batch 5000
./bin/algo_diff_cpp <db_path> --run-id 20261025-030000
```

主仓 daily_monitor 阶段 1.4 的实际调用 (含三包参数):

```bash
cd <module>/bin && \
  PDTMCONF=... HTTPX_BIN=... \
  ./algo_diff_cpp "$DB" --run-id "$RUN_ID" \
    --httpx-path ... --resolvers-path ... --fetch-samples N
```

## 命令行参数

| 参数 | 说明 |
| --- | --- |
| `--business-id N` | 只处理单业务 (默认 0 = 全表) |
| `--batch N` | UPDATE batch 尺寸 (默认 5000) |
| `--dry-run` | 只跑不写 |
| `--skip-reverify` | 跳过 T1/T2 reverify (test only) |
| `--run-id ID` | run_id for logs (默认 timestamp) |
| `--reverify-helper-path P` | T1/T2 用的 Python 编排器路径 |
| `--httpx-path P` | httpx 二进制路径 (三包取样时直接起它; 不启用三包时只透传给 T1/T2 的 Python helper) |
| `--resolvers-path P` | 解析器列表路径 (三包取样用) |
| `--fetch-samples N` | 三包方案: 额外取样次数 (0 = 关闭, 默认) |

环境变量兜底 (命令行未给时依次读): `HTTPX_BIN` / `RESOLVERS_FILE` / `FETCH_SAMPLES` / `PDTMCONF`
(`PDTMCONF` 指向的配置文件里读 `httpx_bin` / `resolvers_file`)。

## 退出码

| 码 | 含义 |
| --- | --- |
| 0 | OK |
| 2 | 参数错误 |
| 3 | schema validation 失败 (web_subdomains / web_subdomains_pre 缺列等) |
| 4 | zlib inflate 失败 (fail-fast) |
| 5 | batch UPDATE 失败 |
| 6 | T1/T2 reverify helper 失败 |

## 算法流水线

编排见 `src/pipeline.cpp`:

| 阶段 | 转移 | 说明 |
| --- | --- | --- |
| T3 fast transition | `ct=0` → 0/1/10/12 | 纯函数, `state_machine.cpp` |
| T1 reverify (phase C) | `ct=12` → 10/13 | spawn Python helper |
| T2 reverify (phase D) | `ct=13` → 10/8 | spawn Python helper |
| 三包取样 (可选) | `ct=10` 的行补采 2 份样本 | |
| T4 A 算法 | `ct=10` → 0/2/4/6 | 相似度 + 三段阈值 |

算法核心 8 步 (详见 `src/algo_core.cpp` 与 `src/state_machine.cpp`):

- STEP 1 快字段 diff (title/status_code/redirect/technologies/content_length)
- STEP 2 body 字节相同
- STEP 3 长度快筛
- STEP 4 Content-Type 路由投影 (JSON/HTML/XML/raw)
- STEP 5 动态字段 mask (当前整段停用, 见下)
- STEP 6 anchors (跨天累积)
- STEP 7 三包噪声提取 (可选) + 相似度 + 三段阈值
- STEP 8 超长降级 (MAX_BODY_BYTES)

### 与 Python 参考实现的差异

| | 实现 |
| --- | --- |
| Python | `difflib.SequenceMatcher` (Ratcliff-Obershelp) + `json.loads` + `zlib.decompress` |
| C++ | `difflib.cpp` (Python difflib.SequenceMatcher 的 C++ 移植, 逐行对照 CPython 3.10 实现) + `simdjson::ondemand` + libz inflate |

输出 ct 字节级一致 (0/1/2/4/6/8), kind 字符串一致:

`cold_start` / `first_run` / `fast_field` / `body_identical` /
`body_length_mismatch` / `body_structurally_similar` /
`body_structurally_different` / `body_ambiguous_first_seen` /
`body_ambiguous_drift` / `body_ambiguous_but_stable` / `body_too_long` /
`three_pkg_same` / `three_pkg_different`

### 三包方案

`--fetch-samples N > 0` 时启用。同一时刻对同一批 URL 连发 N 次, 连同原扫描样本
构成多包, 用"位置对应覆盖率"区分动态噪声与真实变更:

```
stable  = matching_blocks(pkg1,pkg2) ∩ matching_blocks(pkg1,pkg3)
pos_cov = |{i ∈ stable : pre[i] == pkg1[i]}| / |stable|
pos_cov ≥ 0.95 -> 未变
```

取样实现见 `http_fetch.cpp`, 每轮起一个 httpx 子进程处理整批 URL
(绝不在 URL 循环里逐个调), 响应 jsonl 用 vendored simdjson 解析。

样本存 `web_subdomains.curr_body_1` / `curr_body_2` (zlib BLOB), 该两列由
srcradar 主仓的迁移脚本幂等创建, 且必须在调用本模块之前完成。

### STEP 5 动态字段 mask 的现状

整段停用 (代码保留, 调用处注释)。原因:

1. mask 按 (host, path, length) 启发式匹配, 有长度上限, 处理不了更长的动态块
2. mask 掉短动态字段后, 剩余差异区变成"唯一一大块", 而 anchors 需要两个以上
   稳定块才能夹住动态区 → 找不到锚点 → 差异整段裸露 → 相似度明显偏低 →
   实际没变的资产被判成变更
3. 动态噪声改由三包方案判定

恢复方法: 取消 `algo_core.cpp` 里对应两行的注释即可。

## 限制

- 仅在 `install.sh` 跑过的本机能跑 (本机编译产物 bind 在 `build/`)
- 不引入 libcurl/openssl (httpx 部分由主仓 scanner.sh 的 reverify 路径承担,
  本模块不直连网络)
- 三包方案 (`--fetch-samples N`) 会直接起 httpx 子进程取样, 这时
  `--httpx-path` 指向的二进制会被实际调用; 不启用三包时它只被透传给
  T1/T2 的 Python helper

## 已知限制

### 1. 多模态服务: 采样落在不同后端时产生假变更

数据模型的前提是"一个 (子域, 端口) 只有一行 body", 也就是每个资产只能表达
单一状态。变更判定本质上是比较两天的两次采样。

有些服务不满足这个前提: 同一 URL 背后有多个后端实例, 且各实例的响应配置
不一致 (典型是后端池 / 负载均衡下不同节点的默认页或健康检查端点返回不同
内容)。这类服务的"状态"其实是一个集合, 而库里只能存一个值 —— 于是:

- 两次采样落到不同实例 → body 的长度 / title 等快字段出现差异 →
  STEP 1 (fast_field) 直接判 ct=2, 甚至进不到 body 比较
- 两次采样落到同一实例 → 判 ct=0
- 净效果: 同一资产在不同日期会随机地"有时判变更, 有时判未变"

这类假阳性不能通过改进相似度算法消除 —— 单点采样与多模态服务之间在信息上
不可判定。可选的缓解方向 (均未实现):

1. 采样侧多取几次: 采样时连发 2~3 次, 若内容不一致则把该资产标记为多模态
   并跳过变更判定 (不再产生变更信号)。库里的 `curr_body_1` / `curr_body_2`
   两列正好可以承载这两次额外采样
2. 判定侧做集合包含: 快照侧保留多个历史 body, 只要当前 body 命中其中任意
   一个即判未变
3. 显式多状态模型: 每个资产存一组状态及出现频次, 判定改为"状态集合是否变化"

现状: 采样侧未做多模态检测, 命中的资产会产生持续的假阳性。

### 2. 三包方案不覆盖多模态场景

三包 (同一时刻连发 2 次 + 原扫描样本) 的前提是"同一时刻内的多次采样一致,
变化发生在跨天之间"。它能区分的是"每次都变的动态字段" (时间戳 / 请求 ID /
签名等), 而不是"多后端返回不同内容" —— 后者是跨请求随机的, 同一时刻连发两次
照样可能拿到不同内容, 此时三包长度不一致, 判定会退回两包路径 (见
`evaluate_three_pkg` 的长度一致性检查), 等价于回到单点比较。

### 3. 单子域单状态 (schema 层面的上限)

`web_subdomains` 每行只存一份 `curr_body`, 快照表同理。若要表达"一个资产有
多个合法状态", 需要改 schema 与整条判定链路, 不在当前模块范围内。

## License

GNU General Public License v2.0 — 完整文本见 [LICENSE](./LICENSE)。

本模块的部分算法派生自 sqlmap (GPL-2.0), 因此整体以 GPL-2.0 发布。
逐项来源声明见 [NOTICE](./NOTICE)。
