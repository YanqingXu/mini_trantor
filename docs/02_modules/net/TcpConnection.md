# TcpConnection —— 工程级源码拆解

## 1. 类定位

* **角色**：整个网络库中**最复杂的类**，一个 TCP 连接的全部生命周期管理者
* **层级**：Net 层（面向用户的核心抽象）
* 统一管理连接状态、Socket、Channel、Buffer、回调、协程唤醒、TLS、背压

```
用户代码
   │
   ├── onConnection(conn)     → 连接/断开通知
   ├── onMessage(conn, buf)   → 收到数据
   ├── conn->send(data)       → 发送数据
   ├── co_await conn->asyncReadSome()  → 协程读
   └── co_await conn->asyncWrite(data) → 协程写
          │
          ▼
    ┌────────────────────────────────┐
    │        TcpConnection           │ ◄── 本文主角
    │  state_ / socket_ / channel_   │
    │  inputBuffer_ / outputBuffer_  │
    │  readWaiter_ / writeWaiter_    │
    │  ssl_ / tlsState_              │
    └────────────────────────────────┘
```

## 2. 解决的问题

**核心问题**：如何在单线程 Reactor 模型中安全地管理一个 TCP 连接的完整生命周期？

需要解决：
1. **状态管理**：连接从建立到关闭的四态切换
2. **读写缓冲**：非阻塞 I/O 下的数据缓冲和拼接
3. **跨线程发送**：用户可能在任意线程调用 `send()`
4. **生命周期安全**：回调和协程恢复时连接可能已被销毁
5. **协程桥接**：非侵入式地将 reactor 事件转为 co_await
6. **TLS 透明支持**：read/write 路径自动选择明文或 SSL

如果没有 TcpConnection：
- 用户需要自己管理 fd + channel + buffer + 状态
- 跨线程 send 需要自己实现 runInLoop
- 关闭和错误处理需要分散在各处

## 3. 对外接口（API）

### 用户调用

| 方法 | 用途 | 线程安全 |
|------|------|----------|
| `send(data)` | 发送数据 | 跨线程安全 |
| `shutdown()` | 优雅关闭（half-close） | 跨线程安全 |
| `forceClose()` | 强制关闭 | 跨线程安全 |
| `connected()` / `disconnected()` | 查询状态 | 任意线程 |
| `setTcpNoDelay(on)` | 设置 TCP_NODELAY | 任意线程 |
| `setContext(any)` / `getContext()` | 附加用户数据 | 仅 loop 线程 |
| `setBackpressurePolicy(hi, lo)` | 设置背压策略 | 跨线程安全 |

### 协程接口

| 方法 | 用途 |
|------|------|
| `asyncReadSome(minBytes)` | 协程读，返回 ReadAwaitable |
| `asyncWrite(data)` | 协程写，返回 WriteAwaitable |
| `waitClosed()` | 协程等待关闭，返回 CloseAwaitable |

### 回调设置（由 TcpServer/TcpClient 调用）

| 方法 | 回调时机 |
|------|----------|
| `setConnectionCallback` | 连接建立/断开 |
| `setMessageCallback` | 收到数据 |
| `setWriteCompleteCallback` | outputBuffer 清空 |
| `setHighWaterMarkCallback` | outputBuffer 超过阈值 |
| `setCloseCallback` | 连接关闭（内部用，TcpServer 设置） |

### 内部接口

| 方法 | 调用者 |
|------|--------|
| `connectEstablished()` | TcpServer/TcpClient |
| `connectDestroyed()` | TcpServer/TcpClient |
| `startTls(ctx, isServer)` | TcpServer/TcpClient |

## 4. 核心成员变量

