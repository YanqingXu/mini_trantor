# mini-trantor Roadmap

## 1. 文档目标

本文档将"当前项目还缺什么"整理为一份可执行的演进路线图。

路线图的目标不是继续堆功能，而是优先补齐：

1. 底座一致性
2. 生命周期闭环
3. 线程亲和与关闭语义
4. 可接入性
5. 可观测性与工程护栏
6. 上层复用生态

---

## 2. 排序原则

优先级按以下顺序确定：

1. 先修会影响所有上层模块正确性的缺口
2. 先修生命周期、线程亲和、关闭语义，再扩协议面
3. 先把核心 contract 收敛，再追求工程舒适度
4. 每个阶段都必须直接对应模块、测试文件和退出信号

---

## 3. 当前判断

mini-trantor 目前已经具备一个"小而完整"的网络库骨架：

- Reactor core 已成型
- one-loop-per-thread 线程模型已成型
- TcpServer / TcpClient 双侧主链路已成型
- coroutine bridge、DNS、TLS、HTTP、WebSocket、RPC 已具备可运行能力
- 安装与 `find_package` 已具备
- `unit / contract / integration` 三层测试结构已建立

但作为"长期可演进的 C++ 网络库底座"，仍有几个关键缺口：

- 取消语义和错误语义仍未统一
- 进程级优雅关闭与信号集成尚未闭环
- 地址模型仍偏 IPv4-only
- 配置、可观测性、工程护栏仍偏轻
- 协议层和传输层解耦还不够彻底
- client 生态能力仍不完整
- README / docs / intent 存在阶段性漂移

---

## 4. 路线图总览

### G0：文档与 Intent 对齐

目标：
- 先收敛项目认知，不让实现、README、docs、intent 继续漂移

主要模块 / 文件：
- [README.md](/home/xyq/mini-trantor/README.md)
- [intents/architecture/system_overview.intent.md](/home/xyq/mini-trantor/intents/architecture/system_overview.intent.md)
- [intents/architecture/v3_stages.intent.md](/home/xyq/mini-trantor/intents/architecture/v3_stages.intent.md)
- [docs/00_overview/00_project_summary.md](/home/xyq/mini-trantor/docs/00_overview/00_project_summary.md)
- [docs/00_overview/01_architecture_overview.md](/home/xyq/mini-trantor/docs/00_overview/01_architecture_overview.md)

测试关联：
- 无新增 runtime 测试要求
- 但后续 PR 必须能在 change description 中准确引用现有测试文件

退出信号：
- README、docs、intent 中的版本边界一致
- 测试数量、语言标准、目录说明与仓库实际内容一致
- 不再出现"文档声称存在、仓库实际不存在"的目录或能力

---

### v5-alpha：统一取消与错误语义（已完成 ✅）

当前状态：
- ✅ 已退出（2026-04-25）。退出信号全部满足，56/56 测试通过。

目标：
- 建立全库一致的 cancellation 和 error surface

为什么优先：
- timeout、组合 awaitable、关闭路径、重试语义都依赖这一层

主要模块：
- [mini/net/TcpConnection.h](/home/xyq/mini-trantor/mini/net/TcpConnection.h)
- [mini/net/detail/ConnectionAwaiterRegistry.h](/home/xyq/mini-trantor/mini/net/detail/ConnectionAwaiterRegistry.h)
- [mini/coroutine/SleepAwaitable.h](/home/xyq/mini-trantor/mini/coroutine/SleepAwaitable.h)
- [mini/coroutine/WhenAny.h](/home/xyq/mini-trantor/mini/coroutine/WhenAny.h)
- [mini/net/NetError.h](/home/xyq/mini-trantor/mini/net/NetError.h)
- [mini/net/DnsResolver.h](/home/xyq/mini-trantor/mini/net/DnsResolver.h)
- [mini/coroutine/ResolveAwaitable.h](/home/xyq/mini-trantor/mini/coroutine/ResolveAwaitable.h)
- [mini/coroutine/Task.h](/home/xyq/mini-trantor/mini/coroutine/Task.h)

