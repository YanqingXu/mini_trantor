# mini-trantor

mini-trantor 是一个参考 trantor 思想、以学习和演进为目标的 C++ Reactor 网络库。

它不是单纯“写代码”的项目，而是一个 **Intent 驱动架构** 的实验与工程实践：

- Intent 先行
- Code 作为实现产物
- Tests 作为契约验证
- Diagrams 作为架构解释
- AI 作为受约束的实现协作者

## 当前状态

### v1（已完成）
- ✅ `v1-alpha`：同步 Reactor 主链路稳定 — Channel / Poller / EventLoop / Buffer / Acceptor / TcpConnection / TcpServer
- ✅ `v1-beta`：线程模型稳定 — EventLoopThread / EventLoopThreadPool / 跨线程调度
- ✅ `v1-coro-preview`：协程桥接跑通 — `Task<T>` / TcpConnection awaitables / coroutine echo server

### v2（已完成）
- ✅ `v2-alpha`：TcpClient 主链路稳定 — Connector（主动连接适配器）/ TcpClient / 可配置重连退避
- ✅ `v2-beta`：async timer API 稳定 — `SleepAwaitable` 协程定时器桥接 / coroutine idle timeout 集成

### v3（已完成）
- ✅ `v3-alpha`：结构化并发原语完成 — `WhenAll`（多任务并发等待全部完成）/ `WhenAny`（多任务竞争等待首个完成 + 剩余取消）/ 与 `Task<T>`·`SleepAwaitable` 组合
- ✅ `v3-beta`：DNS Resolver 完成 — DnsResolver（线程池异步解析 + TTL 缓存）/ TcpClient hostname-based connect / ResolveAwaitable 协程桥接
- ✅ `v3-gamma`：TLS/SSL 集成完成 — TlsContext（RAII SSL_CTX 封装）/ TcpConnection 非阻塞 TLS 状态机 / TcpServer·TcpClient 的 `enableSsl()` API / OpenSSL 后端

### v4（已完成）
- ✅ `v4-alpha`：HTTP/1.1 协议层完成 — HttpRequest（请求值对象）/ HttpResponse（响应构建器 + 序列化）/ HttpContext（per-connection 增量解析状态机）/ HttpServer（TcpServer 协议适配器 + HttpCallback）/ keep-alive + Connection: close + 400 Bad Request
- ✅ `v4-beta`：WebSocket 支持完成 — WebSocketCodec（RFC 6455 帧编解码）/ WebSocketHandshake（HTTP Upgrade 验证 + Sec-WebSocket-Accept 计算）/ WebSocketConnection（per-connection 状态机 + auto ping/pong + close 握手）/ WebSocketServer（TcpServer 包装 + HTTP→WS 升级）
- ✅ `v4-gamma`：RPC 支持完成 — RpcCodec（长度前缀二进制帧编解码）/ RpcChannel（per-connection 请求-响应关联 + 超时管理）/ RpcServer（TcpServer 协议适配器 + method 注册分发）/ RpcClient（TcpClient 包装 + callback 和 coroutine 双模式调用）
- ✅ `v4-delta`：协程版 RPC 完成 — RpcServer `registerCoroMethod()`（协程返回值即响应，异常即错误）/ RpcClient `coroCall()`（返回 payload 直接，错误抛 `RpcError`）/ `dispatchCoroHandler` 安全桥接（free function 保证帧生命周期）/ 支持 handler 内 `co_await` 异步操作（SleepAwaitable 等）

### v5-alpha（已完成）
- ✅ 统一取消原语（`CancellationToken` / `CancellationSource`）、`WhenAny` loser cancel、DNS cancel、显式 `NetError`（`PeerClosed` / `ConnectionReset` / `NotConnected` / `Cancelled` / `TimedOut` / `ResolveFailed`）、`withTimeout()` 已进入主线
- ✅ 退出信号全部满足：
  - `asyncReadSome`、`asyncWrite`、`asyncSleep`、DNS resolve、`WhenAny` 共享一致取消模型
  - 调用者可区分 peer close、timeout、主动 cancel、I/O error
  - close/error/cancel 路径不会 double-resume 或泄漏协程句柄

