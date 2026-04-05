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

### v3（进行中）
- ✅ `v3-gamma`：TLS/SSL 集成完成 — TlsContext（RAII SSL_CTX 封装）/ TcpConnection 非阻塞 TLS 状态机 / TcpServer·TcpClient 的 `enableSsl()` API / OpenSSL 后端
- ✅ `v3-beta`：DNS Resolver 完成 — DnsResolver（线程池异步解析 + TTL 缓存）/ TcpClient hostname-based connect / ResolveAwaitable 协程桥接

当前 37/37 测试全部通过（unit × 10 + contract × 16 + integration × 11）。

## 下一阶段方向

以下为候选演进方向，具体阶段边界待 intent 文档定义：

- **结构化并发原语**：如 `whenAll` / `whenAny`，使多个 awaitable 可以组合等待

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
