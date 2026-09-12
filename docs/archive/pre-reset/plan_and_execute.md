# mini-trantor 后续任务目标与执行清单

## 0. 背景判断

本文档基于 `assessment.md` 对 mini-trantor 的评估结论整理，重点回应其中提到的两类问题：

1. 工程护栏不足：CI、sanitizer、fuzz、benchmark、install verification 需要从“计划项”变成持续运行的工程事实。
2. 文档与定位漂移：README、目录说明、CMake 内容、当前模块边界之间存在时间差，容易让外部读者误判项目定位与成熟度。

当前仓库已经出现部分 v5-zeta 落地成果：

- `.github/workflows/ci.yml` 已存在。
- `cmake/Sanitizers.cmake` 已存在。
- `intents/architecture/v5_zeta_engineering_guardrails.intent.md` 已存在。
- install + `find_package` 消费验证已进入 CI。
- `tests/integration/benchmark/test_fps_like_broadcast_latency.cpp` 已提供轻量广播延迟基线。

但仍需补强：

- ThreadSanitizer 尚未成为独立护栏。
- Fuzz 入口尚未体系化纳入 CMake。
- Benchmark 仍更像 integration test，缺少稳定指标、分组、阈值与趋势记录。
- README 仍需要从“版本流水账”重构为“定位 + 模块成熟度 + 工程证据”。

---

## 1. 总体目标

下一阶段不优先继续扩展功能，而是把 mini-trantor 从“功能丰富的实验平台”推进到“边界清楚、护栏可信、证据可追踪的 Intent 驱动工程样板”。

核心目标：

1. v5-zeta 工程护栏补齐：让每次变更都绕不过 build、test、sanitizer、install、fuzz/benchmark 入口。
2. README 与文档定位收敛：清楚说明 mini-trantor 是游戏服务端实验取向的现代 C++23 异步网络框架，而不是宣称生产替代 trantor/asio。
3. 模块成熟度显式化：用 Stable / Beta / Experimental 标注模块状态，避免范围膨胀造成误读。
4. 生命周期与线程边界可验证：把 `TcpConnection`、`EventLoop`、coroutine awaiter、跨线程调度、协议解析器等高风险路径纳入专门测试与 sanitizer/fuzz 入口。

---

## 2. Intent 驱动执行原则

每个任务开始前必须回答：

1. 这个任务保护哪个 intent？
2. 它验证哪些 invariant？
3. 它涉及哪些线程归属规则？
4. 它是否改变所有权或生命周期语义？
5. 它对应哪些 contract / integration / fuzz / benchmark 证据？

核心模块变更仍必须回答 change gate：

1. 这个模块归属哪个 loop / thread？
2. 谁拥有它，谁释放它？
3. 哪些回调可能重入？
4. 哪些操作允许跨线程，如何投递？
5. 对应哪个测试文件验证？

---

## 3. 优先级总览

| 优先级 | 阶段目标 | 解决的痛点 | 退出信号 |
| --- | --- | --- | --- |
| P0 | v5-zeta 工程护栏补强 | 生命周期安全、线程边界、CI 可信度、install 消费路径 | 每个 PR 自动跑 build/test/sanitizer/install；TSan 有独立入口；fuzz target 可构建 |
| P1 | README 与模块状态重构 | 定位漂移、范围膨胀、外部读者看不懂当前边界 | README 顶部 1 分钟说清定位；模块状态清单可维护 |
| P2 | 性能基线与长期可靠性 | “能跑”但缺少长期维护证据、压测证据与趋势记录 | benchmark 可独立运行；soak/fuzz 可周期执行；性能报告可追踪 |

---

## 4. P0：v5-zeta 工程护栏补强

### P0-1：CI 矩阵补全

目标：

- 将当前 GitHub Actions 从基本 Debug/Release 构建扩展为更明确的工程护栏矩阵。

建议 job：

- `gcc-debug-asan-ubsan`
- `gcc-release`
- `clang-debug`
- `install-verify`
- `tsan-threading-contract`

执行内容：

