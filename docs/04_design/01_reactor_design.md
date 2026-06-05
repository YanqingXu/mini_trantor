# Reactor 设计分析

## 1. 设计意图

mini-trantor 的 Reactor 模式是整个网络库的核心调度引擎，实现了经典的 **one-loop-per-thread** 模型：
每个线程最多拥有一个 EventLoop，所有 I/O 事件和用户回调都在该线程顺序执行。

## 2. 核心组件

```
┌─────────────────────────────────────────┐
│                EventLoop                │
│                                         │
│  ┌──────────┐   ┌────────────────────┐  │
│  │ Poller   │   │ pendingFunctors_   │  │
│  │ (epoll)  │   │  (跨线程投递队列)  │  │
│  └────┬─────┘   └────────┬───────────┘  │
│       │ activeChannels         │         │
│       ▼                        ▼         │
│  ┌──────────┐   ┌────────────────────┐  │
│  │ Channel  │   │  wakeupFd_         │  │
│  │ (fd代理) │   │  (eventfd 唤醒)    │  │
│  └──────────┘   └────────────────────┘  │
└─────────────────────────────────────────┘
```

## 3. 核心循环（三阶段）

```cpp
void EventLoop::loop() {
    while (!quit_) {
        // Phase 1: epoll_wait 等待事件
        activeChannels_.clear();
        poller_->poll(kPollTimeMs, &activeChannels_);

        // Phase 2: 处理 I/O 事件
        for (auto* ch : activeChannels_) {
            ch->handleEvent(pollReturnTime);
        }

        // Phase 3: 执行 pendingFunctors
        doPendingFunctors();
    }
}
```

### 为什么是三阶段

1. **Phase 1（等待）**: epoll_wait 阻塞直到有事件或超时
2. **Phase 2（事件分发）**: 处理所有活跃的 I/O 事件
3. **Phase 3（回调执行）**: 处理跨线程投递的回调和延迟操作

这个顺序确保：
- I/O 事件优先处理
- 跨线程回调不会饿死（每轮循环都执行）
- quit 后还会处理一轮 pendingFunctors（drain）

## 4. 跨线程通信（wakeup + queueInLoop）

```
Thread A                          EventLoop Thread
   │                                    │
   │  queueInLoop(func)                 │
   │  ├─ lock mutex                     │
   │  ├─ push func to pendingFunctors_  │
   │  ├─ unlock mutex                   │
   │  └─ wakeup():                      │
   │     write(wakeupFd_, 1)  ─────────▶│
   │                                    │ epoll_wait 被唤醒
   │                                    │ doPendingFunctors():
   │                                    │   lock mutex
   │                                    │   swap functors ← pendingFunctors_
   │                                    │   unlock mutex
   │                                    │   for each f: f()
```

### swap 优化

```cpp
void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    {
        std::lock_guard lock(mutex_);
        functors.swap(pendingFunctors_);  // O(1) swap
    }
    for (auto& f : functors) { f(); }
}
```

* swap 到局部变量，最小化临界区
* 执行 functors 期间不持锁，允许并发 queueInLoop
* 如果 functors 中的回调又 queueInLoop → 不死锁（锁已释放）

## 5. Channel 事件代理

```
┌─────────────────────────────────────────────┐
│ Channel (fd 事件代理)                        │
│                                             │
│  events_ (关注的事件)  →  Poller 注册       │
│  revents_ (触发的事件) ←  Poller 返回       │
│                                             │
│  handleEventWithGuard():                    │
│  ┌───────────┬──────────────────────────┐   │
│  │ EPOLLHUP  │ closeCallback_           │   │
│  │ EPOLLERR  │ errorCallback_           │   │
│  │ EPOLLIN   │ readCallback_(timestamp) │   │
│  │ EPOLLOUT  │ writeCallback_           │   │
│  └───────────┴──────────────────────────┘   │
└─────────────────────────────────────────────┘
```

### tie() 机制

```cpp
void Channel::handleEvent(Timestamp t) {
    if (tied_) {
        auto guard = tie_.lock();   // weak_ptr → shared_ptr
        if (guard) {
            handleEventWithGuard(t);
        }
        // guard 为空说明对象已销毁，静默忽略
    } else {
        handleEventWithGuard(t);
    }
}
```

* TcpConnection 自身的 shared_ptr 被 tie 到 Channel
* 如果 TcpConnection 被销毁（shared_ptr 引用计数为 0），Channel 不会回调到悬空对象

## 6. 设计原则对照

| 原则 | 实现 |
|------|------|
| **单线程所有权** | EventLoop 的 threadId_ + assertInLoopThread() |
| **无锁 I/O** | 事件处理在 owner 线程，不需要锁 |
| **跨线程安全** | queueInLoop + mutex + eventfd wakeup |
| **RAII** | Channel remove-before-destroy, EventLoop 析构关闭 wakeupFd |
| **non-blocking** | 所有 fd 都设为 O_NONBLOCK |

## 7. 与经典 Reactor 的比较

| 特性 | mini-trantor | muduo | libevent |
|------|-------------|-------|----------|
| 多路复用 | epoll only | epoll/poll | epoll/kqueue/select |
| 线程模型 | one-loop-per-thread | 同 | 自由 |
| 跨线程唤醒 | eventfd | eventfd | socketpair |
| 定时器 | timerfd + map | timerfd + set | min-heap |
| 协程支持 | 内置 | 无 | 无 |

## 8. 扩展点

1. **添加新的 I/O 后端**: 实现 Poller 接口（如 kqueue for macOS）
2. **添加信号处理**: 通过 signalfd + Channel 接入 Reactor 循环
3. **添加 idle 通知**: 在 Phase 3 后检测是否有 activeChannels