```cpp
// ===== 基础 =====
EventLoop* loop_;                        // 绑定的 EventLoop（不拥有）
std::string name_;                       // 连接名称 "ServerName#1"
StateE state_;                           // kConnecting → kConnected → kDisconnecting → kDisconnected

// ===== I/O 三件套 =====
std::unique_ptr<Socket> socket_;         // 拥有 fd，析构时 close(fd)
std::unique_ptr<Channel> channel_;       // fd 的事件订阅和分发
Buffer inputBuffer_;                     // 接收缓冲区
Buffer outputBuffer_;                    // 发送缓冲区

// ===== 地址 =====
InetAddress localAddr_;                  // 本端地址
InetAddress peerAddr_;                   // 对端地址

// ===== 用户回调 =====
ConnectionCallback connectionCallback_;
MessageCallback messageCallback_;
HighWaterMarkCallback highWaterMarkCallback_;
WriteCompleteCallback writeCompleteCallback_;
CloseCallback closeCallback_;            // 内部回调，由 TcpServer 设置

// ===== 背压 =====
std::size_t highWaterMark_;              // 高水位回调阈值
std::size_t backpressureHighWaterMark_;  // 背压策略高水位
std::size_t backpressureLowWaterMark_;   // 背压策略低水位
bool reading_;                           // 当前是否在读（背压可能暂停读取）

// ===== 协程 =====
ReadAwaiterState readWaiter_;            // { handle, minBytes, active }
WriteAwaiterState writeWaiter_;          // { handle, active }
CloseAwaiterState closeWaiter_;          // { handle, active }

// ===== TLS =====
std::shared_ptr<TlsContext> tlsContext_; // TLS 上下文（多连接共享）
SSL* ssl_ = nullptr;                    // OpenSSL 连接对象（独占）
TlsState tlsState_ = kTlsNone;          // kTlsNone → kTlsHandshaking → kTlsEstablished

// ===== 扩展 =====
std::any context_;                       // 用户上下文（如 HttpContext）
```

## 5. 执行流程（最重要）

### 5.1 状态机

```
         connectEstablished()      shutdown()         handleClose()
              ┌──────┐           ┌──────────┐        ┌──────────┐
              │      ▼           │          ▼        │          ▼
kConnecting ──┴─► kConnected ───┴─► kDisconnecting ──┴─► kDisconnected
    │                                                       │
    │              forceClose() ─────────────────────────────┘
    │              handleError() ────────────────────────────┘
```

### 5.2 建立连接

```
TcpServer::newConnection(sockfd, peerAddr)
  → new TcpConnection(ioLoop, name, sockfd, localAddr, peerAddr)
  │   ├─ state_ = kConnecting
  │   ├─ socket_ = Socket(sockfd)
  │   ├─ channel_ = Channel(loop, sockfd)
  │   └─ channel_ 设置 read/write/close/error 回调
  │
  → ioLoop->runInLoop([conn] { conn->connectEstablished(); })

connectEstablished():     // 在 ioLoop 线程执行
  ├─ setState(kConnected)
  ├─ channel_->tie(shared_from_this())     // 生命周期保护
  ├─ channel_->enableReading()             // 开始监听 EPOLLIN
  ├─ applyBackpressurePolicy()
  └─ connectionCallback_(shared_from_this())  // 通知用户
```

### 5.3 接收数据

```
epoll_wait → EPOLLIN → Channel::handleEvent → TcpConnection::handleRead

handleRead():
  ├─ TLS 握手中? → doTlsHandshake(); return
  │
  ├─ 记录当前是否有 readWaiter_
  │
  ├─ ssl_ ? sslReadIntoBuffer() : inputBuffer_.readFd()
  │
  ├─ n > 0:
  │   ├─ resumeReadWaiterIfNeeded()        // 恢复挂起的协程
  │   └─ if (messageCallback_ && !hasReadWaiter):
  │       messageCallback_(this, &inputBuffer_)  // 通知用户
  │
  ├─ n == 0: handleClose()                 // 对端关闭
  │
  ├─ n == -2: /* SSL WANT_READ, ignore */
  │
  └─ n < 0: handleError(savedErrno)
```

**协程与回调的互斥**：如果有活跃的 readWaiter（协程在等待），
则不调用 messageCallback_，数据只给协程消费。

### 5.4 发送数据

```
conn->send(data)
  ├─ isInLoopThread()? → sendInLoop(data) 直接执行
  └─ else → runInLoop(sendInLoop(copy_of_data))  // 拷贝数据到 loop 线程

sendInLoop(data, len):
  ├─ state_ == kDisconnected? return
  │
  ├─ outputBuffer 为空且 Channel 未写? → 尝试直接 write
  │   ├─ 全部写完 → writeCompleteCallback + resumeWriteWaiter + return
  │   └─ 部分写入或 EAGAIN → 继续
  │
  ├─ faultError? → handleError; return
  │
  └─ 有剩余数据:
      ├─ outputBuffer_.append(data + nwrote, remaining)
      ├─ channel_->enableWriting()         // 订阅 EPOLLOUT
      ├─ 检查 highWaterMark 回调
      └─ applyBackpressurePolicy()
```

