# TcpServer —— 工程级源码拆解

## 1. 类定位

* **角色**：Acceptor、线程池、连接生命周期管理的**协调者**
* **层级**：Net 应用层最顶层（用户直接使用的入口类）
* TcpServer 在 base loop 线程运行，I/O 在 worker loop 线程运行

```
         用户代码 (任意线程)
              │
          TcpServer  ─────── base loop (Acceptor 在此运行)
              │
     EventLoopThreadPool
       ├── ioLoop-0
       ├── ioLoop-1
       └── ioLoop-N
              │
     每个 ioLoop 运行 TcpConnection
```

TcpServer **是整个服务器的"组装者"**：把 Acceptor 的 accept 事件、线程池的 loop 分配、TcpConnection 的生命周期绑定在一起。

---

## 2. 解决的问题

**核心问题**：如何管理多个 TCP 连接，使每个连接绑定到一个独立的 EventLoop，同时保证连接的创建和销毁不存在数据竞争？

如果没有 TcpServer：
- 用户需要手动管理 Acceptor + 线程池 + 连接映射
- accept 到新连接后，需要手动选 loop，创建 TcpConnection，设置回调，启动
- 连接关闭时，需要手动从映射中移除（跨线程操作）
- 空闲连接超时检测需要自建定时器逻辑

TcpServer 的设计价值：
1. **连接映射集中管理**：`connections_` 只在 base loop 线程操作
2. **跨 loop 移除安全**：closeCallback → removeConnection → runInLoop（base loop） → connectDestroyed（ioLoop）
3. **idleTimeout 策略**：每个连接独立定时器，由 ioLoop 管理
4. **lifetimeToken 保护**：析构后的回调通过 `weak_ptr` 安全失效

---

## 3. 对外接口（API）

### 配置（start 之前调用）

| 方法 | 用途 |
|------|------|
| `setThreadNum(n)` | 设置 worker 线程数（0 = 单线程，base loop 兼做 I/O） |
| `setIdleTimeout(duration)` | 配置空闲连接超时时间 |
| `setBackpressurePolicy(high, low)` | 配置全局背压水位（覆盖所有新连接） |
| `setThreadInitCallback(cb)` | worker 线程启动时的初始化回调 |
| `enableSsl(tlsContext)` | 开启 TLS（所有新连接自动握手） |
| `setConnectionCallback(cb)` | 连接建立/断开回调 |
| `setMessageCallback(cb)` | 收到数据回调 |
| `setHighWaterMarkCallback(cb, mark)` | 写缓冲高水位回调 |
| `setWriteCompleteCallback(cb)` | 写缓冲清空回调 |

### 运行时

| 方法 | 线程安全 | 用途 |
|------|----------|------|
| `start()` | 跨线程安全（atomic CAS） | 启动线程池 + 开始监听 |
| `connectionCount()` | base loop 线程 | 当前连接数 |

---

## 4. 核心成员变量

```cpp
EventLoop* loop_;                          // base loop（Acceptor 的 owner loop）

// ===== 监听与连接建立 =====
std::unique_ptr<Acceptor> acceptor_;       // listenfd 管理（base loop 线程）
std::shared_ptr<EventLoopThreadPool> threadPool_; // worker loops（base loop 线程管理）

// ===== 用户回调（注入到所有 TcpConnection）=====
ConnectionCallback connectionCallback_;
MessageCallback messageCallback_;
HighWaterMarkCallback highWaterMarkCallback_;
WriteCompleteCallback writeCompleteCallback_;
ThreadInitCallback threadInitCallback_;

// ===== 运行状态 =====
std::atomic<bool> started_;               // 防止 start() 重复调用（CAS 保护）
int nextConnId_;                          // 连接 ID 自增（base loop 线程独占）

// ===== 策略参数 =====
std::size_t highWaterMark_;               // highWaterMarkCallback 触发阈值
std::size_t backpressureHighWaterMark_;   // 背压高水位
std::size_t backpressureLowWaterMark_;    // 背压低水位
Duration idleTimeout_;                    // 空闲超时（Duration::zero() 表示禁用）

// ===== 连接生命周期 =====
std::unordered_map<std::string, TcpConnectionPtr> connections_; // base loop 线程独占
std::shared_ptr<void> lifetimeToken_;     // 析构时 reset，用于守护延迟回调

// ===== TLS =====
std::shared_ptr<TlsContext> tlsContext_;  // 共享的 SSL_CTX 包装
```

