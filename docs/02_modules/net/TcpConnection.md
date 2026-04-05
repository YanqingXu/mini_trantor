# TcpConnection —— 工程级源码拆解

## 1. 类定位

* **角色**：单个 TCP 连接的**核心承载体**，统一管理连接状态、缓冲区、Channel 事件和 coroutine 恢复入口
* **层级**：Net 应用层（坐落在 Reactor 核心层之上）
* 继承 `enable_shared_from_this`，生命周期由 `shared_ptr` 管理

```
         TcpServer (base loop)
               │ 构造 + connectEstablished
               ▼
         TcpConnection  ──── std::shared_ptr ────► 用户持有
               │
       ┌───────┼──────────┐
       │       │          │
    Socket   Channel   Buffer×2
     (fd)   (epoll)   (in/out)
       │       │
       └───────┴──► ioLoop (owner loop)
```

TcpConnection **不拥有 EventLoop**，但**绑定到唯一一个 ioLoop**，所有状态变更都在该 loop 线程执行。

---

## 2. 解决的问题

**核心问题**：TCP 连接有生命周期（connecting → connected → disconnecting → disconnected），读写、关闭、错误都可能在不同时机发生。如何在单线程模型下安全地管理？

如果没有 TcpConnection：
- 每个连接的状态（fd/buffer/callbacks）分散在回调函数中 → 无法保证原子性关闭
- 跨线程 send 需要手动 marshal → 容易数据竞争
- close 和 error 可能走不同路径 → 双重 close fd 或遗漏 epoll 注销
- 没有 shared_ptr 保护 → 回调期间对象已销毁

TcpConnection 的设计价值：
1. **状态机**：4 个状态严格约束操作合法性
2. **Loop 线程亲和**：所有 handle* 方法 + sendInLoop + shutdownInLoop 在 loop 线程
3. **跨线程安全 send**：自动检测线程，非 loop 线程则 copy + runInLoop
4. **统一关闭路径**：close/error/shutdown 都收敛到 `handleClose()`
5. **coroutine 集成**：三种 awaitable 不绕过 EventLoop 调度

---

## 3. 对外接口（API）

### 用户（业务层）调用

| 方法 | 线程安全 | 用途 |
|------|----------|------|
| `send(string_view)` | 跨线程安全 | 发送数据 |
| `send(void*, len)` | 跨线程安全 | 发送数据（原始指针版） |
| `shutdown()` | 跨线程安全 | 优雅关闭（半关，等写完） |
| `forceClose()` | 跨线程安全 | 强制关闭（立即） |
| `setTcpNoDelay(bool)` | loop 线程 | 设置 TCP_NODELAY |
| `setBackpressurePolicy(high, low)` | 跨线程安全 | 配置背压策略 |
| `setContext(any)` / `getContext()` | loop 线程 | 挂载业务上下文 |

### TLS 支持

| 方法 | 线程安全 | 用途 |
|------|----------|------|
| `startTls(ctx, isServer, hostname)` | loop 线程 | 激活 TLS，必须在 connectEstablished 前 |
| `isTlsEstablished()` | loop 线程 | 查询握手是否完成 |

### 回调设置（TcpServer 注入）

| 方法 | 触发时机 |
|------|----------|
| `setConnectionCallback(cb)` | 连接建立或断开 |
| `setMessageCallback(cb)` | 收到数据 |
| `setHighWaterMarkCallback(cb, mark)` | outputBuffer 超高水位 |
| `setWriteCompleteCallback(cb)` | outputBuffer 清空 |
| `setCloseCallback(cb)` | 连接关闭（TcpServer 用此移除连接） |

### 生命周期控制（TcpServer 调用）

| 方法 | 调用者 | 用途 |
|------|--------|------|
| `connectEstablished()` | TcpServer（ioLoop） | 正式建立连接，tie Channel |
| `connectDestroyed()` | TcpServer（ioLoop） | 彻底销毁，移除 Channel |

