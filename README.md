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

### v4（进行中）
- ✅ `v4-alpha`：HTTP/1.1 协议层完成 — HttpRequest（请求值对象）/ HttpResponse（响应构建器 + 序列化）/ HttpContext（per-connection 增量解析状态机）/ HttpServer（TcpServer 协议适配器 + HttpCallback）/ keep-alive + Connection: close + 400 Bad Request
- ✅ `v4-beta`：WebSocket 支持完成 — WebSocketCodec（RFC 6455 帧编解码）/ WebSocketHandshake（HTTP Upgrade 验证 + Sec-WebSocket-Accept 计算）/ WebSocketConnection（per-connection 状态机 + auto ping/pong + close 握手）/ WebSocketServer（TcpServer 包装 + HTTP→WS 升级）
- ✅ `v4-gamma`：RPC 支持完成 — RpcCodec（长度前缀二进制帧编解码）/ RpcChannel（per-connection 请求-响应关联 + 超时管理）/ RpcServer（TcpServer 协议适配器 + method 注册分发）/ RpcClient（TcpClient 包装 + callback 和 coroutine 双模式调用）
- ✅ `v4-delta`：协程版 RPC 完成 — RpcServer `registerCoroMethod()`（协程返回值即响应，异常即错误）/ RpcClient `coroCall()`（返回 payload 直接，错误抛 `RpcError`）/ `dispatchCoroHandler` 安全桥接（free function 保证帧生命周期）/ 支持 handler 内 `co_await` 异步操作（SleepAwaitable 等）

当前 49/49 测试全部通过（unit × 16 + contract × 18 + integration × 14 + 1 pre-existing coroutine segfault）。

## 下一阶段方向

以下为候选演进方向，具体阶段边界待 intent 文档定义：

- **WebSocket 支持**：~~HTTP Upgrade 握手 + WebSocket 帧编解码~~ ✅ 已完成
- **信号处理**：SIGINT/SIGTERM 的 EventLoop 集成，优雅关闭
- **HTTP 客户端**：基于 TcpClient 的 HTTP/1.1 请求发送与响应解析
- **RPC 生态**：protobuf codegen / 服务发现 / 连接池 / 重试策略

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
- `mini/net/`: Reactor 核心实现（EventLoop、Channel、Poller、TcpConnection、TcpServer、TcpClient、Connector、TimerQueue、TlsContext、DnsResolver 等）
- `mini/http/`: HTTP/1.1 协议层（HttpRequest、HttpResponse、HttpContext、HttpServer）
- `mini/ws/`: WebSocket 协议层（WebSocketCodec、WebSocketHandshake、WebSocketConnection、WebSocketServer）
- `mini/rpc/`: RPC 协议层（RpcCodec、RpcChannel、RpcServer、RpcClient）
- `mini/coroutine/`: 协程桥接层（`Task.h` 协程结果对象、`SleepAwaitable.h` 定时器 awaitable、`ResolveAwaitable.h` DNS 解析 awaitable）
- `mini/base/`: 基础工具（Timestamp、noncopyable）
- `tests/`: 按 `unit/`、`contract/`、`integration/` 分层的测试
- `examples/`: 示例程序（echo_server、coroutine_echo_server）
- `docs/`: 文档
- `diagrams/`: 架构图

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
#include "mini/coroutine/SleepAwaitable.h"
#include "mini/net/TlsContext.h"
#include "mini/net/DnsResolver.h"
#include "mini/coroutine/ResolveAwaitable.h"
#include "mini/http/HttpServer.h"
#include "mini/ws/WebSocketServer.h"
#include "mini/rpc/RpcServer.h"
#include "mini/rpc/RpcClient.h"
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
    [](const std::vector<mini::net::InetAddress>& addrs) {
        if (!addrs.empty()) {
            // 使用 addrs[0] 建立连接
        }
    });

// 方式三：协程 awaitable
auto addrs = co_await mini::coroutine::asyncResolve(resolver, &loop, "example.com", 8080);
if (!addrs.empty()) {
    mini::net::TcpClient client(&loop, addrs[0], "MyClient");
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