**直接写优化**：如果 outputBuffer 为空，先尝试直接 `write()`/`SSL_write()`。
大部分情况下数据能一次性写完，避免了不必要的 epoll_ctl 调用。

### 5.5 handleWrite

```
epoll_wait → EPOLLOUT → handleWrite()
  ├─ TLS 握手中? → doTlsHandshake(); return
  ├─ 未在写? → return
  │
  ├─ ssl_ ? sslWriteFromBuffer() : outputBuffer_.writeFd()
  │
  ├─ n > 0:
  │   ├─ retrieve 已写数据
  │   ├─ applyBackpressurePolicy()
  │   └─ outputBuffer 清空?
  │       ├─ channel_->disableWriting()    // 取消 EPOLLOUT
  │       ├─ writeCompleteCallback()
  │       ├─ resumeWriteWaiterIfNeeded()   // 恢复写协程
  │       └─ state_ == kDisconnecting? → shutdownInLoop()
  │
  └─ n <= 0 && 不是 EAGAIN: handleError()
```

### 5.6 关闭连接

```
handleClose():
  ├─ state_ == kDisconnected? return (防重入)
  ├─ setState(kDisconnected)
  ├─ reading_ = false
  ├─ channel_->disableAll()
  ├─ guard = shared_from_this()           // 防止回调中析构
  ├─ resumeAllWaitersOnClose()            // 恢复所有协程
  ├─ connectionCallback_(guard)           // 通知用户断开
  └─ closeCallback_(guard)                // 通知 TcpServer 移除

connectDestroyed():        // TcpServer 投递到 ioLoop
  ├─ if (state_ == kConnected):
  │   ├─ setState(kDisconnected)
  │   ├─ channel_->disableAll()
  │   └─ connectionCallback_()
  └─ channel_->remove()                   // 从 Poller 移除
  // shared_ptr 引用计数归零 → ~TcpConnection → ~Socket → close(fd)
```

### 5.7 协程桥接

```
co_await conn->asyncReadSome(minBytes)
  → ReadAwaitable::await_ready():
      → canReadImmediately(minBytes)? → true (不挂起)
  → ReadAwaitable::await_suspend(handle):
      → armReadWaiter(handle, minBytes)   // 在 loop 线程执行
          → if canReadImmediately → queueResume(handle)
          → else readWaiter_ = {handle, minBytes, true}
  → 数据到达 → handleRead → resumeReadWaiterIfNeeded()
      → if readWaiter_.active && inputBuffer >= minBytes:
          → queueResume(handle)           // 通过 queueInLoop 恢复
  → ReadAwaitable::await_resume():
      → return consumeReadableBytes()     // 消费 inputBuffer
```

**单等待者约束**：每个连接同时只能有一个 read waiter、一个 write waiter、一个 close waiter。
违反会抛 `logic_error`。

## 6. 关键交互关系

```
TcpServer                       TcpClient
    │                               │
    ├─ 创建 TcpConnection ──────────┤
    ├─ setCloseCallback ────────────┤
    │                               │
    ▼                               ▼
 TcpConnection
    │
    ├─ owns Socket (fd)
    ├─ owns Channel (events)
    ├─ owns Buffer ×2 (input/output)
    ├─ uses EventLoop (runInLoop)
    ├─ uses TlsContext (optional, shared)
    │
    └─ callbacks → User Code
         ├─ connectionCallback
         ├─ messageCallback
         └─ writeCompleteCallback
```

## 7. 关键设计点

### 7.1 enable_shared_from_this

TcpConnection 必须通过 `shared_ptr` 管理，因为：
1. `channel_->tie()` 需要 `shared_from_this()`
2. 跨线程 `send()` 需要持有 `shared_ptr` 防止生命周期问题
3. `closeCallback` 需要 `shared_ptr` 让 TcpServer 安全移除

### 7.2 直接写优化

```cpp
// sendInLoop 中：
if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
    nwrote = ::write(fd, data, len);  // 尝试直接写
    if (remaining == 0) return;       // 全部写完，不注册 EPOLLOUT
}
```

避免了大部分场景下的 `epoll_ctl(MOD)` 调用，减少系统调用。