- Debug job 跑 unit + contract。
- Release job 跑 unit + contract + integration。
- Clang job 至少跑 configure/build/unit/contract，提前发现编译器差异。
- install job 验证 `cmake --install` + 外部 `find_package(mini_trantor CONFIG REQUIRED)`。

解决痛点：

- 防止不同编译模式或编译器差异掩盖生命周期、模板、链接、头文件安装问题。

Exit Criteria：

- `.github/workflows/ci.yml` 中有清晰 job 拆分。
- 每个 job 名称能直接说明它验证的护栏。
- CI 日志中能看到 unit / contract / integration 的执行边界。

---

### P0-2：ASan / UBSan 配置标准化

目标：

- 统一 sanitizer 开关，避免 CI 通过手写 `CMAKE_CXX_FLAGS` 与本地 `cmake/Sanitizers.cmake` 形成双轨配置。

执行内容：

- 在根 `CMakeLists.txt` 中增加选项：
  - `MINI_ENABLE_ASAN_UBSAN`
  - `MINI_ENABLE_TSAN`
- `MINI_ENABLE_ASAN_UBSAN=ON` 时启用 AddressSanitizer + UndefinedBehaviorSanitizer。
- Debug CI 使用同一 CMake option。
- 文档中记录本地运行命令。

解决痛点：

- ASan 捕获 use-after-free、heap-buffer-overflow、double-free。
- UBSan 捕获 undefined behavior、空指针、对齐错误、整数相关未定义行为。
- 重点保护 `TcpConnection`、`EventLoop`、coroutine awaiter、协议解析状态机。

Exit Criteria：

- 本地可运行：

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMINI_ENABLE_ASAN_UBSAN=ON
cmake --build build-asan -j$(nproc)
ctest --test-dir build-asan --output-on-failure -L "unit|contract"
```

- CI 不再依赖手写全局 sanitizer flags。

---

### P0-3：ThreadSanitizer 独立入口

目标：

- 将线程边界验证从“contract test 假设正确”升级为“TSan 可以抓实际 data race”。

执行内容：

- 新增 `MINI_ENABLE_TSAN=ON`。
- TSan 不与 ASan 混用。
- CI 新增 `tsan-threading-contract` job。
- 首批只跑线程敏感测试集合，避免 CI 过慢。

建议首批测试标签：

- `event_loop`
- `event_loop_thread`
- `event_loop_thread_pool`
- `tcp_server`
- `broadcast`
- `coroutine`
- `logic`

重点覆盖：

- `EventLoop::queueInLoop()` 跨线程唤醒。
- `EventLoopThreadPool::stop()` 生命周期。
- `TcpConnection::send()` 跨线程投递。
- broadcast 分 owner loop fanout。
- coroutine timeout / cancel race。
- `LogicLoop` 与 network owner loop 的回写边界。

解决痛点：

- 直接回应 `assessment.md` 中对线程边界、生命周期安全、跨线程调度的生产级担忧。

Exit Criteria：

- 本地可运行：

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DMINI_ENABLE_TSAN=ON
cmake --build build-tsan -j$(nproc)
ctest --test-dir build-tsan --output-on-failure -L "threading|lifecycle"
```

- CI 有独立 TSan job。
- 若 TSan job 过慢，允许只跑高风险 label，不允许完全跳过。

---

### P0-4：测试标签体系升级

目标：

- 在现有 unit / contract / integration 分层之外，增加风险维度标签。

建议标签：

- `lifecycle`
- `threading`
- `coro`
- `protocol`
- `transport`
- `benchmark`
- `install`

执行内容：

- 扩展 `tests/CMakeLists.txt` 的 `add_mini_test()`，支持附加 label。
- 为高风险测试补充标签。
- 更新 `tests/README.md`。

解决痛点：

- 当前只能按层级跑测试，无法精准运行生命周期、线程、协议解析等风险集合。

Exit Criteria：

- 可运行：

```bash
ctest --test-dir build --output-on-failure -L lifecycle
ctest --test-dir build --output-on-failure -L threading
ctest --test-dir build --output-on-failure -L protocol
```

---

### P0-5：Fuzz 测试入口落地

目标：

- 对协议解析器和帧解析器建立 libFuzzer 入口，优先发现畸形输入导致的崩溃、越界、无限循环、状态机污染。

