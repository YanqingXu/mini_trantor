# 连接关闭流程

## 三种关闭触发路径

### 路径 A：对端关闭（read 返回 0）

```
内核: 对端发送 FIN
  → epoll_wait 返回 EPOLLIN
  → TcpConnection::handleRead()
      → readFd() 返回 0
      → handleClose()
```

### 路径 B：主动优雅关闭（half-close）

```
用户调用: conn->shutdown()
  → setState(kDisconnecting)
  → runInLoop(shutdownInLoop)
      → socket_->shutdownWrite()   // 发送 FIN，但仍可接收
      → 等待对端关闭 → 最终走路径 A
```

### 路径 C：强制关闭

```
用户调用: conn->forceClose()
  → runInLoop(forceCloseInLoop)
      → handleClose()              // 直接关闭，不等待
```

## handleClose —— 关闭核心逻辑

```cpp
void TcpConnection::handleClose() {
    if (state_ == kDisconnected) return;  // 防止重入

    setState(kDisconnected);
    reading_ = false;
    channel_->disableAll();                // 取消所有 epoll 事件
    auto guardThis = shared_from_this();   // 防止 callback 中 this 被销毁

    resumeAllWaitersOnClose();             // 恢复所有挂起的协程
    connectionCallback_(guardThis);        // 通知用户：连接已断开
    closeCallback_(guardThis);             // 通知 TcpServer 移除连接
}
```

## TcpServer 移除连接

```
handleClose()
  → closeCallback_
  → TcpServer::removeConnection(conn)        // 可能在 worker loop 线程
      → loop_->runInLoop(removeConnectionInLoop)  // 投递到 base loop

removeConnectionInLoop():                    // base loop 线程
  → connections_.erase(conn->name())         // 从映射表移除
  → ioLoop->queueInLoop(conn->connectDestroyed())  // 投递到 worker loop

connectDestroyed():                          // worker loop 线程
  → setState(kDisconnected)
  → channel_->disableAll()
  → channel_->remove()                      // 从 Poller 中移除
  // TcpConnection shared_ptr 引用计数归零 → 析构
  // → Socket 析构 → close(fd)
```

## 跨线程时序

```
Worker Loop Thread              Base Loop Thread
    │                               │
    ├─ handleClose()                │
    │   ├─ disableAll()             │
    │   ├─ connectionCallback()     │
    │   └─ closeCallback()          │
    │       └─ removeConnection()   │
    │           └─ runInLoop() ────►│
    │                               ├─ removeConnectionInLoop()
    │                               │   ├─ connections_.erase()
    │                               │   └─ queueInLoop() ─────►│
    │                               │                           │
    ├─ connectDestroyed() ◄─────────┼───────────────────────────┘
    │   ├─ channel_->disableAll()   │
    │   └─ channel_->remove()       │
    │       └─ epoll_ctl(DEL)       │
    │                               │
    │   (shared_ptr 最后一个引用释放) │
    │   → ~TcpConnection()         │
    │       → ~Socket() → close(fd)│
```

## 生命周期保护

### lifetimeToken_ 机制

TcpServer 析构时 `lifetimeToken_.reset()`，之后所有 delayed 的 closeCallback 通过 `weak_ptr::lock()` 检测到 TcpServer 已销毁，安全跳过：

```cpp
connection->setCloseCallback([this, lifetime](const TcpConnectionPtr& conn) {
    if (!lifetime.lock()) return;  // TcpServer 已销毁
    removeConnection(conn);
});
```

### tie() 机制

Channel 的 `tie()` 持有 TcpConnection 的 `weak_ptr`，回调执行时 `lock()` 提升为 `shared_ptr`：

```cpp
void Channel::handleEvent(Timestamp t) {
    if (tied_) {
        auto guard = tie_.lock();  // 如果连接已销毁，guard 为空
        if (guard) {
            handleEventWithGuard(t);  // 安全执行
        }
    }
}
```

## 半关闭 (Half-Close) 语义

`shutdown()` 只关闭写端，不关闭读端：

```
conn->shutdown()
  → socket_->shutdownWrite()  // 发送 FIN
  → 对端收到 EOF
  → 对端可继续发送数据
  → 对端最终 close() → 本端收到 EOF → handleClose()
```

这允许优雅的四次挥手：先把待发送的数据发完，再等对端确认关闭。

如果 outputBuffer 中还有数据未发送完：

```cpp
void TcpConnection::shutdownInLoop() {
    if (!channel_->isWriting()) {  // outputBuffer 已清空
        socket_->shutdownWrite();
    }
    // 否则等 handleWrite() 清空 outputBuffer 后再 shutdown
}
```