### v5-beta / v5-gamma / v5-delta（已完成）
- ✅ `v5-beta`：优雅关闭与信号集成 — `SignalWatcher` / SIGPIPE 屏蔽 / `Acceptor::stop()` / `EventLoopThreadPool::stop()` / `TcpServer::stop(Duration)`
- ✅ `v5-gamma`：IPv6 与地址模型补全 — `InetAddress` 基于 `sockaddr_storage`，`Connector` / `DnsResolver` 支持 family-aware 双栈路径
- ✅ `v5-delta`：配置体系与可观测性 — `ConnectorOptions` / `DnsResolverOptions` / `TcpServerOptions` / `TcpClientOptions`，以及连接、背压、TLS、广播、EventLoop、Session、LogicLoop 指标 hook

### v5-epsilon（已完成）
- ✅ 协议层与传输层进一步解耦：HTTP / WebSocket / RPC 不再直接依赖 `TcpConnection` 宽接口
  - 新增 `IProtocolConnection` 窄接口（send/shutdown/forceClose/connected/getLoop/name/setProtocolContext/getProtocolContext）
  - 新增 `ProtocolConnectionAdapter` 适配器，绑定到 `TcpConnection` 生命周期，协议状态通过 `setProtocolContext()` 存储
  - HTTP / WS / RPC 协议层的 send/shutdown/forceClose/context 操作均改为通过 adapter 完成
  - 新增 7 个 adapter 单元测试 + 4 个 HTTP transport contract 测试，所有 WS / RPC contract + integration 测试无回归

### v5-zeta（已完成）
- ✅ 工程护栏补齐：CI、sanitizer、install 校验
  - 新增 `.github/workflows/ci.yml`，Debug（ASan + UBSan）/ Release 双矩阵构建 + 测试
  - 新增 `cmake/Sanitizers.cmake` 模块，Debug 模式自动启用 AddressSanitizer / UndefinedBehaviorSanitizer
  - 新增 `intents/architecture/v5_zeta_engineering_guardrails.intent.md` 设计意图文档
  - 修复 3 个测试：acceptor stop 超时、HTTP server 线程关联断言、awaiter registry segfault
  - CI 中包含 install + find_package 消费验证
  - Fuzz 入口仍未纳入；轻量 benchmark 已有 `tests/integration/benchmark/test_fps_like_broadcast_latency.cpp`

### v6-alpha：客户端生态（主链路完成，示例收尾待补）
- ✅ **Phase A — 基础设施**：`HttpResponseContext` + `RpcPoolOptions`
- ✅ **Phase B — HTTP Client**：`HttpClient` 已实现 keep-alive 复用、连接重建、协程 API、超时，并有 contract + integration 回归
- ✅ **Phase C — RPC 连接池**：`RpcConnectionPool` 已实现 round-robin、生命周期、重建续发、停止 fail-all pending，并有 contract 覆盖
- ⚠️ **Phase D — 收尾**：`examples/http_client.cpp`、`examples/rpc_pool_client.cpp` 与快速上手文档仍未完成

### v6-alpha：游戏服务器网络底座（进行中）
- ✅ **Task-01 统一传输抽象**：`ITransportEndpoint` / `TransportManager` 已成为 TCP / UDP / KCP 的统一注册、查询、发送、关闭入口；`SessionManager`、`LogicLoop`、`BroadcastRouter` 已支持面向 `TransportSessionId + ITransportEndpoint` 的默认路径
- ✅ **Task-02 / Task-03 传输基线**：`UdpServer` / `UdpSocket`、`UdpTransportEndpoint`、`KcpTransport` / `KcpSession` 已有 loopback / reliable-flow / transport contract 覆盖
- ✅ **Task-04 / Task-05 / Task-06 广播链路**：`BroadcastRouter` + `BroadcastDispatcher` + `PayloadPool` 已支持按 owner loop 分桶、payload 共享、端点 fanout；广播路由从 connection-name 扩展到 PlayerSession、room/group、AOI bucket
- ✅ **Task-07 / Task-08 协议与序列化**：`PacketFramer` 已统一粘包/半包帧结构；`CodecAdapter`、Protobuf-style、FlatBuffers-style adapter 已有 unit / contract / integration 覆盖
- ✅ **Task-09 / Task-10 会话与重连**：`PlayerSession` + `SessionManager` 支持 auth、online、closing、reconnect window、transport rebind 与 replay auth 集成路径
- ✅ **Task-11 逻辑线程化**：`LogicLoop` + `GameCommandQueue` 已按 fixed-step 消费命令，并能通过 endpoint 回写到 owner loop
- ✅ **Task-12 指标升级**：广播 fanout/route/queue/fanout latency、Session reconnect、LogicLoop backlog/lag、EventLoop pending functor 等 hook 已接入；广播 benchmark 已绑定明确延迟阈值
- ✅ **端到端 vertical slice**：`GameServerPipeline` 已接通 `TCP framed packet -> codec decode -> auth/session -> LogicLoop command -> response/broadcast -> owner loop send`
- ⚠️ **仍未完成的边界**：生产级 AOI 空间索引、UDP/KCP 拥塞/重传策略打磨、示例化游戏服务器 main、压力测试规模化、fuzz 与文档/diagram 收尾仍需继续推进