已完成项：
- [已完成] 新增 [mini/coroutine/CancellationToken.h](/home/xyq/mini-trantor/mini/coroutine/CancellationToken.h)，其中同时提供 `CancellationToken` / `CancellationSource` / `CancellationRegistration`
- [已完成] [mini/net/NetError.h](/home/xyq/mini-trantor/mini/net/NetError.h) 建立显式错误面，当前已覆盖 `PeerClosed` / `ConnectionReset` / `NotConnected` / `Cancelled` / `TimedOut` / `ResolveFailed`
- [已完成] 新增 [mini/coroutine/Timeout.h](/home/xyq/mini-trantor/mini/coroutine/Timeout.h)，将 `WhenAny(asyncOp, asyncSleep(...))` 的常见 timeout race 收敛为统一 `NetError::TimedOut`
- [已完成] [mini/coroutine/SleepAwaitable.h](/home/xyq/mini-trantor/mini/coroutine/SleepAwaitable.h) 接入 token cancel，并将取消映射为 `NetError::Cancelled`
- [已完成] [mini/net/TcpConnection.h](/home/xyq/mini-trantor/mini/net/TcpConnection.h) / [mini/net/TcpConnection.cc](/home/xyq/mini-trantor/mini/net/TcpConnection.cc) 的 `asyncReadSome` / `asyncWrite` / `waitClosed` 已接入 token，并返回 `Expected<...>`
- [已完成] [mini/net/detail/ConnectionAwaiterRegistry.h](/home/xyq/mini-trantor/mini/net/detail/ConnectionAwaiterRegistry.h) 具备 cancel waiter 路径，恢复仍通过 owner loop `queueInLoop()`
- [已完成] [mini/coroutine/WhenAny.h](/home/xyq/mini-trantor/mini/coroutine/WhenAny.h) 已向 subtask 注入 cancellation token，并在 winner 确定后 `cancelLosers()`
- [已完成] [mini/coroutine/Task.h](/home/xyq/mini-trantor/mini/coroutine/Task.h) 已支持把 cancellation token 绑定到 promise，供 awaitable 在 `await_suspend()` 时继承
- [已完成] `TcpConnection` / `WhenAny` 相关 contract 测试已恢复到当前源码可编译、可通过的状态；此前暴露出的编译问题、悬空 capture 与脆弱竞态测试已收口
- [已完成] [mini/net/DnsResolver.h](/home/xyq/mini-trantor/mini/net/DnsResolver.h) / [mini/coroutine/ResolveAwaitable.h](/home/xyq/mini-trantor/mini/coroutine/ResolveAwaitable.h) 已接入 `CancellationToken`，取消结果显式映射为 `NetError::Cancelled`

未完成项：
- [已完成] 深入说明类文档已补充 `withTimeout()` / `NetError::TimedOut` 使用示例（见 README）；旧的 "`WhenAny` 败者不取消" 表述已清理

建议新增模块（按当前实现调整）：
- [已完成] 不再需要单独新增 `mini/coroutine/CancellationSource.h`，当前已合并进 [mini/coroutine/CancellationToken.h](/home/xyq/mini-trantor/mini/coroutine/CancellationToken.h)
- [可选] 若后续需要更清晰的 public API 边界，可再拆分 `CancellationSource.h`；这不是当前退出信号所必需

优先覆盖的现有测试：
- [tests/unit/coroutine/test_sleep_awaitable.cpp](/home/xyq/mini-trantor/tests/unit/coroutine/test_sleep_awaitable.cpp)
- [tests/unit/coroutine/test_when_any.cpp](/home/xyq/mini-trantor/tests/unit/coroutine/test_when_any.cpp)
- [tests/contract/coroutine/test_combinator_contract.cpp](/home/xyq/mini-trantor/tests/contract/coroutine/test_combinator_contract.cpp)
- [tests/contract/tcp_connection/test_tcp_connection.cpp](/home/xyq/mini-trantor/tests/contract/tcp_connection/test_tcp_connection.cpp)
- [tests/contract/dns/test_dns_contract.cpp](/home/xyq/mini-trantor/tests/contract/dns/test_dns_contract.cpp)

建议新增测试：
- [已完成] [tests/unit/coroutine/test_cancellation_token.cpp](/home/xyq/mini-trantor/tests/unit/coroutine/test_cancellation_token.cpp)
- [已完成] [tests/contract/coroutine/test_cancellation_contract.cpp](/home/xyq/mini-trantor/tests/contract/coroutine/test_cancellation_contract.cpp)
- [已完成] [tests/contract/coroutine/test_timeout_contract.cpp](/home/xyq/mini-trantor/tests/contract/coroutine/test_timeout_contract.cpp)
- [已完成] [tests/integration/coroutine/test_timeout_race.cpp](/home/xyq/mini-trantor/tests/integration/coroutine/test_timeout_race.cpp)

