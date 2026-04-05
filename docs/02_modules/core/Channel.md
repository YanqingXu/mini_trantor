# Channel —— 工程级源码拆解

## 1. 类定位

* **角色**：fd 与 EventLoop 之间的**事件订阅与分发桥梁**
* **层级**：Reactor 层（核心底层）
* 每个 Channel 绑定一个 fd 和一个 EventLoop，负责"订阅感兴趣的事件"和"分发就绪的事件"

```
       EventLoop
          │
          ├─── Poller (epoll)
          │       │
          │       ├── epoll_ctl(fd, events)  ◄── Channel::update()
          │       └── revents ──────────────►  Channel::setRevents()
          │
          └─ for ch in activeChannels:
                 ch->handleEvent()  ──► readCb / writeCb / closeCb / errorCb
```

Channel **不拥有 fd**，也**不拥有 EventLoop**，它只是一个"代理注册和回调分发"的轻量实体。

## 2. 解决的问题

**核心问题**：如何将 epoll 的低级事件（EPOLLIN / EPOLLOUT / EPOLLERR / EPOLLHUP）映射到面向对象的回调？

如果没有 Channel：
- 每次 `epoll_wait` 返回后，需要手动查表找到 fd 对应的处理函数
- 事件类型判断散落在各处，容易遗漏 HUP/ERR 处理
- 没有统一的"取消订阅"和"生命周期保护"机制
- TcpConnection、TimerQueue、Acceptor 等都需要各自实现与 epoll 的交互

Channel 把 epoll 的事件模型**封装为回调模型**：
- 用户设置 `readCallback_` → Channel 在 EPOLLIN 时调用
- 用户调用 `enableWriting()` → Channel 自动 `epoll_ctl(EPOLL_CTL_MOD)`

## 3. 对外接口（API）

### 事件注册（Owner 调用）

| 方法 | 用途 |
|------|------|
| `enableReading()` | 订阅 EPOLLIN &#124; EPOLLPRI |
| `disableReading()` | 取消订阅读事件 |
| `enableWriting()` | 订阅 EPOLLOUT |
| `disableWriting()` | 取消订阅写事件 |
| `disableAll()` | 取消所有事件订阅 |
| `remove()` | 从 Poller 中移除（必须先 disableAll） |

### 回调设置（Owner 调用）

| 方法 | 回调触发条件 |
|------|-------------|
| `setReadCallback(cb)` | EPOLLIN &#124; EPOLLPRI &#124; EPOLLRDHUP |
| `setWriteCallback(cb)` | EPOLLOUT |
| `setCloseCallback(cb)` | EPOLLHUP（无 EPOLLIN） |
| `setErrorCallback(cb)` | EPOLLERR |

### 生命周期保护

| 方法 | 用途 |
|------|------|
| `tie(shared_ptr)` | 绑定 owner 的 weak_ptr，防止回调期间 owner 被销毁 |

### 内部方法（EventLoop / Poller 调用）

| 方法 | 调用者 | 用途 |
|------|--------|------|
| `handleEvent(timestamp)` | EventLoop::loop() | 分发就绪事件 |
| `setRevents(revents)` | Poller | 设置 epoll 返回的实际事件 |
| `events()` | Poller | 获取关注的事件掩码 |
| `index()` / `setIndex()` | Poller | Poller 内部的 Channel 状态(kNew/kAdded/kDeleted) |

## 4. 核心成员变量

```cpp
EventLoop* loop_;                 // 所属 EventLoop（不拥有，原始指针）
const int fd_;                    // 绑定的文件描述符（不拥有，不 close）
uint32_t events_;                 // 关注的事件集合（EPOLLIN | EPOLLOUT 等）
uint32_t revents_;                // Poller 返回的实际事件
int index_;                       // Poller 内部状态：-1(kNew), 1(kAdded), 2(kDeleted)
bool eventHandling_;              // 是否正在 handleEventWithGuard 中（防止回调中析构）
bool addedToLoop_;                // 是否已注册到 Poller（析构检查用）
bool tied_;                       // 是否启用了 tie 保护
std::weak_ptr<void> tie_;         // owner 的 weak_ptr（type-erased）
ReadEventCallback readCallback_;  // 读回调（带 Timestamp 参数）
EventCallback writeCallback_;     // 写回调
EventCallback closeCallback_;     // 关闭回调
EventCallback errorCallback_;     // 错误回调
```

### 线程安全

所有成员变量都只在 owner EventLoop 线程访问，无需加锁。

## 5. 执行流程（最重要）

### 5.1 构造