建议目录：

```text
tests/fuzz/
  http/fuzz_http_context.cpp
  ws/fuzz_ws_codec.cpp
  rpc/fuzz_rpc_codec.cpp
  framing/fuzz_packet_framer.cpp
  corpus/
    http/
    ws/
    rpc/
    framing/
```

首批 fuzz target：

- HTTP：`HttpContext` 增量解析。
- WebSocket：`WebSocketCodec` 帧解析、mask、payload length。
- RPC：`RpcCodec` 长度前缀、request/response frame。
- PacketFramer：magic、len、msgId、flags、seq、payload。

执行内容：

- 新增 CMake option：`MINI_ENABLE_FUZZ=ON`。
- 使用 LLVM `LLVMFuzzerTestOneInput` 入口。
- fuzz target 默认不进入普通 CI build。
- 可增加手动 GitHub Actions workflow 或 nightly workflow。

解决痛点：

- 协议解析是网络库生产风险高发区，尤其是半包、粘包、畸形长度、状态机错误、异常 close path。

Exit Criteria：

- 本地可运行：

```bash
CC=clang CXX=clang++ cmake -S . -B build-fuzz -DMINI_ENABLE_FUZZ=ON
cmake --build build-fuzz -j$(nproc)
./build-fuzz/tests/fuzz/fuzz_http_context -runs=1000
```

- 每个 fuzz target 至少有一个最小 corpus 样例。

---

### P0-6：Install Verify 加强

目标：

- 将 install 消费验证从基础 net 头文件扩展到主要 public API。

执行内容：

- CI consumer 工程 include：
  - `mini/net/EventLoop.h`
  - `mini/net/TcpServer.h`
  - `mini/net/TcpClient.h`
  - `mini/coroutine/Task.h`
  - `mini/coroutine/Timeout.h`
  - `mini/http/HttpServer.h`
  - `mini/http/HttpClient.h`
  - `mini/ws/WebSocketServer.h`
  - `mini/rpc/RpcServer.h`
  - `mini/rpc/RpcClient.h`
  - `mini/game/GameServerPipeline.h`
- 验证 link 成功。

解决痛点：

- 防止 public header 安装遗漏、依赖未传播、外部项目无法消费。

Exit Criteria：

- install job 中的 consumer 工程能完整 configure/build。
- 失败时可以明确定位到 public header 或 target export 问题。

---

### P0-7：PR 模板升级

目标：

- 将 Intent 驱动工程护栏写进 PR 模板，避免核心模块改动只描述功能，不描述验证证据。

执行内容：

- 更新 `.github/PULL_REQUEST_TEMPLATE.md`。
- 增加字段：
  - Intent reference
  - Module state impact
  - Thread owner
  - Ownership / release path
  - Reentrant callbacks
  - Cross-thread operations
  - Test evidence
  - Sanitizer evidence
  - Fuzz / benchmark evidence if protocol or performance related

解决痛点：

- 防止 AI 或开发者绕过 core module change gate。

Exit Criteria：

- 每个核心模块 PR 都能通过模板回答五个 change gate 问题。
- PR 中必须写出具体测试文件，而不是只写“covered by tests”。

---

## 5. P1：README 与文档定位优化

### P1-1：README 顶部定位重构

建议将 README 顶部定位改为：

> mini-trantor 是一个面向游戏服务端实验的现代 C++23 异步网络框架，用于验证 Reactor、Coroutine、RPC、WebSocket、UDP/KCP、Session Pipeline 与 Intent 驱动开发流程的结合。

同时明确：

- 它不是 trantor/asio 的生产替代品。
- 它是学习、实验、作品集、AI 工程流验证平台。
- 当前重点是游戏服务端网络底座，而不是无限扩展通用框架。

解决痛点：

- 回应 `assessment.md` 中的“定位漂移”与“范围膨胀”风险。

Exit Criteria：

- README 前 30 行能说明：
  - 这个项目是什么。
  - 适合谁。
  - 当前成熟度如何。
  - 不承诺什么。

---

### P1-2：README 结构重排

建议结构：