---

## 5. 执行流程（关键路径）

### 5.1 启动 start()

```cpp
void TcpServer::start() {
    bool expected = false;
    if (started_.compare_exchange_strong(expected, true)) {  // 防重复 start
        threadPool_->start(threadInitCallback_);              // 启动 worker threads
        loop_->runInLoop([this] { acceptor_->listen(); });    // base loop 上开始监听
    }
}
```

`started_` 用 `atomic CAS` 而不是 `loop_->assertInLoopThread()`，是因为 start() 允许从任意线程调用一次。

### 5.2 新连接到达 newConnection

```
Acceptor::handleRead() → newConnectionCallback_(connfd, peerAddr)
    ↓ (base loop 线程)
TcpServer::newConnection(connfd, peerAddr):

1. 选 ioLoop：  EventLoop* ioLoop = threadPool_->getNextLoop()
2. 生成名称：   name_ + "#" + nextConnId_++
3. 创建连接：   auto conn = make_shared<TcpConnection>(ioLoop, name, fd, ...)
4. 注册到 map： connections_[connName] = conn
5. 注入回调：   connectionCallback / messageCallback / backpressure / highWaterMark / writeComplete
6. 注入关闭回调（含 idleState 和 lifetimeToken 守护）
7. ioLoop→runInLoop：
   → 若有 TLS：conn->startTls(ctx, isServer) + conn->connectEstablished()
   → 否则：conn->connectEstablished()
```

**为什么 connectEstablished 要通过 runInLoop 在 ioLoop 上执行？**
TcpConnection 的所有状态修改（包括 Channel::tie + enableReading）必须在其 owner loop（ioLoop）上操作，而 newConnection 在 base loop 上执行。

### 5.3 连接关闭的跨线程回流

```
ioLoop 线程：
  TcpConnection::handleClose()
    → closeCallback_(conn)
        → TcpServer 注入的 lambda：
            if (!lifetime.lock()) return;  // TcpServer 已析构，安全退出
            removeConnection(conn)         // 跨线程 marshal 到 base loop

base loop 线程：
  TcpServer::removeConnectionInLoop(conn):
    1. connections_.erase(conn->name())   // 从 map 中移除（base loop 独占）
    2. ioLoop->queueInLoop([conn] { conn->connectDestroyed(); })

ioLoop 线程：
  TcpConnection::connectDestroyed()
    → channel_->remove()                  // 从 epoll 中注销
    → conn 的 shared_ptr 计数归零 → 析构
```

### 5.4 空闲超时（idleTimeout）

```cpp
struct IdleTimeoutState {
    EventLoop* loop;           // ioLoop
    weak_ptr<TcpConnection> connection;
    Duration timeout;
    TimerId timerId;
    uint64_t generation;       // 版本号，用于取消旧定时器
};
```

每次：
- 连接建立：`refreshIdleTimer`（启动定时器）
- 收到数据：`refreshIdleTimer`（重置定时器）
- 写完成：`refreshIdleTimer`
- 定时器到期：检查 generation，若一致则 `conn->forceClose()`
- 连接关闭：`cancelIdleTimer`

定时器运行在 `ioLoop`（connection 的 owner loop），避免跨 loop timer 操作。

### 5.5 析构（优雅关闭）

```cpp
TcpServer::~TcpServer() {
    loop_->assertInLoopThread();
    lifetimeToken_.reset();                // 所有延迟回调 lifetime.lock() 返回空，安全终止
    acceptor_->setNewConnectionCallback({});  // 停止接受新连接

    for (auto& [name, conn] : connections_) {
        auto c = conn;
        c->getLoop()->runInLoop([c] {
            c->setCloseCallback({});       // 清除 closeCallback（防循环引用）
            c->connectDestroyed();         // 在 ioLoop 上销毁连接
        });
    }
    // connections_ 清空（本 loop 的析构不等待 ioLoop 完成）
}
```

---

## 6. 协作关系