### 7.3 协程与回调共存

```cpp
if (n > 0) {
    resumeReadWaiterIfNeeded();                   // 先通知协程
    if (messageCallback_ && !hasReadWaiter) {     // 没有协程才走回调
        messageCallback_(shared_from_this(), &inputBuffer_);
    }
}
```

同一个连接可以在不同阶段使用不同模式。

### 7.4 TLS 透明支持

```cpp
if (ssl_) {
    n = sslReadIntoBuffer(&savedErrno);
} else {
    n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
}
```

对用户完全透明，handleRead/handleWrite 自动选择 SSL 路径。

### 7.5 背压策略

```cpp
void applyBackpressurePolicy() {
    if (reading_ && buffered >= backpressureHighWaterMark_) {
        reading_ = false;
        channel_->disableReading();   // 暂停读取，让对端 TCP 窗口缩小
    }
    if (!reading_ && buffered <= backpressureLowWaterMark_) {
        reading_ = true;
        channel_->enableReading();    // 恢复读取
    }
}
```

## 8. 潜在问题

### 8.1 协程异常安全

如果 `armReadWaiter` 中 `queueInLoop` 之后协程帧被外部销毁，
`handle.resume()` 会访问悬空帧。

**缓解**：协程帧由 `Task<T>` 管理，正常使用不会手动销毁。

### 8.2 send 的拷贝开销

跨线程 `send()` 需要将 data 拷贝到 `std::string` 再 move 到 lambda：
```cpp
std::string payload(static_cast<const char*>(data), len);
loop_->runInLoop([self, payload = std::move(payload)] { ... });
```

大数据量时可能有性能影响。

### 8.3 TLS 重协商

当前不处理 TLS 重协商（renegotiation）。如果对端发起重协商，可能导致意外行为。
现代 TLS 1.3 已废弃重协商。

## 9. 极简实现

```cpp
class MinimalTcpConnection {
public:
    MinimalTcpConnection(int fd)
        : fd_(fd), state_(kConnected) {}

    void handleRead() {
        char buf[4096];
        ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n > 0) {
            onMessage_(std::string(buf, n));
        } else if (n == 0) {
            state_ = kDisconnected;
            ::close(fd_);
        }
    }

    void send(const std::string& data) {
        ::write(fd_, data.data(), data.size());
    }

private:
    int fd_;
    int state_;
    std::function<void(std::string)> onMessage_;
};
```

完整版在此基础上加：
1. **Buffer** → 非阻塞 I/O 的数据缓冲
2. **Channel** → 事件订阅
3. **状态机** → 优雅关闭、半关闭
4. **shared_ptr + tie** → 生命周期安全
5. **runInLoop** → 跨线程安全
6. **协程 awaitable** → co_await 支持
7. **TLS** → SSL_read/SSL_write 路径
8. **背压** → disableReading/enableReading

## 10. 面试角度总结

**Q1: TcpConnection 的四个状态是什么？转换条件？**
A: kConnecting(初始) → kConnected(connectEstablished) → kDisconnecting(shutdown) → kDisconnected(handleClose/forceClose/connectDestroyed)

**Q2: send() 如何保证线程安全？**
A: 不加锁。如果是 loop 线程直接 sendInLoop；否则拷贝数据到 lambda 通过 runInLoop 投递。

**Q3: 为什么 send 有"直接写优化"？**
A: 如果 outputBuffer 为空且未注册 EPOLLOUT，直接调 write()。大多数情况数据能一次写完，省去 epoll_ctl 系统调用。

**Q4: 协程和回调如何共存？**
A: 有 readWaiter 活跃时，数据给协程（不调 messageCallback）；无 readWaiter 时，走传统回调路径。

**Q5: TcpConnection 为什么必须用 shared_ptr？**
A: 因为 channel 的 tie() 需要 weak_ptr（来自 shared_from_this）；跨线程 send 需要持有引用；closeCallback 需要安全移除。

**Q6: 连接关闭的三种路径？**
A: (1) 对端关闭：read 返回 0 → handleClose，(2) 主动 shutdown：half-close → 等对端，(3) forceClose：直接 handleClose。

**Q7: 背压策略如何工作？**
A: outputBuffer 超过 highWaterMark 时 disableReading（暂停读取，TCP 窗口缩小）；低于 lowWaterMark 时 enableReading 恢复。
