# EventLoop —— 工程级源码拆解

## 1. 类定位

* **角色**：整个 Reactor 框架的核心调度器，唯一的"事件循环执行者"
* **层级**：Reactor 层（最底层核心）
* 所有 I/O 事件分发、跨线程回调投递、定时器触发都经过 EventLoop

```
                    用户代码
                       │
              ┌────────┴────────┐
              │   TcpServer /   │
              │   TcpClient     │
              └────────┬────────┘
                       │
         ┌─────────────┼─────────────┐
         │         EventLoop         │  ◄── 本文主角
         │  (poll + dispatch + functor) │
         └─────────────┬─────────────┘
              ┌────────┼────────┐
              │    Poller       │
              │   TimerQueue    │
              │   Channel(s)    │
              └─────────────────┘
```

## 2. 解决的问题

**核心问题**：如何在单线程内高效地多路复用 I/O 事件 + 定时器 + 跨线程任务？

如果没有 EventLoop：
- 每个 fd 需要一个独立线程 → 高并发时线程爆炸
- 定时器需要独立线程 + 条件变量 → 与 I/O 事件不协调
- 跨线程任务投递需要自建消息队列 → 每个组件各搞一套
- 没有统一的"在哪个线程执行"的保证 → 数据竞争满天飞

EventLoop 把三件事统一到一个循环中：
1. **I/O 事件轮询**：通过 Poller（epoll）
2. **定时器触发**：通过 TimerQueue（timerfd）
3. **跨线程任务投递**：通过 pendingFunctors_ + eventfd wakeup

## 3. 对外接口（API）

### 用户直接调用

| 方法 | 用途 | 线程安全 |
|------|------|----------|
| `loop()` | 启动事件循环主体 | 仅 owner 线程 |
| `quit()` | 请求退出循环 | 跨线程安全 |
| `runInLoop(cb)` | 在 loop 线程执行回调 | 跨线程安全 |
| `queueInLoop(cb)` | 投递回调到下一轮执行 | 跨线程安全 |
| `runAt(time, cb)` | 定时执行（绝对时间） | 跨线程安全 |
| `runAfter(delay, cb)` | 延迟执行 | 跨线程安全 |
| `runEvery(interval, cb)` | 定期执行 | 跨线程安全 |
| `cancel(timerId)` | 取消定时器 | 跨线程安全 |
| `isInLoopThread()` | 判断当前是否在 loop 线程 | 任意线程 |

### 内部调用（由 Channel/Poller 等使用）

| 方法 | 调用者 | 用途 |
|------|--------|------|
| `updateChannel(ch)` | Channel | 注册/更新 epoll 监听 |
| `removeChannel(ch)` | Channel | 移除 epoll 监听 |
| `hasChannel(ch)` | Channel | 检查是否已注册 |
| `wakeup()` | queueInLoop/quit | 通过 eventfd 唤醒阻塞的 poll |

## 4. 核心成员变量

```cpp
// ===== 循环控制 =====
bool looping_;                          // 是否正在循环中（loop() 进入时 true）
std::atomic<bool> quit_;                // 退出标志（唯一的 atomic 变量，支持跨线程 quit）
bool eventHandling_;                    // 正在分发事件中（防止分发期间析构）
bool callingPendingFunctors_;           // 正在执行 pending 回调（影响 wakeup 决策）

// ===== 线程亲和性 =====
const std::thread::id threadId_;        // 构造时记录，永不改变

// ===== I/O 多路复用 =====
std::unique_ptr<Poller> poller_;        // EPollPoller 实例（独占所有权）
ChannelList activeChannels_;            // poll 返回的活跃 Channel 列表
Channel* currentActiveChannel_;         // 当前正在处理的 Channel（调试用）

// ===== 定时器 =====
std::unique_ptr<TimerQueue> timerQueue_;// 基于 timerfd 的定时器队列（独占所有权）

// ===== 跨线程 wakeup =====
int wakeupFd_;                          // eventfd 文件描述符
std::unique_ptr<Channel> wakeupChannel_;// 监听 eventfd 的 Channel

// ===== 跨线程任务队列 =====
mutable std::mutex mutex_;              // 保护 pendingFunctors_ 的互斥锁
std::vector<Functor> pendingFunctors_;  // 待执行的跨线程回调

// ===== 时间戳 =====
Timestamp pollReturnTime_;              // 最近一次 poll 返回的时间
```