### Coroutine Awaitables

| 方法 | 功能 |
|------|------|
| `asyncReadSome(minBytes)` | co_await 等待读取至少 minBytes 字节 |
| `asyncWrite(data)` | co_await 等待写完成 |
| `waitClosed()` | co_await 等待连接关闭 |

---

## 4. 核心成员变量

```cpp
// ===== 线程亲和 =====
EventLoop* loop_;                   // owner loop（ioLoop），不拥有

// ===== 连接状态 =====
std::string name_;                  // 唯一名称（base loop 分配）
StateE state_;                      // kConnecting/kConnected/kDisconnecting/kDisconnected
bool reading_;                      // 是否正在读取（背压暂停时为 false）

// ===== 网络资源 =====
std::unique_ptr<Socket> socket_;    // RAII fd 包装（析构时 close）
std::unique_ptr<Channel> channel_;  // fd 的事件代理（绑定到 ioLoop）
InetAddress localAddr_, peerAddr_;

// ===== 缓冲区 =====
Buffer inputBuffer_;                // 入站字节缓冲
Buffer outputBuffer_;               // 出站字节缓冲（write 不完时暂存）

// ===== 回调 =====
ConnectionCallback connectionCallback_;
MessageCallback messageCallback_;
HighWaterMarkCallback highWaterMarkCallback_;
WriteCompleteCallback writeCompleteCallback_;
CloseCallback closeCallback_;       // ⟵ TcpServer 用此清理 connections_ 映射

// ===== 背压 =====
std::size_t highWaterMark_;         // 触发 highWaterMarkCallback 的阈值
std::size_t backpressureHighWaterMark_;  // 触发读暂停的阈值
std::size_t backpressureLowWaterMark_;   // 恢复读取的阈值

// ===== TLS =====
TlsState tlsState_;                 // kTlsNone/kTlsHandshaking/kTlsEstablished/kTlsShuttingDown
std::shared_ptr<TlsContext> tlsContext_;
SSL* ssl_;                          // OpenSSL session（裸指针，析构时 SSL_free）

// ===== Coroutine waiters =====
ReadAwaiterState readWaiter_;       // 当前等待的读协程
WriteAwaiterState writeWaiter_;     // 当前等待的写协程
CloseAwaiterState closeWaiter_;     // 当前等待关闭的协程

// ===== 背景说明 =====
std::any context_;                  // 用户自定义上下文（透明存储）
```

---

## 5. 执行流程（关键路径）

### 5.1 连接状态机

```
              构造
               │ state_ = kConnecting
               ▼
         connectEstablished()
               │ state_ = kConnected
               │ channel_->tie(shared_from_this())
               │ channel_->enableReading()
               │ connectionCallback_(connected)
               ▼
         [正常收发阶段]
               │
        ┌──────┴──────┐
        │             │
    shutdown()    forceClose()
        │             │
 state_=kDisconnecting │
        │             │
    handleWrite()     │
    （等输出清空）      │
        │             │
        └──────┬──────┘
               ▼
          handleClose()
               │ state_ = kDisconnected
               │ channel_->disableAll()
               │ connectionCallback_(disconnected)
               │ closeCallback_()  ──► TcpServer::removeConnection
               ▼
         connectDestroyed()  (由 TcpServer 在 ioLoop 调用)
               │ channel_->remove()
               │ TcpConnection 析构（shared_ptr 引用为 0）
```

### 5.2 数据接收（handleRead）

```cpp
void TcpConnection::handleRead(Timestamp) {
    loop_->assertInLoopThread();
    int savedErrno = 0;
    ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);

    if (n > 0) {
        resumeReadWaiterIfNeeded();          // 唤醒 co_await asyncReadSome
        if (messageCallback_ && !hasReadWaiter) {
            messageCallback_(shared_from_this(), &inputBuffer_);
        }
    } else if (n == 0) {
        handleClose();                       // 对端关闭（read 返回 0）
    } else {
        handleError(savedErrno);             // 读错误 → 同样走 handleClose
    }
}
```

