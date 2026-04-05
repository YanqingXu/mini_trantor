# TcpServer —— 工程级源码拆解

## 1. 类定位

* **角色**：面向用户的**服务器入口**，协调 Acceptor、线程池和连接管理
* **层级**：Net 层（最顶层用户接口）
* TcpServer 把 accept、线程分配、连接生命周期、callback 设置统一封装

```
用户代码:
  TcpServer server(loop, addr, "Echo");
  server.setMessageCallback(onMessage);
  server.setThreadNum(4);
  server.start();
  loop->loop();
         │
         ▼
    ┌─────────────────────────────┐
    │         TcpServer            │ ◄── 本文主角
    │  Acceptor + ThreadPool       │
    │  connections_ map            │
    │  idle timeout / backpressure │
    └─────────────────────────────┘
         │                │
         ▼                ▼
    base loop       worker loops
    (accept)        (I/O 处理)
```

## 2. 解决的问题

**核心问题**：如何安全地协调"接受新连接"、"分配到线程"、"管理连接生命周期"？

如果没有 TcpServer：
- 用户需要自己写 accept 循环 + 线程分配逻辑
- 需要自己维护 connection map + 跨线程安全的移除
- 需要自己实现空闲超时、背压策略
- 析构时的安全清理非常容易出错

TcpServer 封装了：
1. **Acceptor** → 监听 + accept
2. **EventLoopThreadPool** → worker 线程管理
3. **connections_ map** → 连接生命周期跟踪
4. **IdleTimeoutState** → 空闲超时（带 generation 计数器）
5. **lifetimeToken_** → 安全析构保护

## 3. 对外接口（API）

### 配置（start 前调用）

| 方法 | 用途 |
|------|------|
| `setThreadNum(n)` | 设置 worker 线程数（0 = 共用 base loop） |
| `setIdleTimeout(duration)` | 设置空闲超时（自动 forceClose） |
| `setBackpressurePolicy(hi, lo)` | 设置背压策略 |
| `enableSsl(tlsContext)` | 启用 TLS |
| `setThreadInitCallback(cb)` | 线程初始化回调 |

### 回调设置

| 方法 | 回调时机 |
|------|----------|
| `setConnectionCallback(cb)` | 连接建立或断开 |
| `setMessageCallback(cb)` | 收到数据 |
| `setHighWaterMarkCallback(cb, mark)` | outputBuffer 超过阈值 |
| `setWriteCompleteCallback(cb)` | outputBuffer 清空 |

### 控制

| 方法 | 用途 |
|------|------|
| `start()` | 启动线程池 + 开始监听 |
| `connectionCount()` | 当前连接数（仅 base loop 线程） |

## 4. 核心成员变量

```cpp
EventLoop* loop_;                                // base loop（accept 线程）
const std::string name_;                         // 服务器名称
std::unique_ptr<Acceptor> acceptor_;             // 监听 socket 管理
std::shared_ptr<EventLoopThreadPool> threadPool_;// worker 线程池

// ===== 用户回调 =====
ConnectionCallback connectionCallback_;
MessageCallback messageCallback_;
HighWaterMarkCallback highWaterMarkCallback_;
WriteCompleteCallback writeCompleteCallback_;
ThreadInitCallback threadInitCallback_;

// ===== 状态 =====
std::atomic<bool> started_;                      // 是否已 start（防重复调用）
int nextConnId_;                                 // 连接 ID 递增计数器

// ===== 策略 =====
std::size_t highWaterMark_;                      // 高水位回调阈值
std::size_t backpressureHighWaterMark_;          // 背压高水位
std::size_t backpressureLowWaterMark_;           // 背压低水位
Duration idleTimeout_;                           // 空闲超时时间

// ===== 连接管理 =====
std::unordered_map<std::string, TcpConnectionPtr> connections_;  // name → conn
std::shared_ptr<void> lifetimeToken_;            // 析构保护令牌

// ===== TLS =====
std::shared_ptr<TlsContext> tlsContext_;         // TLS 上下文（可选）
```

### 生命周期

| 变量 | 所属线程 | 说明 |
|------|----------|------|
| `connections_` | base loop | 只在 base loop 线程读写 |
| `nextConnId_` | base loop | 只在 newConnection 中递增 |
| `started_` | 任意线程 | atomic |
| 其余 | base loop | 只在构造/start 前设置 |

## 5. 执行流程（最重要）

### 5.1 构造

```
TcpServer::TcpServer(loop, listenAddr, name, reusePort)
  ├─ loop_ = loop (base loop)
  ├─ acceptor_ = new Acceptor(loop, listenAddr, reusePort)
  ├─ threadPool_ = new EventLoopThreadPool(loop, name)
  ├─ lifetimeToken_ = make_shared<int>(0)
  └─ acceptor_->setNewConnectionCallback(newConnection)
```

### 5.2 start