### 生命周期与线程安全

| 变量 | 生命周期 | 线程安全 |
|------|----------|----------|
| `quit_` | 构造 → 析构 | `std::atomic` |
| `threadId_` | 构造时确定 | `const`，天然安全 |
| `pendingFunctors_` | 构造 → 析构 | `mutex_` 保护 |
| `wakeupFd_` | 构造 → 析构 | 仅 loop 线程 close |
| 其余所有变量 | 构造 → 析构 | 仅 loop 线程访问 |

## 5. 执行流程（最重要）

### 5.1 构造

```
EventLoop::EventLoop()
  ├─ 检查 thread_local t_loopInThisThread == nullptr（一个线程只能有一个 loop）
  ├─ t_loopInThisThread = this
  ├─ poller_ = Poller::newDefaultPoller(this)     // 创建 EPollPoller
  ├─ timerQueue_ = make_unique<TimerQueue>(this)  // 创建 TimerQueue（内部创建 timerfd）
  ├─ wakeupFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)
  ├─ wakeupChannel_ = make_unique<Channel>(this, wakeupFd_)
  ├─ wakeupChannel_->setReadCallback(handleRead)
  └─ wakeupChannel_->enableReading()              // 注册到 epoll
```

### 5.2 loop() 主循环

```cpp
void EventLoop::loop() {
    looping_ = true;
    quit_ = false;

    while (!quit_) {
        // 阶段 1: 等待事件（最多 10 秒超时）
        activeChannels_.clear();
        pollReturnTime_ = poller_->poll(10000, &activeChannels_);

        // 阶段 2: 分发活跃 Channel 的事件
        eventHandling_ = true;
        for (Channel* channel : activeChannels_) {
            currentActiveChannel_ = channel;
            channel->handleEvent(pollReturnTime_);
        }
        eventHandling_ = false;

        // 阶段 3: 执行跨线程投递的回调
        doPendingFunctors();
    }

    // 退出后: 排干待执行回调（重要！防止漏掉 connectDestroyed 等关键回调）
    while (hasPending) {
        doPendingFunctors();
    }

    looping_ = false;
}
```

**一次 loop 迭代的时间线**：

```
┌───────────────────── 一次迭代 ─────────────────────┐
│                                                     │
│  epoll_wait()      handleEvent()    doPendingFunctors() │
│  ┌─────────┐      ┌───────────┐    ┌──────────────┐│
│  │ 阻塞等待 │ ──► │ 事件分发   │ ──► │ 跨线程回调   ││
│  │ I/O事件  │      │ read/write │    │ runInLoop    ││
│  │ (≤10s)  │      │ close/error│    │ 投递的任务   ││
│  └─────────┘      └───────────┘    └──────────────┘│
│                                                     │
└─────────────────────────────────────────────────────┘
```

### 5.3 runInLoop / queueInLoop —— 跨线程回调

```
runInLoop(cb):
  ├─ isInLoopThread()?
  │   ├─ YES → cb()                        // 直接在当前线程执行
  │   └─ NO  → queueInLoop(cb)             // 投递到队列

queueInLoop(cb):
  ├─ lock(mutex_)
  ├─ pendingFunctors_.push_back(cb)
  ├─ unlock
  └─ if (!isInLoopThread() || callingPendingFunctors_):
      └─ wakeup()                          // 写 eventfd 唤醒 poll
```

**为什么 `callingPendingFunctors_` 时也要 wakeup？**