```text
README.md
  1. What is mini-trantor
  2. Current Status
  3. Module Maturity Matrix
  4. Quick Start
  5. Architecture Map
  6. Engineering Guardrails
  7. Intent Driven Workflow
  8. Roadmap
  9. Build and Install
```

执行内容：

- 把长版本流水账压缩成阶段摘要。
- 把详细路线图迁移到 `docs/roadmap.md` 或专门 roadmap 文档。
- README 保留最重要状态与入口链接。

解决痛点：

- 外部读者不应在 README 中被大量版本记录淹没。

Exit Criteria：

- README 可在 3 分钟内读完核心定位。
- 深入内容通过 docs 链接承接。

---

### P1-3：模块状态清单

建议新增 README 表格或 `docs/production-readiness.md`：

| 模块 | 状态 | 理由 | 升级到下一状态的条件 |
| --- | --- | --- | --- |
| EventLoop / Channel / Poller / TimerQueue | Stable | Reactor 主链路与 contract 覆盖较完整 | 持续通过 ASan/UBSan/TSan |
| TcpServer / TcpConnection / Buffer | Stable | 主路径、生命周期、关闭路径已有覆盖 | 增加 soak 与更多关闭竞态测试 |
| EventLoopThread / EventLoopThreadPool | Stable | one-loop-per-thread 与 stop 语义已测试 | TSan 长期无告警 |
| TcpClient / Connector / DNS | Beta | 主链路可用，失败/取消/重连竞态仍需加强 | DNS fail/cache/cancel race 覆盖 |
| Coroutine Task / Sleep / Timeout / WhenAny | Beta | 已有 contract，仍需 race 长跑 | double-resume/cancel race 进入 TSan/soak |
| TLS | Beta | 已跑通 echo/handshake | TLS error path、证书失败路径补齐 |
| HTTP / WebSocket / RPC | Beta | 协议层可用 | parser/codec fuzz 稳定运行 |
| UDP / KCP | Experimental | 有 loopback/reliable-flow 基线 | 拥塞/重传策略与真实游戏路径验证 |
| GameServerPipeline / Session / LogicLoop | Experimental | vertical slice 已接通 | 示例 main、压测、AOI、重连窗口长跑 |
| CodecAdapter / PacketFramer | Beta | 有 unit/contract/integration | fuzz 与畸形输入覆盖 |

解决痛点：

- 避免“模块很多 = 全部生产可用”的误读。

Exit Criteria：

- README 或 docs 中存在模块状态矩阵。
- 每个 Experimental 模块都写清楚未验证边界。

---

### P1-4：工程护栏文档

建议新增或更新：

```text
docs/production-readiness.md
docs/engineering-guardrails.md
```

内容包括：

- CI job 列表。
- sanitizer 本地运行命令。
- TSan 运行范围。
- fuzz target 列表。
- benchmark 运行方式。
- install verify 范围。
- 当前 blind spots。

解决痛点：

- 将“工程成熟度”从口头描述变成可审计文档。

Exit Criteria：

- README 能链接到工程护栏文档。
- 每个护栏都有本地命令和 CI 状态说明。

---

## 6. P2：性能基线与长期可靠性

### P2-1：Benchmark 体系正规化

目标：

- 将现有广播延迟测试从单个 integration test 发展为可追踪性能基线。

执行内容：

- 增加 `benchmark` label。
- 输出关键指标：
  - client count
  - broadcast count
  - payload size
  - route latency
  - queue latency
  - fanout latency
  - total elapsed
- 固定阈值与测试环境说明。

解决痛点：

- 当前 benchmark 缺少趋势记录，无法判断性能是否退化。

Exit Criteria：

- 可运行：

```bash
ctest --test-dir build --output-on-failure -L benchmark
```

- benchmark 输出可以复制进文档或 CI artifact。

---

### P2-2：广播与 Game Pipeline 基线扩展

目标：

- 覆盖游戏服务器方向最关键的数据路径。

建议场景：

- 4 clients / 24 broadcasts：轻量 CI smoke。
- 32 clients / 1KB payload / room broadcast。
- 128 clients / small payload / group broadcast。
- reconnect window + broadcast。
- AOI bucket fanout。
- `TCP framed packet -> codec decode -> auth/session -> LogicLoop command -> response/broadcast -> owner loop send` vertical slice latency。