当前 build 树中 109/109 CTest 用例全部通过（unit × 36 + contract × 39 + integration × 34）。

## 下一阶段方向

下一阶段统一围绕“通用游戏服务器底座”推进：

- 详细规划见 [docs/roadmap_game_server_network_base_execution_plan.md](docs/roadmap_game_server_network_base_execution_plan.md)
- 阶段边界见 `intents/architecture/v6_stages.intent.md`

当前推荐的推进顺序为：

1. 固化 `GameServerPipeline` 的示例入口与架构图，避免 vertical slice 只停留在测试中
2. 扩大广播压测规模，加入 payload reuse、重连窗口、AOI bucket 的持续 benchmark
3. 将 UDP/KCP 从 loopback/reliable-flow 基线推进到可配置策略与真实游戏消息路径

## 核心理念
对于重要模块，不先写代码，先写：
1. intent
2. invariants
3. threading rules
4. ownership rules
5. contract tests
6. implementation

## 目录说明
- `intents/`: 设计意图与模块宪法（architecture / modules / usecases）
- `rules/`: 项目级约束规则（线程亲和、所有权、测试、编码、Review）
- `mini/net/`: Reactor 核心实现（EventLoop、Channel、Poller、EPollPoller、Buffer、Callbacks、TcpConnection、TcpServer、TcpClient、Connector、Acceptor、InetAddress、Socket、SocketsOps、TimerQueue、TimerId、EventLoopThread、EventLoopThreadPool、TlsContext、DnsResolver、NetError、SignalWatcher、`detail/` 内部辅助模块等）
- `mini/net/transport/`: 统一传输抽象（`ITransportEndpoint`、`TransportEndpoint`、`UdpTransportEndpoint`、`TransportManager`）
- `mini/net/udp/` / `mini/net/kcp/`: UDP 基线与 KCP preview 传输实现
- `mini/net/broadcast/` / `mini/net/buffer/`: 广播路由、ioLoop 分桶批量发送、共享 payload 与 payload pool
- `mini/net/framing/`: 通用 `PacketFramer` 帧协议（magic + len + msgId + flags + seq + payload）
- `mini/http/`: HTTP/1.1 协议层（HttpRequest、HttpResponse、HttpResponseContext、HttpClient、HttpContext、HttpServer）
- `mini/ws/`: WebSocket 协议层（WebSocketCodec、WebSocketHandshake、WebSocketConnection、WebSocketServer）
- `mini/rpc/`: RPC 协议层（RpcCodec、RpcChannel、RpcServer、RpcClient、RpcPoolOptions、RpcConnectionPool）
- `mini/codec/`: Protobuf-style / FlatBuffers-style codec adapter 抽象
- `mini/game/`: 游戏服务器底座模块（`PlayerSession`、`SessionManager`、`GameServerPipeline`、`LogicLoop`、`GameCommandQueue`）
- `mini/coroutine/`: 协程桥接层（`Task.h` 协程结果对象、`CancellationToken.h` 取消原语、`SleepAwaitable.h` 定时器 awaitable、`WhenAll.h` 多任务并发等待、`WhenAny.h` 多任务竞争等待、`Timeout.h` 统一 timeout 包装、`ResolveAwaitable.h` DNS 解析 awaitable）
- `mini/base/`: 基础工具（Timestamp、noncopyable、Logger）
- `tests/`: 按 `unit/`、`contract/`、`integration/` 分层的测试
- `examples/`: 示例程序（echo_server、coroutine_echo_server）
- `docs/`: 文档

