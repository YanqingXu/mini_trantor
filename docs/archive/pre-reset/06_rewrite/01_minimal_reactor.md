# 极简 Reactor 重写指南

## 1. 目标

从零实现一个最小但完整的 Reactor 循环，包含：
- EventLoop（单线程事件循环）
- Channel（fd 事件代理）
- EPollPoller（epoll 封装）

约 300 行代码，可编译运行。

## 2. 最小 API

```cpp
// EventLoop: 核心循环
class EventLoop {
public:
    EventLoop();
    ~EventLoop();
    void loop();                              // 阻塞运行
    void quit();                              // 退出
    void runInLoop(std::function<void()> cb); // 执行回调
private:
    bool quit_{false};
    int wakeupFd_;
    std::unique_ptr<EPollPoller> poller_;
    std::unique_ptr<Channel> wakeupChannel_;
    std::vector<Channel*> activeChannels_;
    std::mutex mutex_;
    std::vector<std::function<void()>> pendingFunctors_;
};

// Channel: fd 事件代理
class Channel {
public:
    Channel(EventLoop* loop, int fd);
    void setReadCallback(std::function<void()> cb);
    void enableReading();
    void disableAll();
    void remove();
    void handleEvent();
    int fd() const;
    int events() const;
    void setRevents(int revents);
private:
    EventLoop* loop_;
    int fd_;
    int events_{0};
    int revents_{0};
    std::function<void()> readCallback_;
};

// EPollPoller: epoll 封装
class EPollPoller {
public:
    EPollPoller();
    ~EPollPoller();
    void poll(int timeoutMs, std::vector<Channel*>* activeChannels);
    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);
private:
    int epollfd_;
    std::vector<struct epoll_event> events_;
};
```

## 3. 实现骨架

### EventLoop

```cpp
EventLoop::EventLoop()
    : wakeupFd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      poller_(std::make_unique<EPollPoller>()),
      wakeupChannel_(std::make_unique<Channel>(this, wakeupFd_)) {
    wakeupChannel_->setReadCallback([this] {
        uint64_t one;
        ::read(wakeupFd_, &one, sizeof(one));
    });
    wakeupChannel_->enableReading();
}

void EventLoop::loop() {
    while (!quit_) {
        activeChannels_.clear();
        poller_->poll(10000, &activeChannels_);
        for (auto* ch : activeChannels_) {
            ch->handleEvent();
        }
        // 执行 pendingFunctors
        std::vector<std::function<void()>> functors;
        {
            std::lock_guard lock(mutex_);
            functors.swap(pendingFunctors_);
        }
        for (auto& f : functors) { f(); }
    }
}

void EventLoop::runInLoop(std::function<void()> cb) {
    {
        std::lock_guard lock(mutex_);
        pendingFunctors_.push_back(std::move(cb));
    }
    uint64_t one = 1;
    ::write(wakeupFd_, &one, sizeof(one));
}
```

### EPollPoller

```cpp
EPollPoller::EPollPoller()
    : epollfd_(::epoll_create1(EPOLL_CLOEXEC)), events_(16) {}

void EPollPoller::poll(int timeoutMs, std::vector<Channel*>* out) {
    int n = ::epoll_wait(epollfd_, events_.data(), events_.size(), timeoutMs);
    for (int i = 0; i < n; ++i) {
        auto* ch = static_cast<Channel*>(events_[i].data.ptr);
        ch->setRevents(events_[i].events);
        out->push_back(ch);
    }
    if (n == static_cast<int>(events_.size())) {
        events_.resize(events_.size() * 2);
    }
}

void EPollPoller::updateChannel(Channel* channel) {
    epoll_event ev{};
    ev.events = channel->events();
    ev.data.ptr = channel;
    ::epoll_ctl(epollfd_, EPOLL_CTL_ADD, channel->fd(), &ev);
}
```

## 4. 验证程序

```cpp
int main() {
    EventLoop loop;

    int efd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    Channel channel(&loop, efd);

    channel.setReadCallback([&] {
        uint64_t val;
        ::read(efd, &val, sizeof(val));
        printf("received event: %lu\n", val);
        loop.quit();
    });
    channel.enableReading();

    // 1 秒后触发
    std::thread([efd] {
        sleep(1);
        uint64_t one = 1;
        ::write(efd, &one, sizeof(one));
    }).detach();

    loop.loop();
    ::close(efd);
}
```

## 5. 学习要点

| 概念 | 对应代码 |
|------|----------|
| 事件循环 | EventLoop::loop() 的三阶段 |
| 事件复用 | EPollPoller::poll() + epoll_wait |
| 事件分发 | Channel::handleEvent() |
| 跨线程唤醒 | eventfd + wakeupChannel |
| 回调投递 | pendingFunctors_ + swap 优化 |

## 6. 从极简到完整的演进路径

```
极简 Reactor (本文)
  ↓ + Timer 支持 (timerfd)
  ↓ + Acceptor (listen socket)
  ↓ + TcpConnection (connected socket)
  ↓ + Buffer (non-blocking I/O 缓冲)
  ↓ + TcpServer (连接管理)
  ↓ + EventLoopThreadPool (多线程)
  ↓ + 协程桥接 (Task + Awaitable)
完整 mini-trantor
```
