# 线程模型

## 核心原则：One-Loop-Per-Thread

mini-trantor 的线程模型是 **one-loop-per-thread**：每个线程最多拥有一个 EventLoop，一个 EventLoop 只在一个线程中运行。

这不是建议，而是 **强制约束**：

```cpp
// EventLoop 构造时记录 owner thread
EventLoop::EventLoop()
    : threadId_(std::this_thread::get_id()) {
    if (t_loopInThisThread != nullptr) {
        throw std::runtime_error("another EventLoop already exists in this thread");
    }
    t_loopInThisThread = this;
}
```

## 线程角色

### Base Loop（主线程 / acceptor 线程）

- 运行 TcpServer 的 Acceptor，负责 `accept()` 新连接
- 维护 `connections_` 映射表
- 通过 `EventLoopThreadPool::getNextLoop()` 将新连接分配给 worker loop

### Worker Loop（I/O 工作线程）

- 每个 worker 线程运行一个独立的 EventLoop
- 负责已建立连接的读写和回调
- 一个 TcpConnection 绑定到一个 worker loop 后，生命周期内不迁移

### 单线程模式

当 `setThreadNum(0)` 时，base loop 同时扮演 acceptor 和 I/O 角色，所有连接都在主线程处理。

## 线程模型图

```
Main Thread (base loop)          Worker Thread 1          Worker Thread 2
┌─────────────────────┐    ┌──────────────────────┐    ┌──────────────────────┐
│    EventLoop        │    │    EventLoop          │    │    EventLoop          │
│  ┌───────────────┐  │    │  ┌────────────────┐  │    │  ┌────────────────┐  │
│  │ Acceptor      │  │    │  │ TcpConnection A│  │    │  │ TcpConnection C│  │
│  │ (listen fd)   │  │    │  │ TcpConnection B│  │    │  │ TcpConnection D│  │
│  └───────────────┘  │    │  └────────────────┘  │    │  └────────────────┘  │
│  ┌───────────────┐  │    │  ┌────────────────┐  │    │  ┌────────────────┐  │
│  │ connections_  │  │    │  │ TimerQueue     │  │    │  │ TimerQueue     │  │
│  │ (映射表)      │  │    │  └────────────────┘  │    │  └────────────────┘  │
│  └───────────────┘  │    └──────────────────────┘    └──────────────────────┘
└─────────────────────┘
         │ accept()       round-robin 分配
         ├─────────────────────►│
         ├──────────────────────┼──────────────►│
         :                      :               :
```

## 跨线程通信机制

### runInLoop / queueInLoop

跨线程操作的核心手段：

```cpp
void EventLoop::runInLoop(Functor cb) {
    if (isInLoopThread()) {
        cb();           // 同线程：直接执行
    } else {
        queueInLoop(std::move(cb));  // 异线程：放入队列
    }
}

void EventLoop::queueInLoop(Functor cb) {
    {
        std::lock_guard lock(mutex_);
        pendingFunctors_.push_back(std::move(cb));  // 唯一的锁
    }
    if (!isInLoopThread() || callingPendingFunctors_) {
        wakeup();  // 通过 eventfd 唤醒目标线程
    }
}
```

### wakeup 机制

每个 EventLoop 拥有一个 `eventfd`，当需要唤醒时：

```
Thread A                            Thread B (EventLoop)
    │                                   │
    ├── queueInLoop(cb) ──┐            │ (blocked in epoll_wait)
    │                     │            │
    │  pendingFunctors_.push(cb)       │
    │  write(wakeupFd_, 1) ──────────► │ epoll_wait returns
    │                                   │ handleRead() drains eventfd
    │                                   │ doPendingFunctors() → cb()
```

### 线程安全保证

| 操作 | 线程安全？ | 原因 |
|------|-----------|------|
| `EventLoop::runInLoop()` | ✅ | 跨线程路径通过 mutex + eventfd |
| `EventLoop::queueInLoop()` | ✅ | mutex 保护 pendingFunctors_ |
| `TcpConnection::send()` | ✅ | 内部自动 runInLoop 转发 |
| `TcpConnection::shutdown()` | ✅ | 内部自动 runInLoop 转发 |
| `TcpServer::start()` | ✅ | atomic compare_exchange |
| `Channel::enableReading()` | ❌ | 必须在 owner loop 线程 |
| `EventLoop::loop()` | ❌ | 必须在 owner 线程 |
| `TimerQueue::addTimer()` | ✅ | 内部自动 runInLoop 转发 |

## EventLoopThread 生命周期

```
创建 EventLoopThread
    │
    ├── startLoop()
    │   ├── 创建子线程
    │   ├── 子线程中构造 EventLoop
    │   ├── condition_variable 通知主线程 loop 已就绪
    │   ├── 返回 EventLoop* 给主线程
    │   └── 子线程进入 loop.loop()
    │
    ├── (使用中...)
    │
    └── ~EventLoopThread()
        ├── loop_->quit()
        └── thread_.join()
```

## EventLoopThreadPool 分配策略

```cpp
EventLoop* EventLoopThreadPool::getNextLoop() {
    EventLoop* loop = baseLoop_;       // 默认返回 base loop
    if (!loops_.empty()) {
        loop = loops_[next_];          // round-robin 选择
        ++next_;
        if (next_ >= loops_.size()) {
            next_ = 0;
        }
    }
    return loop;
}
```

这是简单的轮转策略——新连接依次分配给下一个 worker loop，确保负载大致均匀。