```
Channel::Channel(loop, fd)
  ├─ loop_ = loop
  ├─ fd_ = fd
  ├─ events_ = kNoneEvent (0)
  ├─ revents_ = 0
  ├─ index_ = -1 (kNew: 未注册到 Poller)
  ├─ eventHandling_ = false
  ├─ addedToLoop_ = false
  └─ tied_ = false
```

### 5.2 注册事件

```
conn->enableReading()
  → channel_->enableReading()
      → events_ |= kReadEvent
      → update()
          → addedToLoop_ = true
          → loop_->updateChannel(this)
              → poller_->updateChannel(this)
                  // index_ == -1 (kNew) → epoll_ctl(EPOLL_CTL_ADD)
                  // index_ == 1 (kAdded) → epoll_ctl(EPOLL_CTL_MOD)
                  // index_ == 2 (kDeleted) → epoll_ctl(EPOLL_CTL_ADD)
```

### 5.3 事件分发

```
epoll_wait() 返回
  → Poller 为每个活跃 fd:
      channel->setRevents(revents)
      加入 activeChannels

  → EventLoop::loop():
      for (channel : activeChannels):
          channel->handleEvent(pollReturnTime)
```

```cpp
void Channel::handleEvent(Timestamp receiveTime) {
    std::shared_ptr<void> guard;
    if (tied_) {
        guard = tie_.lock();       // 提升 weak_ptr → shared_ptr
        if (guard) {
            handleEventWithGuard(receiveTime);  // owner 还活着
        }
        // guard 为空 → owner 已销毁，静默跳过
        return;
    }
    handleEventWithGuard(receiveTime);  // 未 tie() 的 Channel（如 wakeupChannel）
}
```

### 5.4 handleEventWithGuard —— 事件优先级

```cpp
void Channel::handleEventWithGuard(Timestamp receiveTime) {
    eventHandling_ = true;

    // 优先级 1: HUP（对端关闭且无数据可读）
    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) {
        if (closeCallback_) closeCallback_();
    }

    // 优先级 2: ERROR
    if (revents_ & EPOLLERR) {
        if (errorCallback_) errorCallback_();
    }

    // 优先级 3: READ（EPOLLIN | EPOLLPRI | EPOLLRDHUP）
    if (revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
        if (readCallback_) readCallback_(receiveTime);
    }

    // 优先级 4: WRITE
    if (revents_ & EPOLLOUT) {
        if (writeCallback_) writeCallback_();
    }

    eventHandling_ = false;
}
```

**为什么 HUP 要排除有 EPOLLIN 的情况？**
因为 EPOLLHUP + EPOLLIN 说明有数据待读（TCP close 前的最后一批数据），
应该先走 readCallback_ 读取数据，由 `read() == 0` 触发 handleClose。

### 5.5 移除与析构

```
disableAll()
  → events_ = kNoneEvent
  → update()
      → epoll_ctl(EPOLL_CTL_DEL) 或标记 kDeleted

remove()
  → 检查 isNoneEvent()（必须先 disableAll）
  → addedToLoop_ = false
  → loop_->removeChannel(this)
      → poller_->removeChannel(this)
          → epoll_ctl(EPOLL_CTL_DEL)

~Channel()
  → assert(!eventHandling_)    // 不能在回调中析构
  → assert(!addedToLoop_)      // 必须先 remove()
```

**remove-before-destroy 不变式**：Channel 析构前必须 `disableAll() + remove()`。
违反此规则会导致 epoll 持有悬空指针 → 段错误。

## 6. 关键交互关系

```
              TcpConnection     Acceptor     TimerQueue     EventLoop
              (owner)           (owner)       (owner)       (wakeup owner)
                  │                │              │              │
                  └──── 创建 ──────┴──── 创建 ────┴──── 创建 ────┘
                                        │
                                   Channel
                                   /    \
                            update()    handleEvent()
                              │              │
                              ▼              │
                          EventLoop ←────────┘
                              │
                              ▼
                           Poller
                           (epoll)
```

| 关系 | 描述 |
|------|------|
| TcpConnection → Channel | 一个连接拥有一个 Channel，设置 read/write/close/error 回调 |
| Acceptor → Channel | 监听 socket 的 Channel，只关注 read 事件 |
| TimerQueue → Channel | timerfd 的 Channel，只关注 read 事件 |
| EventLoop → Channel | wakeupChannel，只关注 read 事件 |
| Channel → EventLoop | update() / remove() 回调 |
| Channel → Poller | 通过 EventLoop 间接交互 |

## 7. 关键设计点

### 7.1 tie() 机制

```cpp
void Channel::tie(const std::shared_ptr<void>& object) {
    tie_ = object;     // 存 weak_ptr
    tied_ = true;
}
```