```
         用户代码
              │ TcpServer server(loop, addr, "name")
              │ server.setMessageCallback(...)
              │ server.start()
              ▼
         TcpServer  ──owns──► Acceptor (base loop)
              │               │ EPOLLIN → accept → newConnection
              │               ▼
              owns    EventLoopThreadPool
              │         └── EventLoopThread × N
              │                    └── EventLoop (ioLoop)
              │                              │
              creates              TcpConnection
              (base loop)                   │
              maps in                ┌──────┴──────┐
           connections_           Socket        Channel
              │                              (ioLoop 的 epoll)
              │ closeCallback (ioLoop) → removeConnection (base loop)
              │                             → connectDestroyed (ioLoop)
```

| 关系 | 描述 |
|------|------|
| `TcpServer` → `Acceptor` | `unique_ptr`，base loop 线程 |
| `TcpServer` → `EventLoopThreadPool` | `shared_ptr`，base loop 线程管理 |
| `TcpServer` → `TcpConnection` | `shared_ptr` + `unordered_map`，base loop 线程 |
| `TcpConnection` → `TcpServer` | 无直接引用，通过 closeCallback lambda 捕获 `lifetimeToken` |

---

## 7. 关键设计点

### 7.1 lifetimeToken 哨兵

```cpp
std::shared_ptr<void> lifetimeToken_ = std::make_shared<int>(0);

// closeCallback 中：
connection->setCloseCallback([this, lifetime = weak_ptr(lifetimeToken_)](const TcpConnectionPtr& conn) {
    if (!lifetime.lock()) return;   // TcpServer 已析构
    removeConnection(conn);
});
```

closeCallback 通过 `runInLoop` 延迟执行，可能跑在 TcpServer 析构之后。
`lifetimeToken_` 在析构时 reset，`lifetime.lock()` 返回 nullptr，安全退出。

### 7.2 连接映射对 base loop 的独占

`connections_` 只在 base loop 线程操作：
- `newConnection`（base loop）：插入
- `removeConnectionInLoop`（base loop）：删除
- `connectionCount()`（base loop）：读取

任何 ioLoop 线程的操作（close、error）都通过 `runInLoop` 回流到 base loop 再操作映射。

### 7.3 TLS 时序

```
TLS 的 startTls 必须在 connectEstablished 之前：
    ioLoop->runInLoop([connection, ctx] {
        connection->startTls(ctx, true);     // 配置 SSL session
        connection->connectEstablished();     // 开始监听事件（触发握手）
    });
```

两个调用在同一个 `runInLoop` lambda 中按顺序执行，确保 SSL 初始化先于 Channel::enableReading。

### 7.4 start() 的线程安全

```cpp
std::atomic<bool> started_;
// ...
bool expected = false;
if (started_.compare_exchange_strong(expected, true)) { ... }
```

用 CAS 而不是 mutex，使 start() 可以从任意线程安全调用一次（典型用法：主线程构造 TcpServer，在 main 中调用 start() 进入 loop）。

---

## 8. 核心模块改动闸门（5 问）

1. **哪个 loop/线程拥有此模块？**
   base loop（TcpServer 构造时传入的 EventLoop）拥有 TcpServer、Acceptor、connections_ 映射。每个 TcpConnection 归属对应的 ioLoop。

2. **谁拥有它，谁释放它？**
   用户代码（通常是 main 函数）以栈对象或 unique_ptr 持有 TcpServer。析构时 TcpServer 调用各连接的 connectDestroyed（通过 ioLoop runInLoop），连接的 shared_ptr 引用归零后自然析构。

3. **哪些回调可能重入？**
   `newConnection` 在 base loop 的 epoll 分发期间执行，此时不应调用任何触发 Acceptor handleRead 的操作（实际场景不存在）。`closeCallback` 在 ioLoop 的分发期间执行，通过 runInLoop 回流到 base loop，不会重入 connections_ 操作。

4. **哪些操作允许跨线程，如何 marshal？**
   - `start()`：任意线程（atomic CAS 保护）
   - `removeConnection`：ioLoop 线程调用，通过 `loop_->runInLoop` marshal 到 base loop
   - `connectEstablished`：base loop 通过 `ioLoop->runInLoop` marshal 到 ioLoop
   - 业务回调（messageCallback 等）：在 ioLoop 线程执行，用户代码需注意