## 核心模块改动闸门
每个核心模块 PR / 改动都必须回答这 5 个问题：
1. 这个模块归属哪个 loop / thread？
2. 谁拥有它，谁释放它？
3. 哪些回调可能重入？
4. 哪些操作允许跨线程，如何投递？
5. 对应哪个测试文件验证？

## 开发顺序
请先阅读：
- `AGENTS.md`
- `intents/architecture/*`
- `rules/*`

再开始生成或修改核心模块。

## 构建与使用

本项目现在支持标准 CMake 安装与 `find_package` 消费。

构建并安装到本地前缀：

```bash
cmake -S . -B build
cmake --build build
cmake --install build --prefix ./build/_install
```

在外部工程中使用：

```cmake
find_package(mini_trantor CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE mini_trantor::mini_trantor)
```

头文件路径保持为：

```cpp
#include "mini/net/EventLoop.h"
#include "mini/net/TcpServer.h"
#include "mini/net/TcpClient.h"
#include "mini/coroutine/Task.h"
#include "mini/coroutine/CancellationToken.h"
#include "mini/coroutine/SleepAwaitable.h"
#include "mini/coroutine/Timeout.h"
#include "mini/net/TlsContext.h"
#include "mini/net/DnsResolver.h"
#include "mini/coroutine/ResolveAwaitable.h"
#include "mini/net/NetError.h"
#include "mini/http/HttpServer.h"
#include "mini/ws/WebSocketServer.h"
#include "mini/rpc/RpcServer.h"
#include "mini/rpc/RpcClient.h"
#include "mini/net/transport/TransportManager.h"
#include "mini/game/GameServerPipeline.h"
```

### TLS/SSL 使用示例

```cpp
#include "mini/net/TcpServer.h"
#include "mini/net/TlsContext.h"

// 服务端启用 TLS
auto ctx = mini::net::TlsContext::newServerContext("server.crt", "server.key");
mini::net::TcpServer server(&loop, listenAddr, "TlsServer");
server.enableSsl(ctx);
server.start();

// 客户端启用 TLS
auto clientCtx = mini::net::TlsContext::newClientContext();
clientCtx->setCaCertPath("ca.crt");
clientCtx->setVerifyPeer(true);
mini::net::TcpClient client(&loop, serverAddr, "TlsClient");
client.enableSsl(clientCtx, "hostname");
client.connect();
```

### Cancellation / Timeout / NetError 使用示例

```cpp
#include "mini/coroutine/Task.h"
#include "mini/coroutine/Timeout.h"
#include "mini/coroutine/CancellationToken.h"
#include "mini/net/NetError.h"
#include "mini/net/TcpConnection.h"

// 方式一：withTimeout 为异步操作加上超时，返回 Expected<T> 区分错误类型
auto result = co_await mini::coroutine::withTimeout(
    &loop,
    conn->asyncReadSome(1024),
    5s);

if (!result) {
    switch (result.error()) {
    case mini::net::NetError::TimedOut:
        // 操作超时
        break;
    case mini::net::NetError::Cancelled:
        // 主动取消（通过 CancellationToken）
        break;
    case mini::net::NetError::PeerClosed:
        // 对端关闭连接
        break;
    case mini::net::NetError::ConnectionReset:
        // 连接被重置
        break;
    default:
        // 其他 I/O 错误
        break;
    }
    co_return;
}
// result.value() 包含读取到的数据

// 方式二：手动使用 CancellationToken 控制协程取消
mini::coroutine::CancellationSource source;
auto token = source.token();

// 启动一个可取消的异步任务
auto task = someAsyncWork(&loop, token);
// ... 在其他地方决定取消
source.cancel();
auto taskResult = co_await task;
if (!taskResult) {
    // taskResult.error() == NetError::Cancelled
}
```

### DNS Resolver 使用示例