```
server.start()
  ├─ atomic CAS: started_ false → true（防重复调用）
  ├─ threadPool_->start(threadInitCallback_)
  │   ├─ 创建 numThreads 个 EventLoopThread
  │   └─ 每个线程启动 loop()
  └─ loop_->runInLoop([this] { acceptor_->listen(); })
      └─ acceptor_ 开始监听 → enableReading
```

### 5.3 新连接到来

```
epoll_wait → EPOLLIN on listen fd
  → Acceptor::handleRead()
      → int connfd = ::accept4(listenfd, ...)
      → newConnectionCallback_(connfd, peerAddr)
          → TcpServer::newConnection(connfd, peerAddr)
```

```
newConnection(sockfd, peerAddr):           // base loop 线程
  │
  ├─ ioLoop = threadPool_->getNextLoop()   // round-robin 选择 worker
  ├─ connName = "ServerName#42"
  ├─ localAddr = getLocalAddr(sockfd)
  │
  ├─ conn = make_shared<TcpConnection>(ioLoop, connName, sockfd, ...)
  ├─ connections_[connName] = conn         // 加入 map（base loop 线程）
  │
  ├─ 设置 idle timeout（如果启用）:
  │   ├─ idleState = { loop, weak_ptr<conn>, timeout, generation }
  │   └─ 包装用户 callback → 每次活动刷新 timer
  │
  ├─ 设置回调:
  │   ├─ connectionCallback → 包装（含 idle 刷新）
  │   ├─ messageCallback → 包装（含 idle 刷新）
  │   ├─ writeCompleteCallback → 包装（含 idle 刷新）
  │   ├─ closeCallback → removeConnection（含 lifetime 检查）
  │   └─ backpressurePolicy / highWaterMark
  │
  ├─ TLS? → ioLoop->runInLoop([conn, ctx] {
  │           conn->startTls(ctx, true);
  │           conn->connectEstablished();
  │         })
  └─ else → ioLoop->runInLoop([conn] { conn->connectEstablished(); })
```

### 5.4 移除连接

```
handleClose (worker loop)
  → closeCallback (worker loop)
      → TcpServer::removeConnection(conn)
          → loop_->runInLoop(removeConnectionInLoop)    // 投递到 base loop

removeConnectionInLoop (base loop):
  ├─ if (!lifetimeToken_.lock()) return    // TcpServer 已析构
  ├─ connections_.erase(conn->name())      // 从 map 移除
  └─ ioLoop->queueInLoop([conn] { conn->connectDestroyed(); })
```

**为什么 removeConnection 要投递到 base loop？**

因为 `connections_` 只在 base loop 线程修改。worker loop 不能直接操作 base loop 的数据结构。

### 5.5 析构

```
~TcpServer():                              // 必须在 base loop 线程
  ├─ assertInLoopThread()
  ├─ lifetimeToken_.reset()                 // 使所有 weak_ptr 失效
  ├─ acceptor_->setNewConnectionCallback({})// 停止 accept
  │
  └─ for (auto& [name, connection] : connections_):
      ├─ auto conn = connection
      └─ conn->getLoop()->runInLoop([conn] {
            conn->setCloseCallback({});     // 断开 closeCallback
            conn->connectDestroyed();       // 清理
         })
```

## 6. 关键交互关系

```
           TcpServer (base loop)
          ┌─────┴──────┐
          │             │
     Acceptor    EventLoopThreadPool
     (listen)    (worker loop ×N)
          │             │
          ▼             ▼
     newConnection ──► round-robin ──► ioLoop
          │
          ▼
     TcpConnection (worker loop)
          │
          ├─ connectionCallback → User
          ├─ messageCallback → User
          └─ closeCallback → TcpServer::removeConnection
```

| 类 | 关系 |
|----|------|
| **Acceptor** | TcpServer 独占，处理 accept |
| **EventLoopThreadPool** | TcpServer 共享持有，管理 worker 线程 |
| **TcpConnection** | TcpServer 通过 connections_ map 持有 shared_ptr |
| **EventLoop** | TcpServer 使用 base loop，TcpConnection 使用 worker loop |

## 7. 关键设计点

### 7.1 lifetimeToken_ —— 安全析构

```cpp
// 构造时
lifetimeToken_ = make_shared<int>(0);

// 析构时
lifetimeToken_.reset();  // 引用计数归零

// closeCallback 中
connection->setCloseCallback([this, lifetime, ...](const TcpConnectionPtr& conn) {
    if (!lifetime.lock()) return;  // TcpServer 已析构，安全跳过
    removeConnection(conn);
});
```

**问题**：worker loop 上排队的 closeCallback 可能在 TcpServer 析构后才执行。
**解法**：通过 `weak_ptr<void>` 检测 TcpServer 是否还存活。

### 7.2 IdleTimeoutState —— 空闲超时