解决痛点：

- 回应 assessment 中对长连接、广播、会话、背压、游戏服务端方向是否真的可靠的担忧。

Exit Criteria：

- 至少一个轻量 benchmark 进入默认 CI。
- 重量级 benchmark 可手动运行或 nightly 运行。

---

### P2-3：Soak Test 入口

目标：

- 增加长时间运行测试入口，用于发现短测试不容易暴露的泄漏、竞态和队列堆积。

执行内容：

- 新增 CMake option：`MINI_ENABLE_SOAK=ON`。
- soak test 默认不进入普通 CI。
- 建议覆盖：
  - 反复 connect/disconnect。
  - graceful shutdown while clients active。
  - reconnect window。
  - coroutine timeout/cancel race。
  - broadcast under churn。

解决痛点：

- 网络库生产风险通常出现在长时间运行与错误路径组合中。

Exit Criteria：

- 本地可运行：

```bash
cmake -S . -B build-soak -DMINI_ENABLE_SOAK=ON
cmake --build build-soak -j$(nproc)
ctest --test-dir build-soak --output-on-failure -L soak
```

---

### P2-4：失败路径压力补齐

目标：

- 为生产风险最高的失败路径建立 contract / integration 证据。

建议补齐：

- DNS resolve failed。
- DNS cache expired。
- DNS cancel race。
- TLS handshake failed。
- TLS peer close during handshake。
- RPC timeout with pending calls。
- WebSocket malformed frame close。
- PacketFramer oversized length。
- TcpConnection close while awaiter pending。
- EventLoopThreadPool stop while queued functors pending。

解决痛点：

- `assessment.md` 中“生产级能力还缺关键验证”的核心并不是 happy path，而是错误路径。

Exit Criteria：

- 每个失败路径至少对应一个测试文件。
- 文档中记录失败语义：返回错误、关闭连接、取消 awaiter、是否允许重入。

---

### P2-5：性能与可靠性报告

目标：

- 让项目具备可展示、可回归、可对比的工程证据。

建议新增：

```text
docs/benchmarks.md
docs/production-readiness.md
```

内容：

- 当前机器环境。
- 编译器版本。
- benchmark 参数。
- 关键指标。
- 当前 blind spots。
- 下一次基线更新日期。

解决痛点：

- 开源传播和面试展示不只说“我实现了很多模块”，而是能说“这些模块如何被验证”。

Exit Criteria：

- README 链接性能与生产就绪度文档。
- 每次核心性能路径变更都更新或确认 benchmark 未退化。

---

## 7. 推荐执行顺序

第一批：

1. P0-2：统一 ASan/UBSan CMake option。
2. P0-3：增加 TSan 独立入口。
3. P0-4：补充风险维度测试标签。
4. P0-7：升级 PR 模板。

第二批：

1. P0-5：落地 fuzz 目录和四个协议 fuzz target。
2. P0-6：加强 install verify。
3. P1-1/P1-2：README 顶部定位与结构重构。
4. P1-3：模块状态矩阵。

第三批：

1. P2-1：benchmark label 与指标输出。
2. P2-2：广播与 Game Pipeline benchmark 扩展。
3. P2-3：soak test 入口。
4. P2-4：失败路径压力补齐。
5. P2-5：性能与可靠性报告。

---

## 8. 当前阶段 Definition of Done

当以下条件全部满足时，可以认为本轮“工程护栏与定位收敛”阶段完成：

- CI 至少覆盖 Debug ASan/UBSan、Release full test、install verify。
- TSan 有独立本地入口，最好有 CI job。
- `tests/fuzz/` 存在，并能构建 HTTP / WS / RPC / PacketFramer fuzz target。
- `ctest` 支持按 `lifecycle`、`threading`、`protocol`、`benchmark` 等风险标签运行。
- README 清楚说明项目定位、非目标、模块成熟度。
- `docs/production-readiness.md` 或等价文档记录当前可用能力与 blind spots。
- benchmark 有可重复运行命令和明确阈值。
- 每个核心模块变更仍能回答五个 change gate 问题。

---

## 9. 核心判断