```cpp
#include "mini/net/DnsResolver.h"
#include "mini/net/TcpClient.h"
#include "mini/coroutine/ResolveAwaitable.h"

// 方式一：TcpClient 直接使用 hostname 连接
mini::net::TcpClient client(&loop, "example.com", 8080, "MyClient");
client.connect();  // 内部自动 DNS 解析

// 方式二：手动异步解析
auto resolver = mini::net::DnsResolver::getShared();
resolver->resolve("example.com", 8080, &loop,
    [](mini::net::Expected<std::vector<mini::net::InetAddress>> addrs) {
        if (addrs) {
            // 使用 (*addrs)[0] 建立连接
        }
    });

// 方式三：协程 awaitable
auto addrs = co_await mini::coroutine::asyncResolve(resolver, &loop, "example.com", 8080);
if (addrs) {
    mini::net::TcpClient client(&loop, (*addrs)[0], "MyClient");
    client.connect();
}
```

### HTTP/1.1 Server 使用示例

```cpp
#include "mini/http/HttpServer.h"
#include "mini/net/EventLoop.h"

mini::net::EventLoop loop;
mini::http::HttpServer server(&loop, mini::net::InetAddress(8080, true), "HttpServer");

server.setHttpCallback([](const mini::http::HttpRequest& req, mini::http::HttpResponse* resp) {
    resp->setStatusCode(mini::http::HttpResponse::k200Ok);
    resp->setStatusMessage("OK");
    resp->setContentType("text/plain");
    resp->setBody("Hello from mini-trantor HTTP server! Path: " + req.path());
});

server.setThreadNum(4);  // 可选：多线程
server.start();
loop.loop();
```

### WebSocket Server 使用示例

```cpp
#include "mini/ws/WebSocketServer.h"
#include "mini/ws/WebSocketConnection.h"
#include "mini/net/EventLoop.h"

mini::net::EventLoop loop;
mini::ws::WebSocketServer server(&loop, mini::net::InetAddress(9090, true), "WsServer");

server.setMessageCallback([](const mini::net::TcpConnectionPtr& conn,
                             std::string msg, mini::ws::WsOpcode opcode) {
    // Echo: 原样返回收到的消息
    mini::ws::WebSocketConnection::sendText(conn, "echo: " + msg);
});

server.setConnectCallback([](const mini::net::TcpConnectionPtr& conn) {
    printf("WebSocket client connected: %s\n", conn->name().c_str());
});

server.setCloseCallback([](const mini::net::TcpConnectionPtr& conn,
                           mini::ws::WsCloseCode code, const std::string& reason) {
    printf("WebSocket client disconnected: code=%d reason=%s\n",
           static_cast<int>(code), reason.c_str());
});

server.start();
loop.loop();
```

### RPC 使用示例

```cpp
#include "mini/rpc/RpcServer.h"
#include "mini/rpc/RpcClient.h"
#include "mini/coroutine/Task.h"
#include "mini/net/EventLoop.h"

// 服务端：注册 callback 方法
mini::net::EventLoop loop;
mini::rpc::RpcServer server(&loop, mini::net::InetAddress(9090, true), "RpcServer");

server.registerMethod("Greet", [](std::string_view payload,
                                   std::function<void(std::string_view)> respond,
                                   std::function<void(std::string_view)> respondError) {
    respond(std::string("Hello, ") + std::string(payload) + "!");
});

// 服务端：注册 coroutine 方法（co_return = 响应，throw = 错误）
server.registerCoroMethod("AsyncGreet",
    [&loop](std::string payload) -> mini::coroutine::Task<std::string> {
        co_await mini::coroutine::asyncSleep(&loop, 100ms);  // 模拟异步处理
        co_return "Hello, " + payload + "!";
    });

server.setThreadNum(4);  // 可选：多线程
server.start();

// 客户端：callback 模式
mini::rpc::RpcClient client(&loop, mini::net::InetAddress(9090, true), "RpcClient");
client.connect();
client.call("Greet", "World", [](const std::string& error, const std::string& payload) {
    if (error.empty()) {
        printf("Got: %s\n", payload.c_str());  // "Hello, World!"
    }
}, 3000);  // 3秒超时

// 客户端：asyncCall 模式（RpcResult 返回值）
auto result = co_await client.asyncCall("Greet", "World", 3000);
if (result.ok()) {
    printf("Got: %s\n", result.payload.c_str());
}

// 客户端：coroCall 模式（直接返回 payload，错误抛 RpcError）
try {
    std::string payload = co_await client.coroCall("AsyncGreet", "World", 3000);
    printf("Got: %s\n", payload.c_str());
} catch (const mini::rpc::RpcError& e) {
    printf("Error: %s\n", e.what());
}

loop.loop();
```