### 5.3 数据发送（send → sendInLoop）

```cpp
void TcpConnection::send(const void* data, size_t len) {
    if (state_ != kConnected) return;

    if (loop_->isInLoopThread()) {
        sendInLoop(data, len);    // 直接执行
    } else {
        // 跨线程：copy payload 后 runInLoop
        auto self = shared_from_this();
        std::string payload(data, len);
        loop_->runInLoop([self, payload = std::move(payload)]() mutable {
            self->sendInLoop(payload.data(), payload.size());
        });
    }
}
```

```
sendInLoop 的三段逻辑：
┌─────────────────────────────────────────────────────┐
│ 1. outputBuffer 空且未在写                           │
│    → 直接 ::write(fd, data, len)                    │
│    → 若写完：queueInLoop(writeCompleteCallback)      │
│    → 若未写完：转 2                                  │
├─────────────────────────────────────────────────────┤
│ 2. 还有剩余数据：append 到 outputBuffer               │
│    → channel_->enableWriting()（订阅 EPOLLOUT）      │
│    → 若超高水位：queueInLoop(highWaterMarkCallback)  │
├─────────────────────────────────────────────────────┤
│ 3. handleWrite 由 EPOLLOUT 触发                      │
│    → writeFd 写出 outputBuffer                      │
│    → 写完：disableWriting，唤醒写协程                 │
│    → 若 kDisconnecting：shutdownInLoop              │
└─────────────────────────────────────────────────────┘
```

### 5.4 关闭路径（统一收敛到 handleClose）

```
handleClose() 是唯一的关闭入口：
  - handleRead 中 read() == 0 → handleClose
  - handleError 中 → handleClose
  - forceCloseInLoop → handleClose
  - shutdownInLoop（写完后）→ socket_->shutdownWrite → 对端 EOF → handleClose

handleClose() 的幂等保护：
  if (state_ == kDisconnected) return;  // 已处理，直接返回
```

### 5.5 Coroutine Awaitable 工作机制

```
co_await conn->asyncReadSome(1024)：

1. await_ready()：检查 inputBuffer 是否已有 >= 1024 字节（loop 线程快路径）
2. await_suspend(handle)：
   → armReadWaiter(handle, 1024)
   → readWaiter_ = {handle, minBytes=1024, active=true}
   → 协程挂起，控制权返回 EventLoop
3. 下次 handleRead 到来：
   → n > 0 → resumeReadWaiterIfNeeded()
   → buffer 满足 minBytes → queueResume(handle)  // 通过 EventLoop 调度
   → 下一轮 loop 中 handle.resume()
4. await_resume()：
   → consumeReadableBytes(minBytes) → 返回 string
```

**关键设计**：`queueResume` 把协程的 resume 调度到 EventLoop 的下一轮，而不是在 `handleRead` 内部直接 resume，防止协程占用 I/O 事件分发的调用栈。

---

## 6. 协作关系

```
      TcpServer                EventLoopThreadPool
          │                          │
          │ 构造 TcpConnection         │ 分配 ioLoop
          │ connectEstablished        │
          │ removeConnection ◄── closeCallback
          │
      TcpConnection (ioLoop 线程)
          ├── Socket（持有 fd，RAII close）
          ├── Channel（epoll 事件代理）
          │        │ READ → handleRead
          │        │ WRITE → handleWrite
          │        │ CLOSE → handleClose
          │        │ ERROR → handleError
          └── Buffer×2（inputBuffer_, outputBuffer_）
```

| 关系 | 描述 |
|------|------|
| `TcpServer` → `TcpConnection` | `shared_ptr`，connections_ 映射持有 |
| `TcpConnection` → `Socket` | `unique_ptr`，唯一所有者，析构时 close fd |
| `TcpConnection` → `Channel` | `unique_ptr`，唯一所有者 |
| `TcpConnection` → `EventLoop` | 借用（原始指针），不拥有 |
| `Channel` ↔ `TcpConnection` | 通过 tie(shared_from_this()) 弱引用防悬空 |

