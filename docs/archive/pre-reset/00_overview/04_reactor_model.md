# Reactor 模型

## 什么是 Reactor

Reactor 是一种事件驱动的并发模型。核心思想：**不主动轮询，而是等待事件到来再处理**。

```
while (!quit) {
    events = poll(timeout);        // 1. 等待事件（阻塞）
    for (event in events) {
        dispatch(event);           // 2. 分发事件到注册的回调
    }
    runPendingTasks();             // 3. 执行队列中的任务
}
```

mini-trantor 实现的是 **单线程 Reactor** —— 一个 EventLoop 就是一个 Reactor 实例。通过 EventLoopThreadPool 横向扩展为 **多 Reactor 模型**。

## mini-trantor 的 Reactor 实现

### EventLoop::loop() —— Reactor 主循环

```cpp
void EventLoop::loop() {
    looping_ = true;
    quit_ = false;

    while (!quit_) {
        activeChannels_.clear();
        // 第一步：poll —— 等待 I/O 事件
        pollReturnTime_ = poller_->poll(10000, &activeChannels_);

        // 第二步：dispatch —— 分发事件到 Channel 回调
        eventHandling_ = true;
        for (Channel* channel : activeChannels_) {
            channel->handleEvent(pollReturnTime_);
        }
        eventHandling_ = false;

        // 第三步：run pending —— 执行跨线程投递的任务
        doPendingFunctors();
    }

    // 退出时清空剩余任务
    while (hasPending) { doPendingFunctors(); }
    looping_ = false;
}
```

### 三种事件源

EventLoop 通过平台 poller 驱动 socket 事件和 wakeup 事件，并通过 poll timeout 驱动 TimerQueue：

| 事件源 | 来源 | 触发场景 |
|---------|------|---------|
| **socket fd** | 网络连接（listen/accept/connect） | 数据可读、可写、连接关闭、错误 |
| **poll timeout** | `TimerQueue` | 定时器到期 |
| **platform wakeup** | Linux eventfd / Windows socket pair | 跨线程唤醒 |

socket 与 wakeup 在 Poller 层面统一为 Channel 事件；TimerQueue 通过每轮 poll timeout 进入同一个 EventLoop 调度序。

### 事件分发链

```
平台 poller                  Mini-Trantor
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
                         Poller::poll()
wait/select/epoll ─────►  │
   返回活跃 fd/socket     │ fillActiveChannels()
                         │   ├─ event.data.ptr → Channel*
                         │   └─ channel->setRevents(events)
                         ▼
                      EventLoop::loop()
                         │ for channel in activeChannels_:
                         │   channel->handleEvent()
                         ▼
                      Channel::handleEventWithGuard()
                         ├─ ReadEvent  → readCallback_(timestamp)
                         ├─ WriteEvent → writeCallback_()
                         ├─ CloseEvent → closeCallback_()
                         └─ ErrorEvent → errorCallback_()
                         ▼
                      具体处理函数
                         ├─ TcpConnection::handleRead()
                         ├─ TcpConnection::handleWrite()
                         ├─ TcpConnection::handleClose()
                         ├─ TimerQueue::handleRead()
                         └─ EventLoop::handleRead() (wakeup)
```

### Channel 的角色

Channel 是 **fd 与回调之间的桥梁**。每个注册到 EventLoop 的 fd 都有一个对应的 Channel 对象。

```
fd ──── Channel ──── EventLoop (Poller)
         │
         ├── readCallback_   (来自 TcpConnection::handleRead)
         ├── writeCallback_  (来自 TcpConnection::handleWrite)
         ├── closeCallback_  (来自 TcpConnection::handleClose)
         └── errorCallback_  (来自 TcpConnection::handleError)
```

Channel 的职责是：
1. 记住 fd 关心哪些事件（`events_`）
2. 把 Poller 返回的活跃事件（`revents_`）分发到对应回调
3. 通过 `tie()` 机制防止回调执行时对象被销毁

### Poller 的角色

Poller 是对 I/O 多路复用的抽象。Linux 使用 EPollPoller，Windows 使用 SelectPoller。

```cpp
// Poller 维护 fd/socket → Channel 的映射
channels_: { fd → Channel* }

// 状态机：kNew → kAdded ↔ kDeleted → kNew
// kNew:     Channel 未注册到 backend（初始状态）
// kAdded:   Channel 已注册到 backend
// kDeleted: Channel 已从 backend 删除，但仍在 channels_ 映射中
```

## 多 Reactor 模型

```
                    Main Reactor (base loop)
                    ┌─────────────────┐
                    │ Acceptor        │
              ┌─────┤ accept() ──────►├─────┐
              │     └─────────────────┘     │
              │                             │
    ┌─────────▼─────────┐    ┌──────────────▼────────────┐
    │ Sub-Reactor 1     │    │ Sub-Reactor 2              │
    │ (worker loop)     │    │ (worker loop)              │
    │ ┌───────────────┐ │    │ ┌────────────────────────┐ │
    │ │ poll + dispatch│ │    │ │ poll + dispatch        │ │
    │ │ Connection A   │ │    │ │ Connection C           │ │
    │ │ Connection B   │ │    │ │ Connection D           │ │
    │ └───────────────┘ │    │ └────────────────────────┘ │
    └───────────────────┘    └───────────────────────────┘
```

Main Reactor 只负责 accept，Sub-Reactor 负责已建立连接的 I/O。新连接按 round-robin 分配。

## 与传统多线程模型对比

| 特性 | Reactor 模型 | 传统 thread-per-connection |
|------|-------------|--------------------------|
| 线程数 | 固定（N+1 个 loop thread） | 随连接数增长 |
| 上下文切换 | 极少（同一线程内分发） | 频繁（每个连接一个线程） |
| 锁竞争 | 几乎无（单线程内无需锁） | 共享资源需要大量锁 |
| 编程模型 | 回调 / 协程 | 同步阻塞 |
| 适用场景 | 高并发短连接 | 少量慢连接 |