```cpp
struct IdleTimeoutState {
    EventLoop* loop;
    std::weak_ptr<TcpConnection> connection;
    Duration timeout;
    TimerId timerId;
    std::uint64_t generation{0};   // 代计数器
};
```

每次连接活动（消息、写完成、连接建立）→ `refreshIdleTimer()`：
1. `cancelIdleTimer()`: 取消旧 timer + 递增 generation
2. 创建新 timer → `runAfter(timeout, [idleState, generation])`
3. timer 到期时检查 `generation` 是否匹配（旧 timer 可能已被刷新）

**为什么需要 generation 而不是只用 cancel？**

因为 `TimerQueue::cancel` 可能在 timer 已经进入 `getExpired` 后才调用，
此时 timer 已从 map 移除但回调等待执行。generation 检查是额外的安全网。

### 7.3 回调包装

TcpServer 不直接把用户回调设给 TcpConnection，而是包装一层：

```cpp
connection->setConnectionCallback([cb = connectionCallback_, idleState](const TcpConnectionPtr& conn) {
    if (idleState) {
        conn->connected() ? refreshIdleTimer(idleState) : cancelIdleTimer(idleState);
    }
    if (cb) cb(conn);
});
```

这样空闲超时、lifetime 检查等逻辑对用户透明。

### 7.4 round-robin 线程分配

```
threadPool_->getNextLoop()
  → loops_[next_] → next_ = (next_ + 1) % loops_.size()
```

简单高效，保证连接在 worker loop 间均匀分布。

## 8. 潜在问题

### 8.1 connections_ 的并发访问

`connections_` 只在 base loop 线程修改（newConnection / removeConnectionInLoop）。
如果在其他线程调用 `connectionCount()`，会触发 `assertInLoopThread()` 检查。

### 8.2 析构中的 runInLoop

析构时对每个连接调用 `conn->getLoop()->runInLoop(...)`。
如果某个 worker loop 已经 quit，runInLoop 会怎样？

`runInLoop` 中：如果是 loop 线程则直接执行，否则 queueInLoop。
如果 worker loop 已退出但还没析构，queueInLoop 只是 push 到 vector + wakeup。
由于 loop 已退出，这些回调不会被执行。

**风险**：worker loop 比 TcpServer 先析构的话，TcpConnection 和 Channel 可能残留。
**约束**：TcpServer 必须在 worker loop 之前析构。threadPool_ 是 shared_ptr，析构顺序可控。

### 8.3 nextConnId_ 溢出

`int nextConnId_` 理论上可能溢出。`INT_MAX` ≈ 21亿，实际不太可能达到。

## 9. 极简实现

```cpp
class MinimalTcpServer {
public:
    MinimalTcpServer(int port) {
        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        bind(listenFd_, ...);
        listen(listenFd_, SOMAXCONN);
    }

    void loop() {
        while (true) {
            int connfd = accept(listenFd_, ...);
            onNewConnection(connfd);
        }
    }

private:
    void onNewConnection(int fd) {
        // 直接在当前线程处理
        connections_[fd] = std::make_shared<TcpConnection>(fd);
    }

    int listenFd_;
    std::map<int, std::shared_ptr<TcpConnection>> connections_;
};
```

完整版增加：
1. **Acceptor** → 非阻塞 accept + Channel 事件驱动
2. **ThreadPool** → worker loop 分配
3. **lifetimeToken** → 安全析构
4. **IdleTimeout** → 空闲超时
5. **TLS** → 透明加密
6. **背压策略** → 流量控制

## 10. 面试角度总结

**Q1: TcpServer 如何将新连接分配到 worker 线程？**
A: `accept` 在 base loop 线程执行，通过 `threadPool_->getNextLoop()` round-robin 选择 worker loop，
然后 `ioLoop->runInLoop(connectEstablished)` 投递到 worker。

**Q2: 连接移除为什么要经过 base loop？**
A: 因为 `connections_` map 只在 base loop 线程维护。worker loop 的 closeCallback 通过 `runInLoop` 投递到 base loop 移除。

**Q3: lifetimeToken 解决什么问题？**
A: 防止 TcpServer 析构后，排队在 base loop 上的 closeCallback 仍然访问已销毁的 TcpServer。
通过 `weak_ptr::lock()` 检测存活性。

**Q4: IdleTimeout 的 generation 计数器有什么用？**
A: 防止已被刷新的旧 timer 回调仍然执行。cancel 可能来不及阻止已 fired 的 timer，
generation 检查是最后一道防线。

**Q5: TcpServer 析构时如何清理所有连接？**
A: 先 `lifetimeToken_.reset()`，然后遍历 `connections_`，
对每个连接在其 ioLoop 上调用 `connectDestroyed()`。

**Q6: start() 为什么用 atomic CAS？**
A: 防止重复调用 start()。CAS 保证只有第一次调用生效。