**解决的问题**：在 `handleEvent` 回调链中，如果 `closeCallback` 触发了 TcpConnection 的销毁，
后续的 `readCallback` 等可能访问已释放的 TcpConnection 成员。

**解法**：`handleEvent` 开始时用 `tie_.lock()` 提升为 `shared_ptr`，
只要 `guard` 存活，TcpConnection 就不会被析构。

典型使用：
```cpp
// TcpConnection::connectEstablished() 中：
channel_->tie(shared_from_this());
```

### 7.2 不拥有 fd

Channel **不负责 close(fd)**。fd 由其 owner 管理：
- TcpConnection 的 fd 由 `Socket` 对象的析构函数 close
- TimerQueue 的 timerfd 由 TimerQueue 自己 close
- wakeupFd 由 EventLoop 自己 close

这避免了重复 close 和 ownership 混乱。

### 7.3 index 三态

```
kNew (-1)     → 从未注册到 Poller
kAdded (1)    → 当前在 epoll 中
kDeleted (2)  → 曾在 epoll 中，已被 DEL，但 Poller 的 map 里还有记录
```

kDeleted 状态允许 Channel 被重新激活（如 re-enableReading），此时会从 kDeleted → kAdded。

## 8. 潜在问题

### 8.1 回调中删除自身 Channel

`handleEventWithGuard` 按顺序调用 close → error → read → write。
如果 `closeCallback` 中调用了 `forceClose()` 导致 Channel 被 `disableAll() + remove()`，
后续的 read/write 回调可能操作已移除的 Channel。

**当前保护**：`tie()` + `shared_ptr guard` 防止 TcpConnection 析构，但 Channel 本身的 `events_` 可能已被修改。
实际运行中，`handleClose` 中 `disableAll()` 把 `events_` 清零，但 `revents_` 还在 → 后续 if 分支仍会进入。

**风险等级**：低。实际使用中 handleClose 会设置 `state_ = kDisconnected`，后续回调检测到此状态会返回。

### 8.2 events_ 和 revents_ 无 atomic

都在 loop 线程操作，无需 atomic。但如果有人错误地在其他线程调用 `enableReading()`，会产生数据竞争。
**保护**：`EventLoop::assertInLoopThread()` 在 `updateChannel` 中检查。

## 9. 极简实现

```cpp
class MinimalChannel {
public:
    MinimalChannel(int fd) : fd_(fd), events_(0), revents_(0) {}

    void setReadCallback(std::function<void()> cb) { readCb_ = cb; }
    void enableReading() { events_ |= EPOLLIN; /* update epoll */ }
    void setRevents(uint32_t rev) { revents_ = rev; }

    void handleEvent() {
        if (revents_ & EPOLLIN) readCb_();
        if (revents_ & EPOLLOUT) writeCb_();
    }

private:
    int fd_;
    uint32_t events_, revents_;
    std::function<void()> readCb_, writeCb_;
};
```

从极简 → 完整，需要加：
1. **tie() 机制** → 防止回调期间 owner 被销毁
2. **HUP/ERR 分类处理** → 正确关闭连接
3. **index 状态管理** → 与 Poller 配合
4. **remove-before-destroy 检查** → 防止悬空 epoll 事件
5. **eventHandling_ 防重入** → 防止回调中析构

## 10. 面试角度总结

**Q1: Channel 是什么？和 fd 的关系？**
A: Channel 是 fd 在 EventLoop 中的事件代理。它不拥有 fd，只负责订阅事件和分发回调。一个 fd 对应一个 Channel。

**Q2: handleEvent 中为什么要 tie？**
A: 防止回调中 closeCallback 销毁了 TcpConnection（Channel 的 owner），导致后续回调访问悬空指针。tie 用 weak_ptr → shared_ptr 提升保护。

**Q3: Channel 的 index 三个状态是什么意思？**
A: -1(kNew) 未注册、1(kAdded) 在 epoll 中、2(kDeleted) 已删除但 Poller 仍记录。3 个状态让 Channel 可以被重用。

**Q4: 为什么 HUP 要判断是否有 EPOLLIN？**
A: EPOLLHUP + EPOLLIN 说明有数据待读（对端发了 FIN 但还有数据），应先读完再关闭。纯 HUP 才直接走 closeCallback。

**Q5: remove-before-destroy 是什么？为什么需要？**
A: Channel 析构前必须先 remove。否则 epoll 仍持有 Channel 指针，下次 epoll_wait 返回时访问已释放内存 → 段错误。

**Q6: Channel 的线程安全怎么保证？**
A: 不加锁。所有操作都由 EventLoop::assertInLoopThread() 保证在 owner 线程执行。
