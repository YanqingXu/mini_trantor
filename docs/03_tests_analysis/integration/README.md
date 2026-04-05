# 集成测试分析

## 概览

mini-trantor 共有 **14 个集成测试文件**，验证完整的端到端场景。
集成测试启动真实的 TcpServer/TcpClient，建立 TCP 连接，完成数据收发。

## 测试清单

| # | 文件 | 测试目标 | 用例数 | 关键验证点 |
|---|------|----------|--------|------------|
| 1 | `tcp_server/test_tcp_server.cpp` | 单线程 echo | 1 | TcpServer + TcpClient echo 回环 |
| 2 | `tcp_server/test_tcp_server_threaded.cpp` | 多线程 echo | 2 | worker 线程亲和性验证、echo 正确性 |
| 3 | `tcp_server/test_tcp_server_backpressure.cpp` | 高水位背压 | 1 | 高水位回调在 worker 线程触发、forceClose |
| 4 | `tcp_server/test_tcp_server_backpressure_policy.cpp` | 背压策略 | 1 | TcpServer 级背压策略 pause/resume reading |
| 5 | `tcp_server/test_tcp_server_idle_timeout.cpp` | 空闲超时 | 1 | 空闲时间到达后连接被关闭 |
| 6 | `tcp_client/test_tcp_client.cpp` | 客户端 echo | 2 | echo 回环、server 主动关闭后重连 |
| 7 | `timer_queue/test_timer_queue_integration.cpp` | 定时器集成 | 6 | 单次延时、重复定时、取消、跨线程添加/取消、re-entrant add |
| 8 | `coroutine/test_coroutine_echo_server.cpp` | 协程 echo | 2 | 协程 session 在 worker 线程、read/close resume 线程验证 |
| 9 | `coroutine/test_coroutine_idle_timeout.cpp` | 协程空闲超时 | 2 | 正常响应不超时、空闲时连接被关闭 |
| 10 | `http/test_http_server.cpp` | HTTP 服务器 | 3 | GET 回环、POST body echo、HTTP/1.0 关闭行为 |
| 11 | `ws/test_ws_server.cpp` | WebSocket 服务器 | 3 | 多消息单连接、binary 回环、并发客户端 |
| 12 | `dns/test_dns_client.cpp` | DNS + 连接 | 2 | TcpClient hostname 连接 + echo、协程 resolve + connect 链 |
| 13 | `tls/test_tls_echo.cpp` | TLS echo | 3 | TLS echo 回环、协程 TLS echo、证书验证失败 |

## 测试模式分析

### Echo 回环模式（最常用）

```cpp
// 模式: 启动 server → 连接 client → 发送数据 → 验证收到相同数据
TcpServer server(&loop, addr, "echo");
server.setMessageCallback([](auto conn, auto* buf, auto) {
    conn->send(buf->retrieveAllAsString());
});
server.start();

// client 连接后
client.send("hello");
// ... 验证收到 "hello"
```

### 线程亲和性验证模式

```cpp
// 模式: 多线程 server，验证回调在正确的 worker 线程
server.setThreadNum(2);
server.setConnectionCallback([](auto conn) {
    assert(conn->getLoop()->isInLoopThread());
    // 回调在 connection 所属的 EventLoop 线程
});
```

### 定时器集成模式

```cpp
// 模式: 运行 EventLoop + 定时器，验证时序
loop.runAfter(50ms, [&] { fired = true; });
loop.runAfter(100ms, [&] { loop.quit(); });
loop.loop();
assert(fired);
```

### 协程集成模式

```cpp
// 模式: TcpServer + 协程 session handler
server.setCoroutineHandler([](TcpConnectionPtr conn) -> Task<void> {
    while (true) {
        auto data = co_await conn->asyncReadSome();
        if (data.empty()) break;
        co_await conn->asyncWrite(data);
    }
});
```

## 端到端场景覆盖

### 核心链路

```
Client connect → Server accept → Data echo → Client verify → Close
```

| 场景 | 覆盖测试 |
|------|----------|
| 单线程 TCP echo | tcp_server/test_tcp_server.cpp |
| 多线程 TCP echo | tcp_server/test_tcp_server_threaded.cpp |
| 客户端 echo + 重连 | tcp_client/test_tcp_client.cpp |
| 协程 echo (worker 线程) | coroutine/test_coroutine_echo_server.cpp |
| HTTP GET/POST | http/test_http_server.cpp |
| WebSocket text/binary | ws/test_ws_server.cpp |
| TLS echo | tls/test_tls_echo.cpp |
| DNS + TCP echo | dns/test_dns_client.cpp |

### 背压与流控

| 场景 | 覆盖测试 |
|------|----------|
| 高水位回调 | tcp_server/test_tcp_server_backpressure.cpp |
| 背压策略 pause/resume | tcp_server/test_tcp_server_backpressure_policy.cpp |

### 超时与取消

| 场景 | 覆盖测试 |
|------|----------|
| 空闲超时关闭连接 | tcp_server/test_tcp_server_idle_timeout.cpp |
| 协程空闲超时 | coroutine/test_coroutine_idle_timeout.cpp |
| 定时器取消 | timer_queue/test_timer_queue_integration.cpp |

## 测试质量评价

* **强项**: echo 场景覆盖全面（单线程/多线程/协程/TLS/HTTP/WS）
* **强项**: 定时器集成测试覆盖 6 种场景（含跨线程和 re-entrant）
* **强项**: TLS 集成测试同时覆盖回调和协程模式
* **改进点**: 并发连接压力测试较少（只有 WS 有 concurrent clients）
* **改进点**: 缺少网络异常场景（如连接超时、半关闭、RST）

## 测试总览统计

| 类别 | 文件数 | 总用例数（估计） |
|------|--------|------------------|
| 单元测试 | 15 | ~105 |
| 契约测试 | 17 | ~80 |
| 集成测试 | 14 | ~30 |
| **总计** | **46** | **~215** |
