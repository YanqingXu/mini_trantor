# 项目摘要

## 一句话定位

mini-trantor 是一个参考 trantor 设计思想、以学习和演进为目标的 **C++ Reactor 模式游戏服务器网络底座**。

## 核心事实

1. **技术栈**：C++23，Linux epoll，OpenSSL，CMake 3.16+
2. **核心抽象**：one-loop-per-thread Reactor 模型 —— 每个线程拥有一个 EventLoop，所有 I/O 和回调都在该线程内闭环执行
3. **设计哲学**：Intent 驱动开发 —— intent 先行、code 作为派生产物、test 作为契约验证
4. **游戏网络方向**：TCP/UDP/KCP preview、SessionManager、GameServerPipeline、LogicLoop、Broadcast、backpressure/security skeleton
5. **协议支持**：TCP（服务端 + 客户端）、TLS/SSL、HTTP/1.1、WebSocket（RFC 6455）、RPC、UDP datagram、KCP-style reliable UDP preview
6. **协程桥接**：C++20/23 coroutine 与 Reactor 调度语义融合 —— `Task<T>`、`CancellationToken`、`SleepAwaitable`、`Timeout`、`WhenAll`/`WhenAny`
7. **DNS 解析**：异步线程池 + TTL 缓存，不阻塞任何 EventLoop 线程
8. **测试体系**：三层分离 —— unit（纯逻辑）、contract（API/生命周期/线程亲和）、integration（端到端）
9. **收口边界**：当前定位为 game-network foundation + explicit transport preview；高级 KCP/PMTU/FEC 类能力必须标记为 preview/experimental，不代表完整商业网关或生产级协议栈

## 最重要的几个模块

| 模块 | 职责 | 关键类 |
|------|------|--------|
| Reactor Core | 事件循环与 I/O 多路复用 | `EventLoop`, `Channel`, `Poller`, `EPollPoller` |
| Net | TCP 连接管理 | `TcpServer`, `TcpConnection`, `Acceptor`, `TcpClient`, `Connector` |
| Timer | 定时任务 | `TimerQueue`, `TimerId` |
| Thread | 线程模型 | `EventLoopThread`, `EventLoopThreadPool` |
| Coroutine | 协程桥接 | `Task<T>`, `CancellationToken`, `SleepAwaitable`, `Timeout`, `WhenAll`, `WhenAny` |
| HTTP | HTTP/1.1 协议层 | `HttpServer`, `HttpContext`, `HttpRequest`, `HttpResponse` |
| WebSocket | WebSocket 协议层 | `WebSocketServer`, `WebSocketCodec`, `WebSocketConnection` |
| Advanced | TLS + DNS | `TlsContext`, `DnsResolver` |
| Game Foundation | 游戏网络接入与会话/逻辑桥 | `PlayerSession`, `SessionManager`, `GameServerPipeline`, `LogicLoop` |
| Broadcast | 游戏广播路由和批量发送 | `BroadcastRouter`, `BroadcastDispatcher` |
| Transport Preview | KCP/PMTU 预览传输层 | `KcpTransport`, `KcpSession`, `PathMtuSignalAdapter`, `PathMtuCache` |
| Metrics | 轻量观测钩子与文本 snapshot | `MetricsHook`, `MetricsExporter` |

## 阅读建议

如果你第一次接触这个项目：

1. 先看 `AGENTS.md` 了解项目的开发方法论
2. 再看 `docs/game_server_network_base_scope_boundary.md` 明确当前收口边界
3. 再看本文档 → `01_architecture_overview.md` → `02_module_map.md` 建立全局认知
4. 然后按 `EventLoop` → `Channel` → `Buffer` → `TcpConnection` → `TcpServer` 顺序深入核心模块
5. 最后看 `01_callflow/` 目录了解完整的调用链路

## 与同类框架对比

| 特性 | mini-trantor | muduo | libevent |
|------|-------------|-------|----------|
| 语言标准 | C++20 | C++11 | C |
| 协程支持 | 原生 co_await | 无 | 无 |
| 线程模型 | one-loop-per-thread | one-loop-per-thread | 可配置 |
| 协议层 | HTTP + WebSocket | HTTP | HTTP |
| TLS | OpenSSL 集成 | 无 | OpenSSL 集成 |
| DNS | 异步线程池 | 无 | 内置异步 |
| Intent 驱动 | ✅ | ❌ | ❌ |
