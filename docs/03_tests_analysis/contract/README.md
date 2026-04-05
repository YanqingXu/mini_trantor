# 契约测试分析

## 概览

mini-trantor 共有 **17 个契约测试文件**，验证模块间的交互契约和线程亲和性。
契约测试运行真实的 EventLoop（通常在独立线程），但不涉及完整的端到端场景。

## 测试清单

| # | 文件 | 测试目标 | 用例数 | 关键验证点 |
|---|------|----------|--------|------------|
| 1 | `event_loop/test_event_loop.cpp` | EventLoop | 5 | runInLoop 内联执行、queueInLoop 跨线程、FIFO 顺序、quit 跨线程、嵌套 queue |
| 2 | `channel/test_channel_contract.cpp` | Channel | 5 | enableReading/disableAll、handleEvent 分发、tie 阻止过期回调、enable/disable 周期（真实 poll） |
| 3 | `poller/test_poller_contract.cpp` | Poller | 3 | channel add/enable/remove（eventfd）、read 事件从 poll 触发 |
| 4 | `buffer/test_buffer_contract.cpp` | Buffer | 5 | readFd 经 pipe、extra buffer 大数据路径、errno 传递、writeFd、EOF |
| 5 | `acceptor/test_acceptor.cpp` | Acceptor | 5 | listen+accept、无回调时关闭 fd、destroy-before-listen、destroy-after-listen、多次 accept |
| 6 | `connector/test_connector.cpp` | Connector | 4 | 成功连接交付 fd、连接拒绝触发重试、stop during pending、析构安全 |
| 7 | `tcp_connection/test_tcp_connection.cpp` | TcpConnection | ~15 | 背压策略、回调路径、forceClose 编排、高水位标记、读/写 awaiter、双 waiter 拒绝 |
| 8 | `tcp_server/test_tcp_server.cpp` | TcpServer | 4 | worker 线程亲和性、连接 map 可见性、错误线程拒绝、断开后移除 |
| 9 | `tcp_client/test_tcp_client.cpp` | TcpClient | 3 | 连接建立、断开关闭、跨线程 connect 编排到 owner loop |
| 10 | `event_loop_thread/test_event_loop_thread.cpp` | EventLoopThread | 2 | startLoop + queueInLoop 跨线程、quit 后析构 |
| 11 | `event_loop_thread_pool/test_event_loop_thread_pool.cpp` | EventLoopThreadPool | 4 | 零线程、2 线程 round-robin、错误线程 start、错误线程 next |
| 12 | `timer_queue/test_timer_queue.cpp` | TimerQueue | 4 | runAfter 触发、cancel 阻止、runEvery 重复、runAt 跨线程 |
| 13 | `coroutine/test_sleep_awaitable.cpp` | SleepAwaitable | 4 | 在 owner loop 恢复、pending 中 cancel、连续 sleep、时间精度 |
| 14 | `coroutine/test_combinator_contract.cpp` | WhenAll/WhenAny | 4 | WhenAll+SleepAwaitable、WhenAny 超时模式、void 组合器、句柄泄漏预防 |
| 15 | `http/test_http_server.cpp` | HttpServer | 4 | HttpCallback 线程亲和性、keep-alive、Connection: close、畸形请求 400 |
| 16 | `ws/test_ws_server.cpp` | WebSocket | 5 | upgrade 101、text echo、ping/pong、close 握手、无效升级 400 |
| 17 | `dns/test_dns_contract.cpp` | DnsResolver | 5 | 回调在正确 loop 线程、失败解析、并发解析、跨线程 resolve、缓存命中 |
| 17 | `tls/test_tls_handshake.cpp` | TLS | 4 | TLS 握手完成、TLS 读写、TLS shutdown、非 TLS 不受影响 |

## 测试模式分析

### 线程亲和性验证

```cpp
// 模式: 在独立线程运行 EventLoop，验证回调在哪个线程执行
auto loopThread = EventLoopThread();
auto* loop = loopThread.startLoop();
loop->queueInLoop([&] {
    assert(std::this_thread::get_id() == loop->threadId());
    // 回调在 EventLoop 线程执行
});
```

* TcpServer 验证连接回调在 worker loop 线程
* TcpClient 验证 connect 结果投递到 owner loop
* DnsResolver 验证回调在 callbackLoop 线程

### EventLoop 循环运行模式

```cpp
// 模式: 启动 loop 在独立线程，主线程发起操作，loop 线程验证结果
std::thread loopThread([&] { loop.loop(); });
// ... 发起操作 ...
loop.quit();
loopThread.join();
```

### TcpConnection 契约（最重要的契约测试，627 行）

验证的核心契约:
1. **背压策略**: 高水位回调触发 → 暂停读 → 低水位恢复读
2. **forceClose 编排**: forceClose 通过 queueInLoop 序列化
3. **协程 awaiter**: ReadAwaitable/WriteAwaitable 正确恢复
4. **双 waiter 拒绝**: 同时两个协程 co_await read 第二个被拒绝

### Acceptor 契约

验证:
1. 无回调时 accept 后立即关闭 fd（防止 fd 泄漏）
2. 析构前/后 listen 的安全性
3. while(true) accept 循环的多次连接

### WebSocket 契约

验证:
1. HTTP Upgrade → 101 Switching Protocols
2. Text 帧回显
3. Ping → Pong 自动回复
4. Close 握手（Client close → Server close echo → 连接关闭）
5. 非 WebSocket 升级请求 → 400

## 测试质量评价

* **强项**: TcpConnection 契约测试极其全面（~627 行，覆盖背压、生命周期、协程）
* **强项**: EventLoop 契约完整覆盖跨线程/FIFO/quit 语义
* **强项**: WebSocket 全链路契约覆盖 upgrade/消息/ping/close
* **改进点**: TLS 契约测试需要证书文件依赖（tests/certs/）
* **改进点**: EventLoopThread 契约偏简单（只有 2 个用例）