因为 `doPendingFunctors()` 是 swap 了整个 vector 后在新 vector 上遍历，
如果在 `doPendingFunctors()` 执行过程中又有人投递了新回调（到已被 swap 了的空 vector 里），
这些新回调需要唤醒下一个 poll 来执行，否则会被"饿死"直到下一个 I/O 事件到来。

### 5.4 doPendingFunctors —— swap 技巧

```cpp
void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;

    {
        std::lock_guard lock(mutex_);
        functors.swap(pendingFunctors_);    // swap 而不是 copy
    }
    // 此时 mutex_ 已释放！执行回调时不持锁 → 避免死锁

    for (auto& functor : functors) {
        functor();                          // 可能再次 queueInLoop
    }

    callingPendingFunctors_ = false;
}
```

swap 技巧的好处：
1. **缩小临界区**：锁只保护 swap，执行回调时不持锁
2. **避免死锁**：回调内部可以再次调用 queueInLoop（会再次加锁）
3. **公平性**：新投递的回调不会无限延长当前批次

### 5.5 quit 退出

```
quit():
  ├─ quit_ = true (atomic store)
  └─ if (!isInLoopThread()):
      └─ wakeup()         // 唤醒可能正在 epoll_wait 的 loop 线程

loop() while 循环检测到 quit_ == true:
  └─ 退出 while 循环
  └─ 排干 pendingFunctors_    // 确保 connectDestroyed 等不被遗漏
  └─ looping_ = false
```

### 5.6 析构

```
~EventLoop():
  ├─ assert(isInLoopThread())           // 必须在 owner 线程析构
  ├─ assert(!looping_)                  // 必须先退出 loop()
  ├─ wakeupChannel_->disableAll()
  ├─ wakeupChannel_->remove()
  ├─ close(wakeupFd_)
  └─ t_loopInThisThread = nullptr
```

## 6. 关键交互关系

```
                    EventLoopThread
                    (创建 EventLoop)
                         │
                         ▼
    ┌─────────────── EventLoop ───────────────┐
    │                                          │
    │   owns          owns         owns        │
    │    │             │            │           │
    │    ▼             ▼            ▼           │
    │  Poller      TimerQueue   wakeupChannel  │
    │  (epoll)     (timerfd)    (eventfd)      │
    │                                          │
    └────────────────┬─────────────────────────┘
                     │
              manages (not owns)
                     │
            ┌────────┼────────┐
            ▼        ▼        ▼
         Channel  Channel  Channel
         (conn1)  (conn2)  (timer)
```

### 依赖关系

| 类 | 关系 |
|----|------|
| **Poller** | EventLoop 独占所有权，调用 poll/updateChannel/removeChannel |
| **TimerQueue** | EventLoop 独占所有权，调用 addTimer/cancel |
| **Channel** | EventLoop 管理其生命周期注册，Channel::update() 回调到 EventLoop |
| **TcpConnection** | 绑定到某个 EventLoop，所有操作通过 runInLoop 保证线程亲和 |
| **TcpServer** | 持有 base loop + worker loops，通过 runInLoop 回流 |
| **EventLoopThread** | 创建并拥有 EventLoop 实例 |

## 7. 关键设计点

### 7.1 one-loop-per-thread

```cpp
thread_local EventLoop* t_loopInThisThread = nullptr;
// 构造时检查：一个线程只允许一个 EventLoop
if (t_loopInThisThread != nullptr) {
    throw std::runtime_error("another EventLoop already exists in this thread");
}
```

**为什么？**—— 如果一个线程有两个 loop，它们共享 thread_local 状态，
`isInLoopThread()` 判断会混乱，线程亲和性保证会崩溃。

### 7.2 eventfd 唤醒

**为什么不用 pipe？**

| 特性 | eventfd | pipe |
|------|---------|------|
| fd 数量 | 1 个 | 2 个（读 + 写） |
| 内核开销 | 更轻量 | 内核缓冲区 |
| 语义 | 计数器（可累计） | 字节流 |