---

## 7. 关键设计点

### 7.1 connectEstablished 中的 tie 机制

```cpp
void TcpConnection::connectEstablished() {
    channel_->tie(shared_from_this());   // ← 关键
    channel_->enableReading();
    // ...
}
```

Channel 的 `handleEvent` 会通过 `weak_ptr::lock()` 提升为 `shared_ptr`，
只要 guard 不为空，即使用户代码在回调中释放了外部的 `shared_ptr`，
TcpConnection 也不会在回调执行期间析构。

### 7.2 关闭路径的幂等性

```cpp
void TcpConnection::handleClose() {
    if (state_ == kDisconnected) return;  // 幂等保护
    setState(kDisconnected);
    // ...
}
```

多处路径（read=0、error、forceClose）都会调用 `handleClose`，幂等检查确保只执行一次。

### 7.3 跨线程 send 的所有权转移

```cpp
// 非 loop 线程 send：
std::string payload(data, len);
loop_->runInLoop([self, payload = std::move(payload)]() mutable {
    self->sendInLoop(payload.data(), payload.size());
});
```

`payload` 在 lambda capture 中 move，所有权转移到 EventLoop 的任务队列，
不存在并发访问（TcpConnection 的 buffer 只在 loop 线程操作）。

### 7.4 背压（Backpressure）

```
写缓冲积累路径：
outputBuffer 超过 backpressureHighWaterMark
    → channel_->disableReading()（暂停读入站数据）
    → reading_ = false

输出清空路径（handleWrite）：
outputBuffer 低于 backpressureLowWaterMark
    → channel_->enableReading()（恢复读）
    → reading_ = true
```

背压策略的核心：通过控制读事件订阅，间接产生 TCP 窗口 0 的背压效果。

### 7.5 状态机 + Channel 注册的一致性

| 状态 | Channel 订阅 |
|------|-------------|
| kConnecting | 无 |
| kConnected | EPOLLIN（写时加 EPOLLOUT） |
| kDisconnecting | EPOLLIN + EPOLLOUT（写完后取消） |
| kDisconnected | 全部取消（disableAll） |

---

## 8. 核心模块改动闸门（5 问）

1. **哪个 loop/线程拥有此模块？**
   每个 TcpConnection 归属唯一的 ioLoop（由 EventLoopThreadPool 分配）。所有 handle* 方法需在此 loop 线程执行。

2. **谁拥有它，谁释放它？**
   `TcpServer::connections_` 以 `shared_ptr` 持有，`removeConnectionInLoop` 从映射中移除即减少引用。用户持有的 `shared_ptr` 释放后，若 Channel 的 guard 不再存活，TcpConnection 随之析构。

3. **哪些回调可能重入？**
   `handleRead` 中的 `messageCallback_` 可能调用 `conn->send()`，进而调用 `sendInLoop`（同 loop 线程直接执行）。`closeCallback_` 由 TcpServer 接管，执行 `removeConnectionInLoop`，此时连接已处于 `kDisconnected`。

4. **哪些操作允许跨线程，如何 marshal？**
   `send()` / `shutdown()` / `forceClose()` 检测线程，非 loop 线程通过 `runInLoop` marshal。`startTls` / `connectEstablished` / `connectDestroyed` 必须在 loop 线程。

5. **哪个测试文件验证此改动？**
   `tests/contract/test_tcp_connection.cc`（read/write/close 主路径）、`tests/integration/test_tcp_server.cc`（端到端）

---

## 9. 从极简到完整的演进路径

```cpp
// 极简版本（无状态机、无跨线程、无背压）
class MinimalTcpConn {
    int fd_;
    std::function<void(std::string)> onMessage_;
public:
    void handleRead() {
        char buf[4096];
        ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n > 0) onMessage_(std::string(buf, n));
        else ::close(fd_);
    }
    void send(std::string_view data) {
        ::write(fd_, data.data(), data.size());
    }
};
```