5. **哪个测试文件验证此改动？**
   `tests/contract/test_tcp_server.cc`、`tests/integration/` 中的端到端测试

---

## 9. 从极简到完整的演进路径

```cpp
// 极简版本（单线程，无线程池）
class MinimalTcpServer {
    int listenfd_;
    EventLoop* loop_;
    std::map<int, std::shared_ptr<TcpConnection>> conns_;
public:
    void start() {
        // listen
        // epoll ADD listenfd
        // 回调中 accept + 创建 TcpConnection
    }
};
```

从极简 → 完整，需要加：

1. **Acceptor 抽象** → 单一职责，listen/accept 独立
2. **EventLoopThreadPool** → one-loop-per-thread
3. **connections_ 映射** → 集中管理生命周期
4. **跨 loop 的 closeCallback 回流** → 线程安全移除
5. **lifetimeToken** → 防析构后回调
6. **idleTimeout** → 空闲连接回收
7. **背压配置** → 防内存溢出
8. **TLS 集成** → 握手时序控制
9. **start() CAS 保护** → 幂等启动

---

## 10. 易错点与最佳实践

### 易错点

| 错误 | 后果 |
|------|------|
| 在非 base loop 线程访问 connections_ | 数据竞争 |
| TcpServer 在非 base loop 线程析构 | abort（assertInLoopThread） |
| start() 前未设置 messageCallback | 数据到来时被丢弃（无 messageCallback null check） |
| enableSsl 在 start() 之后调用 | 已建立的连接不走 TLS，新连接走 TLS，行为不一致 |
| setThreadNum(0) 但使用多 CPU | 全部 I/O 在 base loop，成为性能瓶颈 |

### 最佳实践

```cpp
// ✓ 标准 TcpServer 启动模板：
EventLoop loop;
InetAddress listenAddr(8080);
TcpServer server(&loop, listenAddr, "EchoServer");

server.setConnectionCallback(onConnection);
server.setMessageCallback(onMessage);
server.setThreadNum(std::thread::hardware_concurrency()); // 充分利用 CPU
server.start();

loop.loop();  // base loop 进入事件循环

// ✓ 开启 TLS（在 start 前）：
auto tlsCtx = std::make_shared<TlsContext>();
tlsCtx->load_cert_chain_file("cert.pem");
tlsCtx->use_private_key_file("key.pem");
server.enableSsl(tlsCtx);
server.start();
```

---

## 11. 面试角度总结

**Q1: TcpServer 的线程模型是什么？**
A: one-loop-per-thread。base loop 运行 Acceptor（accept 新连接）和维护 connections_ 映射。每个 worker loop 拥有若干 TcpConnection，独立运行 I/O 事件分发。

**Q2: newConnection 之后为什么用 ioLoop->runInLoop 调用 connectEstablished？**
A: `newConnection` 在 base loop 线程执行，而 TcpConnection 的 owner 是 ioLoop。`Channel::enableReading`、`tie` 等操作必须在 ioLoop 线程执行，所以通过 `runInLoop` marshal 过去。

**Q3: 连接的 closeCallback 为什么要绕一大圈？**
A: closeCallback 在 ioLoop 调用（`handleClose` 在 ioLoop），但 `connections_` 只能在 base loop 修改，所以必须 `runInLoop` 回流到 base loop 执行 `removeConnectionInLoop`。移除后再通过 `queueInLoop` 回到 ioLoop 执行 `connectDestroyed`。

**Q4: lifetimeToken 解决什么问题？**
A: closeCallback 通过 `runInLoop` 延迟执行，可能在 TcpServer 已析构后才运行。`lifetimeToken` 是一个 shared_ptr，TcpServer 析构时 reset，closeCallback 检查 `lifetime.lock()` 为空则安全退出，不访问已析构的 TcpServer。

**Q5: idleTimeout 的定时器在哪个 loop 上运行？**
A: `ioLoop`（连接的 owner loop）。避免在 base loop 操作 ioLoop 下面的连接（跨 loop 操作），定时器 timerfd 注册在 ioLoop 的 TimerQueue 中。

**Q6: start() 为什么用 atomic CAS 而不是 mutex？**
A: start() 只需要"最多执行一次"的语义，CAS 比 mutex 轻量，且允许从任意线程调用。start() 是一次性操作，不需要后续的 unlock。