eventfd 更现代、更轻量，Linux 2.6.22+ 可用。

### 7.3 quit 后排干 pendingFunctors

这是防止退出时丢失关键回调的设计：
```cpp
// quit 后的排干循环
while (true) {
    bool hasPending = false;
    {
        std::lock_guard lock(mutex_);
        hasPending = !pendingFunctors_.empty();
    }
    if (!hasPending) break;
    doPendingFunctors();
}
```

典型场景：TcpServer 析构时，worker loop 需要执行 `connectDestroyed()`。

### 7.4 RAII 与 abort 式防御

析构时用 `abort()` 而不是异常，因为：
- 析构函数中抛异常可能导致 `std::terminate`
- "在错误线程析构"或"循环中析构"是不可恢复的编程错误

## 8. 潜在问题

### 8.1 quit 后排干可能无限循环？

如果回调 A 在 `doPendingFunctors` 中执行时又 queueInLoop 了 B，
B 又 queueInLoop 了 C... 理论上排干循环永远不会退出。

**实际风险低**：正常使用中，quit 后不会有持续的新回调产生。

### 8.2 pendingFunctors 中回调抛异常

如果某个 functor 抛出异常，`callingPendingFunctors_` 不会被置回 `false`。
后续的 `queueInLoop` 会一直 wakeup（虽然不影响正确性，只是多余的 wakeup）。

### 8.3 无 functor 执行顺序保证的注意点

`doPendingFunctors` 用 swap 取出后顺序执行，同一批次内有序；
但如果 functor 内再次 queueInLoop，新 functor 在下一批次执行。

## 9. 极简实现

如果只实现最小功能的 EventLoop（不含定时器、不含 wakeup）：

```cpp
class MinimalEventLoop {
public:
    MinimalEventLoop() : quit_(false), epollFd_(epoll_create1(0)) {}

    void loop() {
        struct epoll_event events[64];
        while (!quit_) {
            int n = epoll_wait(epollFd_, events, 64, -1);
            for (int i = 0; i < n; ++i) {
                auto* ch = static_cast<Channel*>(events[i].data.ptr);
                ch->handleEvent();
            }
        }
    }

    void quit() { quit_ = true; }

private:
    bool quit_;
    int epollFd_;
};
```

从极简版到完整版需要加：
1. **eventfd + wakeupChannel** → 跨线程唤醒
2. **mutex + pendingFunctors** → 跨线程回调
3. **TimerQueue** → 定时器
4. **thread_local 检查** → 线程亲和性
5. **quit 后排干** → 安全退出

## 10. 面试角度总结

### 高频面试问题

**Q1: EventLoop 的 loop() 循环做了哪三件事？**
A: poll 等待 I/O → 分发活跃 Channel → 执行 pendingFunctors

**Q2: runInLoop 和 queueInLoop 的区别？**
A: `runInLoop` 在 loop 线程直接执行，否则转 `queueInLoop`。`queueInLoop` 总是放入队列，在 `doPendingFunctors` 中执行。

**Q3: 为什么 doPendingFunctors 用 swap？**
A: 缩小临界区、避免死锁（回调中可再次 queueInLoop）、保证公平性（不无限延长当前批次）。

**Q4: 为什么 callingPendingFunctors_ 时也需要 wakeup？**
A: swap 后新投递的回调在空 vector 中，如果不 wakeup，要等下一个 I/O 事件才能执行，可能长时间延迟。

**Q5: EventLoop 如何保证线程安全？**
A: "把操作投递到正确线程"而不是"加锁"。只有 pendingFunctors_ 需要 mutex，其余状态由线程亲和性保证。

**Q6: quit() 后为什么还要排干 pendingFunctors？**
A: 确保 `connectDestroyed()` 等关键清理回调不被遗漏，避免资源泄漏。

**Q7: 为什么用 eventfd 而不是 pipe？**
A: eventfd 只需 1 个 fd（pipe 需要 2 个），更轻量，语义更清晰（计数器 vs 字节流）。