当前测试状态：
- 已接入 CTest 的相关用例包括：
  - [tests/unit/coroutine/test_cancellation_token.cpp](/home/xyq/mini-trantor/tests/unit/coroutine/test_cancellation_token.cpp)
  - [tests/contract/coroutine/test_cancellation_contract.cpp](/home/xyq/mini-trantor/tests/contract/coroutine/test_cancellation_contract.cpp)
  - [tests/contract/coroutine/test_combinator_contract.cpp](/home/xyq/mini-trantor/tests/contract/coroutine/test_combinator_contract.cpp)
  - [tests/contract/coroutine/test_timeout_contract.cpp](/home/xyq/mini-trantor/tests/contract/coroutine/test_timeout_contract.cpp)
  - [tests/contract/dns/test_dns_contract.cpp](/home/xyq/mini-trantor/tests/contract/dns/test_dns_contract.cpp)
  - [tests/contract/tcp_connection/test_tcp_connection.cpp](/home/xyq/mini-trantor/tests/contract/tcp_connection/test_tcp_connection.cpp)
  - [tests/integration/coroutine/test_timeout_race.cpp](/home/xyq/mini-trantor/tests/integration/coroutine/test_timeout_race.cpp)
- 截至 2026-04-07 的定向验证结果：
  - `unit.coroutine.test_cancellation_token` 通过
  - `contract.coroutine.test_cancellation_contract` 通过
  - `contract.coroutine.test_combinator_contract` 通过
  - `contract.coroutine.test_timeout_contract` 通过
  - `contract.dns.test_dns_contract` 通过
  - `unit.dns.test_dns_resolver` 通过
  - `integration.dns.test_dns_client` 通过
  - `contract.tcp_connection.test_tcp_connection` 通过
  - `integration.coroutine.test_timeout_race` 通过
- 当前结论：v5-alpha 已退出。56/56 测试通过，退出信号全部满足。

继续推进清单（已完成）：
1. ~~同步修正文档漂移，至少更新 `WhenAny` 相关文档说明和 README 的阶段状态~~ ✅
2. 复核是否还需要把 `withTimeout()` 上推到更多上层模块（如未来 HTTP client / handshake timeout）— 留待后续阶段
3. ~~继续围绕关闭路径与 timeout/cancel 交错补充更高层集成 coverage~~ ✅

退出信号：
- `asyncReadSome`、`asyncWrite`、`asyncSleep`、DNS resolve、`WhenAny` 共享一致取消模型
- 调用者可区分 peer close、timeout、主动 cancel、I/O error
- close/error/cancel 路径不会 double-resume 或泄漏协程句柄

---

### v5-beta：优雅关闭与信号集成（已完成 ✅）

当前状态：
- ✅ 已退出（2026-04-25）。退出信号全部满足。

目标：
- 让"服务如何停下来"成为库级 contract，而不是应用层临时约定

主要模块：
- [mini/net/EventLoop.h](/home/xyq/mini-trantor/mini/net/EventLoop.h)
- [mini/net/TcpServer.h](/home/xyq/mini-trantor/mini/net/TcpServer.h)
- [mini/net/TcpClient.h](/home/xyq/mini-trantor/mini/net/TcpClient.h)
- [mini/net/Acceptor.h](/home/xyq/mini-trantor/mini/net/Acceptor.h)
- [mini/net/EventLoopThreadPool.h](/home/xyq/mini-trantor/mini/net/EventLoopThreadPool.h)

已新增模块：
- [mini/net/SignalWatcher.h](/home/xyq/mini-trantor/mini/net/SignalWatcher.h) — 通过 signalfd + Channel 将 SIGINT/SIGTERM 接入 EventLoop