`assessment.md` 最重要的提醒不是“继续加功能”，而是 mini-trantor 已经长到需要护栏。下一阶段最值得投入的不是扩模块，而是让生命周期、线程边界、协议解析、安装消费、性能基线变成每次改动都必须经过的工程事实。

---

## 10. 执行记录：2026-06-20

本轮已开始执行 P0 v5-zeta 工程护栏任务，重点把“规划项”转成可运行的工程事实。

### 已完成

- P0-1 CI 矩阵：
  - `.github/workflows/ci.yml` 已扩展为 `gcc-debug-asan-ubsan`、`gcc-release`、`clang-debug`、`tsan-threading-contract`、`fuzz-build`、`install-verify`。
  - CI 明确覆盖 ASan/UBSan、Release 全量测试、Clang 编译差异、TSan 风险标签、fuzz smoke、install + consumer。

- P0-2 ASan/UBSan：
  - `CMakeLists.txt` 增加 `MINI_ENABLE_ASAN_UBSAN`。
  - `cmake/Sanitizers.cmake` 统一 sanitizer flag 注入，避免 CI 与本地双轨配置。
  - 本地 ASan/UBSan Debug 验证：`ctest -L "unit|contract"` 75/75 通过。

- P0-3 TSan：
  - `CMakeLists.txt` 增加 `MINI_ENABLE_TSAN`，并与 ASan/UBSan、fuzz 做互斥边界。
  - CI 使用 Clang TSan 跑 `threading|coro` 风险标签。
  - 本机 GCC TSan 可构建，但运行时统一失败于 `ThreadSanitizer: unexpected memory mapping`，记录为本地工具链限制。

- P0-4 风险标签：
  - `tests/CMakeLists.txt` 支持为测试追加风险标签。
  - 已添加 `lifecycle`、`threading`、`coro`、`protocol`、`transport`、`benchmark`。
  - 已新增 `check-lifecycle`、`check-threading`、`check-protocol`、`check-benchmark` 目标。

- P0-5 Fuzz：
  - 新增 `tests/fuzz/`。
  - 新增 fuzz target：HTTP context、WebSocket codec、RPC codec、PacketFramer。
  - 新增最小 corpus 样例。
  - 本机无 Clang，已验证 GCC 下 `MINI_ENABLE_FUZZ=ON` 会明确拒绝并提示 Clang/libFuzzer 要求；CI 使用 Clang smoke run。

- P0-6 Install Verify：
  - 修复 `mini_trantorConfig.cmake.in`，为安装包补齐 `find_dependency(OpenSSL)`。
  - 本地 Release install + 临时 consumer `find_package(mini_trantor CONFIG REQUIRED)` 验证通过。

- P0-7 PR 模板：
  - `.github/PULL_REQUEST_TEMPLATE.md` 已补充模块状态影响、风险标签测试、sanitizer/fuzz/benchmark/install evidence。

- Benchmark 入口：
  - `integration.benchmark.test_fps_like_broadcast_latency` 已标记 `benchmark`。
  - 修复 benchmark 在 Release 下因 `assert()` 被禁用而跳过等待逻辑的问题。
  - Release `ctest -L benchmark` 通过；ASan `ctest -L benchmark` 通过。

### 过程中暴露并修复的生命周期/线程边界问题

- `EventLoopThread`：
  - 新增显式 `stop()`，作为 quit + join 同步点。
  - 析构改为调用 `stop()`。
  - 合同测试覆盖 stop 等待 in-flight functor 完成、stop 后可 restart。

- `LogicLoop`：
  - `stop()` 现在等待逻辑线程退出后再清理队列，修复 ASan 报出的 `GameCommandQueue` use-after-free。
  - `onLogicTick()` 在 `running_ == false` 时提前返回，避免 stop race 后继续处理。
  - 合同测试补充 start/stop/restart 路径。

- coroutine timeout/cancellation：
  - `Timeout.h` 将 wrapper cancellation token 传递给内部 operation，修复 `whenAny + timeout` loser coroutine 生命周期泄漏。
  - 相关 coroutine 合同测试在 ASan 下通过。

