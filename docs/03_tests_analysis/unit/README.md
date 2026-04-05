# 单元测试分析

## 概览

mini-trantor 共有 **15 个单元测试文件**，覆盖纯值对象和纯协程层的本地不变量验证。
单元测试不依赖 EventLoop 循环运行、网络连接或定时器触发。

## 测试清单

| # | 文件 | 测试目标 | 用例数 | 关键验证点 |
|---|------|----------|--------|------------|
| 1 | `timer_queue/test_timer_id.cpp` | TimerId | 3 | valid/invalid 判断、cancel 不崩溃 |
| 2 | `channel/test_channel.cpp` | Channel | 3 | handleEvent 回调分发、tie() 弱引用保护、expired 后不回调 |
| 3 | `coroutine/test_when_all.cpp` | WhenAll | 10 | void/value/混合类型、单任务、零任务、异常传播、多异常、move 语义 |
| 4 | `coroutine/test_sleep_awaitable.cpp` | SleepAwaitable | 4 | await_ready=false、零时长、loop 指针捕获、初始状态 |
| 5 | `coroutine/test_when_any.cpp` | WhenAny | 8 | 首个完成获胜、void 任务、异常从赢家传播、move 语义 |
| 6 | `coroutine/test_task.cpp` | Task\<T\> | 7 | lazy 语义、chained co_await、void 链、move-only 类型、异常、detach 生命周期 |
| 7 | `http/test_http_request.cpp` | HttpRequest | 6 | parseMethod、methodString、header 存取、path/query/body/version、reset |
| 8 | `http/test_http_response.cpp` | HttpResponse | 6 | 序列化格式、close/keep-alive、自定义 header、204 空 body |
| 9 | `http/test_http_context.cpp` | HttpContext | 13 | 完整/增量解析、query 提取、POST body、畸形请求、HTTP/1.0、reset 复用、header trim |
| 10 | `ws/test_ws_codec.cpp` | WsCodec | 13 | text/binary/ping/pong/close 帧编码、16 位扩展长度、解码 masked/unmasked、不完整帧、RSV 拒绝、unmask |
| 11 | `ws/test_ws_handshake.cpp` | WsHandshake | ~10 | computeAcceptKey（RFC 6455 向量）、isWebSocketUpgrade、各种拒绝条件、buildUpgradeResponse |
| 12 | `dns/test_dns_resolver.cpp` | DnsResolver | 6 | localhost 解析、无效主机名、缓存命中、clearCache、IP 字面量、shared 实例 |
| 13 | `buffer/test_buffer.cpp` | Buffer | 6 | append/retrieve、readFd 经 pipe、大数据（4KB）、writeFd 错误 errno、readFd 错误 errno |
| 14 | `tls/test_tls_context.cpp` | TlsContext | 6 | server/client 上下文创建、无效证书、key 不匹配、CA 路径、verify peer、shared 所有权 |
| 15 | `connector/test_connector.cpp` | Connector | 4 | 初始状态、setRetryDelay、setNewConnectionCallback、安全析构 |

## 测试模式分析

### 协程层测试（test_task, test_when_all, test_when_any）

**特点**: 纯同步运行，不需要 EventLoop

```cpp
// 模式: 创建 → start() → assert done() → result()
auto task = whenAll(intTask(10), intTask(20));
task.start();
assert(task.done());
auto [a, b] = std::move(task).result();
```

* 所有子协程都是立即完成的（synchronous）
* start() 启动后同步执行到 co_return
* 用于验证协程组合器的逻辑正确性

### 值对象测试（HttpRequest, HttpResponse, HttpContext, WsCodec）

**特点**: 纯状态操作，零外部依赖

```cpp
// 模式: 构造 → 操作 → assert 状态
HttpContext ctx;
Buffer buf;
buf.append("GET / HTTP/1.1\r\n...");
assert(ctx.parseRequest(&buf));
assert(ctx.request().method() == HttpMethod::kGet);
```

### 本地不变量测试（Channel, Buffer, Connector, SleepAwaitable）

**特点**: 验证对象构造/析构和本地行为

```cpp
// 模式: 构造 → 设置回调 → 模拟事件 → assert 回调触发
Channel channel(&loop, fd);
channel.setReadCallback([&](Timestamp) { readCalled = true; });
channel.setRevents(EPOLLIN);
channel.handleEvent(now());
assert(readCalled);
```

## 覆盖盲区

| 未覆盖 | 原因 |
|--------|------|
| EventLoop 单元测试 | EventLoop 是 Reactor 核心，单元级别难以隔离（在 contract 中覆盖） |
| Poller/EPollPoller 单元测试 | 需要真实 epoll，在 contract 中覆盖 |
| TcpConnection 单元测试 | 需要 socket fd 和 EventLoop，在 contract 中覆盖 |
| TcpServer 单元测试 | 需要线程池和网络，在 contract 中覆盖 |

## 测试质量评价

* **强项**: 协程组合器测试非常全面（WhenAll 10 用例、WhenAny 8 用例）
* **强项**: HTTP 解析器增量测试覆盖完善（13 用例含各种边界情况）
* **强项**: WebSocket 编解码完整覆盖正常/错误/边界场景
* **改进点**: SleepAwaitable 单元测试偏少（4 用例只验证初始状态，不验证行为）
* **改进点**: Connector 单元测试偏简单（4 用例主要验证构造/析构）