已完成项：
- [已完成] [mini/net/Acceptor.h](/home/xyq/mini-trantor/mini/net/Acceptor.h) / [mini/net/Acceptor.cc](/home/xyq/mini-trantor/mini/net/Acceptor.cc) 新增 `stop()` 方法：停止监听 Channel 并设 `listening_ = false`
- [已完成] [mini/net/EventLoopThreadPool.h](/home/xyq/mini-trantor/mini/net/EventLoopThreadPool.h) / [mini/net/EventLoopThreadPool.cc](/home/xyq/mini-trantor/mini/net/EventLoopThreadPool.cc) 新增 `stop()` 方法：quit 所有 worker loops 并清理线程，重置 `next_` 为 0
- [已完成] [mini/net/TcpServer.h](/home/xyq/mini-trantor/mini/net/TcpServer.h) / [mini/net/TcpServer.cc](/home/xyq/mini-trantor/mini/net/TcpServer.cc) 新增 `stop()` 方法：停止 Acceptor → forceClose 所有连接 → 停止线程池；添加显式 `stopped_` 标志保证幂等性
- [已完成] [mini/net/SignalWatcher.h](/home/xyq/mini-trantor/mini/net/SignalWatcher.h) / [mini/net/SignalWatcher.cc](/home/xyq/mini-trantor/mini/net/SignalWatcher.cc) 通过 signalfd + Channel 将 SIGINT/SIGTERM 接入 EventLoop；构造时全局屏蔽 SIGPIPE；提供 `blockSignals()` / `ignoreSigpipe()` 静态方法
- [已完成] [mini/net/Socket.h](/home/xyq/mini-trantor/mini/net/Socket.h) / [mini/net/Socket.cc](/home/xyq/mini-trantor/mini/net/Socket.cc) 新增 `releaseFd()` 方法支持 Acceptor::stop() 关闭监听 socket

优先覆盖的现有测试：
- [tests/contract/event_loop/test_event_loop.cpp](/home/xyq/mini-trantor/tests/contract/event_loop/test_event_loop.cpp)
- [tests/contract/tcp_server/test_tcp_server.cpp](/home/xyq/mini-trantor/tests/contract/tcp_server/test_tcp_server.cpp)
- [tests/contract/tcp_client/test_tcp_client.cpp](/home/xyq/mini-trantor/tests/contract/tcp_client/test_tcp_client.cpp)
- [tests/integration/tcp_server/test_tcp_server_threaded.cpp](/home/xyq/mini-trantor/tests/integration/tcp_server/test_tcp_server_threaded.cpp)

已新增测试：
- [tests/unit/acceptor/test_acceptor_stop.cpp](/home/xyq/mini-trantor/tests/unit/acceptor/test_acceptor_stop.cpp) — Acceptor::stop() 单元测试
- [tests/unit/event_loop_thread_pool/test_thread_pool_stop.cpp](/home/xyq/mini-trantor/tests/unit/event_loop_thread_pool/test_thread_pool_stop.cpp) — EventLoopThreadPool::stop() 单元测试（8 个场景）
- [tests/contract/signal/test_signal_handling.cpp](/home/xyq/mini-trantor/tests/contract/signal/test_signal_handling.cpp) — SignalWatcher contract 测试
- [tests/contract/tcp_server/test_shutdown_ordering.cpp](/home/xyq/mini-trantor/tests/contract/tcp_server/test_shutdown_ordering.cpp) — shutdown ordering contract 测试（6 个场景）
- [tests/integration/tcp_server/test_graceful_shutdown.cpp](/home/xyq/mini-trantor/tests/integration/tcp_server/test_graceful_shutdown.cpp) — 优雅关闭集成测试

退出信号：
- ✅ SIGINT / SIGTERM 能安全唤醒 owner loop
- ✅ 停止 accept、停止新建连接、关闭已有连接、退出 worker loops 的顺序明确
- ✅ pending functors 与关闭路径不产生悬空 callback

退出信号验证结果：
- `unit.acceptor.test_acceptor_stop` 通过
- `unit.event_loop_thread_pool.test_thread_pool_stop` 通过（8 个场景）
- `contract.signal.test_signal_handling` 通过
- `contract.tcp_server.test_shutdown_ordering` 通过（6 个场景）
- `contract.tcp_server.test_tcp_server` 通过
- `contract.event_loop_thread_pool.test_event_loop_thread_pool` 通过
- `integration.tcp_server.test_graceful_shutdown` 通过（5 个场景）
- 既有测试（contract.event_loop、integration.tcp_server、contract.tcp_client）无回归

---

### v5-gamma：IPv6 与地址模型补全

目标：
- 把地址抽象从 IPv4-only 提升到可双栈使用

主要模块：
- [mini/net/InetAddress.h](/home/xyq/mini-trantor/mini/net/InetAddress.h)
- [mini/net/Socket.h](/home/xyq/mini-trantor/mini/net/Socket.h)
- [mini/net/SocketsOps.h](/home/xyq/mini-trantor/mini/net/SocketsOps.h)
- [mini/net/Acceptor.cc](/home/xyq/mini-trantor/mini/net/Acceptor.cc)
- [mini/net/Connector.cc](/home/xyq/mini-trantor/mini/net/Connector.cc)