从极简 → 完整，需要加：

1. **状态机（4 态）** → 防止在 disconnected 状态写数据
2. **Buffer** → 支持写一半暂存 + 消息边界控制
3. **Channel** → 受 epoll 驱动，不轮询
4. **RAII Socket** → 自动 close
5. **跨线程 send marshal** → thread-safe send
6. **handleClose 统一收敛** → 幂等关闭
7. **tie 机制** → 回调期间防析构
8. **背压策略** → 防止内存爆炸
9. **Coroutine awaitables** → 支持结构化并发写法

---

## 10. 易错点与最佳实践

### 易错点

| 错误 | 后果 |
|------|------|
| 在非 loop 线程直接操作 inputBuffer_ | 数据竞争 |
| forceClose 后仍调用 send | send 检查 `state_ != kConnected`，静默丢弃（有保护） |
| closeCallback 中持有 TcpConnection 的 shared_ptr | 延迟析构，但不崩溃（需注意资源释放时机） |
| 在 messageCallback 中同步调用 conn->shutdown() | 合法，shutdown 会在 handleWrite 清空后生效 |
| 未在 connectEstablished 前调用 startTls | TLS 状态未初始化，doTlsHandshake 不会被触发 |

### 最佳实践

```cpp
// ✓ 正确的连接建立与回调链路：
server.setConnectionCallback([](const TcpConnectionPtr& conn) {
    if (conn->connected()) {
        conn->setContext(std::make_shared<MySession>());
    } else {
        // conn->disconnected()，清理上下文
        auto session = std::any_cast<std::shared_ptr<MySession>>(conn->getContext());
        session->cleanup();
    }
});

// ✓ 协程写法（v1-coro-preview）：
mini::coroutine::Task echo(TcpConnectionPtr conn) {
    while (true) {
        auto data = co_await conn->asyncReadSome(1);
        if (data.empty()) break;
        co_await conn->asyncWrite(std::move(data));
    }
    co_await conn->waitClosed();
}
```

---

## 11. 面试角度总结

**Q1: TcpConnection 的状态机有哪 4 个状态？**
A: `kConnecting`（构造后，未调用 connectEstablished）、`kConnected`（正常工作）、`kDisconnecting`（调用 shutdown 等写完）、`kDisconnected`（handleClose 之后）。

**Q2: send 如何做到线程安全？**
A: send 检测当前线程，若在 loop 线程则直接 `sendInLoop`；否则 copy payload 到 lambda，通过 `runInLoop` 投递到 loop 线程执行。copy 保证 payload 的所有权安全转移。

**Q3: 为什么关闭路径要统一收敛到 handleClose？**
A: 防止重复关闭（double close fd）和遗漏 epoll 注销。`handleClose` 有幂等保护（`if state == kDisconnected return`），无论哪条路径触发，结果一致。

**Q4: connectEstablished 为什么调用 channel_->tie？**
A: 防止 handleEvent 回调链中 closeCallback 触发 TcpConnection 析构，导致后续回调访问悬空对象。tie 持有 weak_ptr，在 handleEvent 开始时提升为 shared_ptr 作为 guard。

**Q5: 背压如何实现？**
A: outputBuffer 超过 `backpressureHighWaterMark` 时 `disableReading`，暂停读入站数据，使 TCP 接收窗口变小，产生端到端的背压。outputBuffer 降到 `backpressureLowWaterMark` 时恢复读。

**Q6: coroutine awaitable 为什么不在 handleRead 内直接 resume？**
A: 防止协程占用 I/O 分发的调用栈（深度嵌套），改用 `queueResume` 把 resume 投递到 EventLoop 的下一轮，与正常的事件分发流程一致，避免 stack overflow 和复杂的重入问题。