- 测试自身生命周期：
  - 修复 `TcpConnection` contract 中 close callback 强引用造成的 shared_ptr cycle。
  - 修复 coroutine sleep/timeout/whenAny 测试中 loop quit 过早导致 loser cancellation functor 未 drain 的问题。
  - 修复 RPC client pool stop 测试中 server coroutine 尚未收尾就销毁 server 的时序问题。

### 本轮验证命令与结果

```bash
cmake -S . -B build-guardrails-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DMINI_ENABLE_ASAN_UBSAN=ON
cmake --build build-guardrails-asan -j$(nproc)
ctest --test-dir build-guardrails-asan --output-on-failure -L "unit|contract" --timeout 60
# 75/75 passed

ctest --test-dir build-guardrails-asan --output-on-failure -L benchmark --timeout 120
# 1/1 passed

cmake -S . -B build-guardrails-fuzz-gcc-check \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=OFF \
  -DMINI_ENABLE_FUZZ=ON
# expected failure: MINI_ENABLE_FUZZ requires Clang/clang++ for libFuzzer support.

cmake -S . -B build-guardrails-tsan-gcc \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DMINI_ENABLE_TSAN=ON
cmake --build build-guardrails-tsan-gcc -j$(nproc)
ctest --test-dir build-guardrails-tsan-gcc --output-on-failure -L "threading|coro" --timeout 120
# local runtime limitation: GCC TSan exits with ThreadSanitizer unexpected memory mapping.

cmake -S . -B build-guardrails-install \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF
cmake --build build-guardrails-install -j$(nproc)
cmake --install build-guardrails-install --prefix build-guardrails-install/_install
# temporary consumer find_package + link passed

cmake -S . -B build-guardrails-benchmark \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-guardrails-benchmark --target integration_benchmark_test_fps_like_broadcast_latency -j$(nproc)
ctest --test-dir build-guardrails-benchmark --output-on-failure -L benchmark --timeout 120
# 1/1 passed
```

### 尚未完成

- P1 工程护栏文档与 production-readiness 独立文档仍未拆出。
- P2 长期 soak、长期 fuzz corpus、趋势化 benchmark 报告仍未执行。
- Clang fuzz smoke 与 Clang TSan 需要在 CI 或安装 Clang 的本地环境中完成真实运行。

---

## 11. 执行记录：2026-06-20 P1 README 定位重构

本轮继续执行 P1，目标是收敛 `assessment.md` 中指出的“定位漂移”和“范围膨胀”风险。

### 已完成

- P1-1 README 顶部定位重构：
  - README 前 30 行改为直接说明 mini-trantor 是“面向游戏服务端实验的现代 C++23 异步网络框架”。
  - 明确它适合学习 Reactor/coroutine、游戏服务端底座实验和 AI Intent 驱动工程流验证。
  - 明确它不是 trantor/muduo/asio 的生产替代品，也不是隐藏线程所有权的全局 runtime。

- P1-2 README 结构重排：
  - 将原来的长版本流水账压缩为 `Current Status`。
  - README 主结构调整为：
    - What It Is
    - Current Status
    - Module Maturity Matrix
    - Architecture Map
    - Quick Start
    - Engineering Guardrails
    - Intent Driven Workflow
    - Directory Map
    - Roadmap
  - 详细路线图交给 `plan_and_execute.md`、`docs/roadmap.md`、`docs/roadmap_game_server_network_base_execution_plan.md` 承接。

- P1-3 模块状态清单：
  - README 新增 `Stable` / `Beta` / `Experimental` 状态定义。
  - 新增模块成熟度矩阵，覆盖 Reactor Core、TCP、线程模型、TcpClient/DNS、coroutine、TLS/IPv6/shutdown、HTTP/WS/RPC、PacketFramer/CodecAdapter、Broadcast、UDP/KCP、GameServerPipeline 等模块。
  - 每个模块都写明当前依据和升级/保持条件，避免“模块很多 = 全部生产可用”的误读。

### 本轮验证

```bash
git diff --check
```

后续仍需在 P1/P2 中补：

- `docs/production-readiness.md`
- `docs/engineering-guardrails.md`
- benchmark 趋势化报告
- 长期 fuzz corpus 与 soak test 入口