优先覆盖的现有测试：
- [tests/contract/connector/test_connector.cpp](/home/xyq/mini-trantor/tests/contract/connector/test_connector.cpp)
- [tests/contract/tcp_client/test_tcp_client.cpp](/home/xyq/mini-trantor/tests/contract/tcp_client/test_tcp_client.cpp)
- [tests/contract/tcp_server/test_tcp_server.cpp](/home/xyq/mini-trantor/tests/contract/tcp_server/test_tcp_server.cpp)

建议新增测试：
- `tests/unit/net/test_inet_address_ipv6.cpp`
- `tests/contract/tcp_client/test_tcp_client_ipv6.cpp`
- `tests/integration/tcp_server/test_tcp_server_ipv6.cpp`

退出信号：
- 监听、主动连接、字符串化、DNS 结果消费均可覆盖 IPv4 / IPv6
- 不破坏既有 IPv4 接口与测试

---

### v5-delta：配置体系与可观测性

目标：
- 把硬编码参数和"只能靠读代码排查问题"的状态，升级为可配置、可观测的库能力

主要模块：
- [mini/net/Connector.h](/home/xyq/mini-trantor/mini/net/Connector.h)
- [mini/net/DnsResolver.h](/home/xyq/mini-trantor/mini/net/DnsResolver.h)
- [mini/net/TcpServer.h](/home/xyq/mini-trantor/mini/net/TcpServer.h)
- [mini/base/Logger.h](/home/xyq/mini-trantor/mini/base/Logger.h)
- [mini/base/Logger.cc](/home/xyq/mini-trantor/mini/base/Logger.cc)

建议新增模块：
- `mini/net/NetOptions.h`
- `mini/net/ServerOptions.h`
- `mini/base/MetricsHook.h`

优先覆盖的现有测试：
- [tests/unit/connector/test_connector.cpp](/home/xyq/mini-trantor/tests/unit/connector/test_connector.cpp)
- [tests/unit/dns/test_dns_resolver.cpp](/home/xyq/mini-trantor/tests/unit/dns/test_dns_resolver.cpp)
- [tests/integration/tcp_server/test_tcp_server_backpressure_policy.cpp](/home/xyq/mini-trantor/tests/integration/tcp_server/test_tcp_server_backpressure_policy.cpp)

建议新增测试：
- `tests/unit/base/test_logger.cpp`
- `tests/contract/net/test_options_contract.cpp`
- `tests/contract/net/test_metrics_hook_contract.cpp`

退出信号：
- 重连、DNS、背压、poll timeout 等关键参数可显式配置
- 连接建立、关闭、超时、重试、握手失败等关键事件有 hook
- 观测能力不破坏 owner-thread 纪律

---

### v5-epsilon：协议层与传输层进一步解耦

目标：
- 给 HTTP client、更多协议适配和未来替代传输打抽象基础

主要模块：
- [mini/net/detail/ConnectionTransport.h](/home/xyq/mini-trantor/mini/net/detail/ConnectionTransport.h)
- [mini/net/TcpConnection.h](/home/xyq/mini-trantor/mini/net/TcpConnection.h)
- [mini/http/HttpServer.h](/home/xyq/mini-trantor/mini/http/HttpServer.h)
- [mini/ws/WebSocketServer.h](/home/xyq/mini-trantor/mini/ws/WebSocketServer.h)
- [mini/rpc/RpcChannel.h](/home/xyq/mini-trantor/mini/rpc/RpcChannel.h)

建议新增模块：
- `mini/net/ProtocolCodec.h`
- 或 `mini/net/StreamAdapter.h`

优先覆盖的现有测试：
- [tests/contract/http/test_http_server.cpp](/home/xyq/mini-trantor/tests/contract/http/test_http_server.cpp)
- [tests/contract/ws/test_ws_server.cpp](/home/xyq/mini-trantor/tests/contract/ws/test_ws_server.cpp)
- [tests/contract/rpc/test_rpc_server.cpp](/home/xyq/mini-trantor/tests/contract/rpc/test_rpc_server.cpp)
- [tests/integration/rpc/test_rpc.cpp](/home/xyq/mini-trantor/tests/integration/rpc/test_rpc.cpp)

建议新增测试：
- `tests/unit/net/test_protocol_codec_adapter.cpp`
- `tests/contract/http/test_http_transport_contract.cpp`

退出信号：
- 协议层不再依赖 `TcpConnection` 的过多内部细节
- 传输、缓冲、编解码边界更清晰
- 不破坏现有 TCP/TLS 主路径

---

### v5-zeta：工程护栏补齐

目标：
- 让项目从"能开发"进化到"敢长期维护"