## 附录：架构审计报告（2026-06-20）

本节按 “结构正确胜过功能堆砌” 的视角，基于当前仓库状态给出工程可持续性评估。

### 1) 技术架构高度

- Reactor 内核（`EventLoop / Poller / Channel / TimerQueue`）与线程模型（one-loop-per-thread）已经形成统一的基础语义：I/O、定时器与回调都在所属 `EventLoop` 线程执行。
- `EventLoopThread / EventLoopThreadPool` 的分发与生命周期链路稳定，配合 `TcpServer / TcpClient / Connector`，使得跨线程访问有明确的单一入口（`runInLoop / queueInLoop`）。
- 协程桥接（`Task<T>`、`CancellationToken`、`SleepAwaitable`、`WhenAny/WhenAll`）无独立调度器，恢复操作回归 `EventLoop`，没有绕开底层事件循环。
- v6-alpha 客户端生态（`HttpClient` + `RpcConnectionPool`）在协议特性之外，优先复用了现有连接与回收机制，保持了“可复用而非并行体系”原则。
- v6-alpha 游戏服务器底座已出现可运行默认路径：统一 transport、session/auth、PacketFramer、LogicLoop、room/group/AOI 广播、owner-loop send 串成了端到端 vertical slice。

### 2) 工程化与方法论（v5-zeta 价值）

- CI 已覆盖 `Debug` 与 `Release` 双矩阵；Debug 路径接入 `ASan/UBSan`，并带来生命周期与指针错误的前置暴露能力。
- 安装链路已通过 `cmake --install` + `find_package` 验证，消费方可用性从“编译”提升为“可集成”。
- `109` 个测试（unit/contract/integration）分层结构完整，且新增契约变更持续通过测试闭环验证。
- 仍需跟进的 gap：fuzz 入口尚未纳入；benchmark 已有轻量广播延迟阈值，但还不是生产级大规模压测矩阵。

### 3) 功能完备性评估（HTTP/WS/RPC/TLS/DNS + v6-alpha）

- HTTP/1.1、WebSocket、RPC、TLS、DNS、IPv6 已具备可运行主链路，能支撑真实服务中的核心交互路径。
- v6-alpha 已形成可复用客户端核心能力：HTTP 请求生命周期、连接重建、keep-alive 与超时；RPC 具备连接池、续发与 `stop` fail-all。
- 游戏服务器方向已覆盖 TCP/UDP/KCP transport baseline、会话重连、fixed-step 逻辑线程、PacketFramer、codec adapter、广播路由与指标 hook。
- 目前的边界是“游戏底座 preview”：AOI 仍是 bucket 语义而非空间索引，UDP/KCP 仍偏基线验证，压力测试规模和生产策略还需继续补齐。

### 4) 未来演进潜力与挑战

- 游戏底座的下一步应优先把测试中的 vertical slice 示例化，并为 room/group/AOI、reconnect、payload reuse 建立持续 benchmark。
- UDP/KCP 可以继续演进为真实游戏消息路径，但必须保持“独立传输实例挂到 EventLoop”，不能绕开 Reactor 线程亲和。
- 服务发现、负载均衡、HTTP/2 仍可作为后续方向，但应排在游戏底座 P0 语义和压力测试稳定之后。

### 5) 结论

当前阶段最值得肯定的是：库已从“通用网络库”进入“游戏服务器底座 preview”的状态，尤其在线程亲和、生命周期、取消/错误语义、会话重连、逻辑线程与广播指标方面形成了统一语言。下一步应继续遵循 Intent 先行与契约优先：先把 vertical slice 示例化、压测化，再推进更复杂的 AOI、UDP/KCP 策略与服务治理能力。
