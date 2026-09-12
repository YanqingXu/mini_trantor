# 连接生命周期设计分析

## 1. 设计意图

TcpConnection 是 mini-trantor 中生命周期最复杂的对象。它需要在 **异步回调、跨线程操作、协程挂起** 等场景下保证安全的创建、使用和销毁。

## 2. 状态机

```
                connectEstablished()
  [创建] ─────────────────────────────▶ kConnected
                                            │
                                            │ handleClose()
                                            │ 或 shutdown()
                                            ▼
                                       kDisconnecting
                                            │
                                            │ connectDestroyed()
                                            ▼
                                       kDisconnected
```

### 状态转换规则

| 当前状态 | 事件 | 目标状态 | 动作 |
|----------|------|----------|------|
| kConnecting | connectEstablished() | kConnected | enableReading, 调用 connectionCallback |
| kConnected | handleClose() | kDisconnecting | disableAll, 调用 closeCallback |
| kConnected | shutdown() | kDisconnecting | shutdownWrite |
| kDisconnecting | connectDestroyed() | kDisconnected | remove channel, 调用 connectionCallback |

## 3. 所有权模型

```
          TcpServer / TcpClient
          (ConnectionMap)
                │
                │ shared_ptr<TcpConnection>
                ▼
        ┌─────────────────┐
        │ TcpConnection   │
        │                 │
        │  channel_       │──── unique_ptr<Channel>
        │  socket_        │──── unique_ptr<Socket>
        │  inputBuffer_   │──── 值成员
        │  outputBuffer_  │──── 值成员
        └────────┬────────┘
                 │
                 │ tie(shared_from_this())
                 ▼
           Channel::tie_  ──── weak_ptr<TcpConnection>
```

### 关键所有权规则

1. **TcpServer 持有 shared_ptr**: ConnectionMap 中存储
2. **Channel 持有 weak_ptr**: 通过 tie() 关联
3. **闭包可能持有 shared_ptr**: closeCallback 等回调捕获
4. **用户可能持有 shared_ptr**: 如果保存了 TcpConnectionPtr

## 4. 安全销毁链

```
对端关闭连接 (read 返回 0)
  │
  ├─ handleClose():
  │   → state_ = kDisconnecting
  │   → channel_->disableAll()           // 不再监听事件
  │   → closeCallback_(shared_from_this())
  │     └─ TcpServer::removeConnection(conn)
  │        └─ loop->queueInLoop(connectDestroyed)  // 异步销毁
  │
  ├─ TcpServer::removeConnection():
  │   → connections_.erase(conn->name())  // 从 map 移除
  │   → conn->getLoop()->runInLoop(       // 投递到 conn 的 loop
  │       [conn] { conn->connectDestroyed(); })
  │   // conn 被 lambda 捕获，延长生命
  │
  └─ connectDestroyed():
      → state_ = kDisconnected
      → channel_->remove()               // 从 Poller 移除
      → connectionCallback_(shared_from_this())
      // lambda 结束后 shared_ptr 释放
      // 如果这是最后一个 shared_ptr → 析构
```

### 为什么需要 queueInLoop？

```
场景: TcpServer 在 base loop，TcpConnection 在 worker loop

handleClose() 在 worker loop 执行
  → closeCallback → TcpServer::removeConnection 在 base loop
    → 但 connectDestroyed 必须在 worker loop 执行（因为要操作 channel）
    → 所以 runInLoop 投递回 worker loop
```

## 5. tie() 与 lifetimeToken_ 

### tie() 解决的问题

```
时序问题:
  1. handleClose → closeCallback → removeConnection → erase from map
  2. 如果 map 是最后持有者 → conn 析构
  3. 但 handleClose 返回后，Channel 还在处理事件（handleEvent 还在栈上）
  4. conn 被析构 → Channel 被析构 → handleEvent 访问已销毁对象 → UB
```

**tie() 解决方案**:
```cpp
void Channel::handleEvent(Timestamp t) {
    auto guard = tie_.lock();  // 临时 shared_ptr，防止 conn 被析构
    if (guard) {
        handleEventWithGuard(t);  // 执行期间 conn 不会被析构
    }
    // guard 析构后，如果 conn 没有其他持有者 → 此时安全析构
}
```

### lifetimeToken_ 解决的问题

```
时序问题（跨线程）:
  1. TcpServer::~TcpServer() 在 base loop 析构
  2. worker loop 上的 connectDestroyed 还在 pendingFunctors 中
  3. 如果 TcpServer 先析构 → 回调引用已销毁的 TcpServer → UB
```

**lifetimeToken_ 解决方案**:
```cpp
// TcpServer 持有 lifetimeToken_ = make_shared<bool>(true)
// removeConnection 回调中：
auto token = lifetimeToken_;  // 拷贝 weak_ptr
loop->runInLoop([conn, token] {
    if (auto alive = token.lock()) {
        conn->connectDestroyed();  // TcpServer 还活着
    }
    // 否则 TcpServer 已销毁，跳过
});
```

## 6. forceClose 与 shutdown 的区别

### shutdown（优雅关闭）

```
shutdown() → shutdownInLoop():
  → socket_->shutdownWrite()    // 关闭写端
  → 对端收到 FIN → 对端 close → 我方收到 read=0
  → handleClose() 正常流程
```

### forceClose（强制关闭）

```
forceClose() → queueInLoop(forceCloseInLoop):
  → if (state_ == kConnected || state_ == kDisconnecting)
    → handleClose()             // 直接走关闭流程
```

* forceClose 通过 queueInLoop 投递，确保在 loop 线程执行
* 适用于超时关闭、错误关闭等场景

## 7. 协程 awaiter 与生命周期

```
协程 awaiter 持有 TcpConnectionPtr (shared_ptr):
  → co_await conn->asyncReadSome()
  → ReadAwaitable 构造时拷贝 shared_ptr
  → 协程挂起期间 shared_ptr 保持连接存活

连接关闭时:
  → handleClose() → resumeAllWaitersOnClose()
  → 恢复所有挂起的协程
  → 协程体检查返回值（空字符串 = 连接关闭）
  → 协程结束 → Awaitable 析构 → shared_ptr 释放
```

## 8. 生命周期陷阱

| 陷阱 | 症状 | 防护 |
|------|------|------|
| Channel 回调触发时 conn 已析构 | 段错误 / UB | tie() weak_ptr guard |
| TcpServer 析构时 worker loop 有 pending 回调 | 回调引用已析构 TcpServer | lifetimeToken_ |
| closeCallback 中再 send | 在 kDisconnecting 状态 send | state 检查 |
| 用户长期持有 shared_ptr | 连接无法释放 | 文档说明 |
| 协程挂起时连接关闭 | 协程永远不恢复 | resumeAllWaitersOnClose |