主要文件：
- [CMakeLists.txt](/home/xyq/mini-trantor/CMakeLists.txt)
- [tests/CMakeLists.txt](/home/xyq/mini-trantor/tests/CMakeLists.txt)
- [.github/PULL_REQUEST_TEMPLATE.md](/home/xyq/mini-trantor/.github/PULL_REQUEST_TEMPLATE.md)

建议新增文件：
- `.github/workflows/ci.yml`
- `cmake/Sanitizers.cmake`
- `tests/fuzz/http/fuzz_http_context.cpp`
- `tests/fuzz/ws/fuzz_ws_codec.cpp`
- `benchmarks/`

建议先覆盖：
- 全部现有 `unit / contract / integration` 测试进入 CI
- 关键并发与生命周期测试进入 ASan / UBSan

退出信号：
- 每次提交自动验证 build、ctest、install、sanitizer
- HTTP / WS parser 具备基础 fuzz 入口
- 核心路径具备基准测试基线

---

### v6-alpha：客户端生态与上层复用能力

目标：
- 在底座语义稳定后，再补真正能被上层服务大量复用的 client 生态能力

主要模块：
- [mini/net/TcpClient.h](/home/xyq/mini-trantor/mini/net/TcpClient.h)
- [mini/http/HttpContext.h](/home/xyq/mini-trantor/mini/http/HttpContext.h)
- [mini/rpc/RpcClient.h](/home/xyq/mini-trantor/mini/rpc/RpcClient.h)

建议新增模块：
- `mini/http/HttpClient.h`
- `mini/rpc/RpcConnectionPool.h`
- `mini/rpc/ServiceDiscovery.h`

建议新增测试：
- `tests/contract/http/test_http_client.cpp`
- `tests/integration/http/test_http_client.cpp`
- `tests/contract/rpc/test_rpc_client_pool.cpp`

退出信号：
- HTTP client 主链路可用
- RPC 侧至少具备连接池或服务发现中的一条稳定主线
- client 生态不绕开现有 Reactor / EventLoop 纪律

---

## 5. 建议执行顺序

建议按下面顺序推进：

1. `G0 + v5-alpha`
2. `v5-beta`
3. `v5-gamma`
4. `v5-delta`
5. `v5-zeta`
6. `v5-epsilon`
7. `v6-alpha`

说明：
- `G0` 与 `v5-alpha` 可以并行，因为它们分别修"认知一致性"和"运行时一致性"
- `v5-beta` 应早于更高层协议扩展，否则关闭语义会在更多模块里复制扩散
- `v5-gamma` 应早于 HTTP client / 更强 client 生态，否则地址模型会在新增 API 中固化
- `v5-delta` 与 `v5-zeta` 可以部分并行，但前者更偏库能力，后者更偏工程护栏
- `v5-epsilon` 放在 client 生态前，避免上层能力继续绑定现有传输细节

---

## 6. 每阶段通用变更要求

每个阶段的实现和 review 都应回答以下问题：

1. 这个变化增强的是哪一层 contract？
2. 哪个 loop / thread 拥有新增状态？
3. 谁拥有它，谁释放它？
4. 哪些回调可能重入？
5. 哪些操作允许跨线程，如何 marshal？
6. 哪个测试文件直接验证新能力？
7. 哪份 intent、文档、图需要同步更新？

---

## 7. 与现有测试结构的映射

本路线图默认继续沿用现有测试规则：

- `tests/unit/`：验证局部状态机、小不变量、边界条件
- `tests/contract/`：验证 public API、线程亲和、生命周期、回调顺序
- `tests/integration/`：验证端到端主链路

新增能力时，应优先复用现有模块目录：

- coroutine 相关变更优先落到 `tests/unit/coroutine/`、`tests/contract/coroutine/`
- TcpConnection / transport 相关变更优先落到 `tests/unit/net/`、`tests/contract/tcp_connection/`
- server / client 生命周期变更优先落到 `tests/contract/tcp_server/`、`tests/contract/tcp_client/`
- 协议层变更优先落到 `tests/contract/http/`、`tests/contract/ws/`、`tests/contract/rpc/`

---

## 8. 里程碑判断

如果只允许做三件事，优先级建议如下：

1. `v5-alpha`：统一取消与错误语义
2. `v5-beta`：优雅关闭与信号集成
3. `v5-gamma`：IPv6 与地址模型补全

这三项完成后，mini-trantor 作为"通用 C++ 网络库底座"的可信度会明显提升。
