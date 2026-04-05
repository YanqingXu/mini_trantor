# TcpServer 启动流程

## 调用链全景

```
main()
  → EventLoop loop                               // 主线程创建 EventLoop
  → TcpServer server(&loop, addr, name)
      → Acceptor(loop, addr, reusePort)
          → Socket(createNonblockingOrDie())      // 非阻塞 listen socket
          → Channel(loop, fd)                     // listen fd 的事件代理
          → bind(listenAddr)
      → EventLoopThreadPool(loop, name)           // worker 线程池（未启动）
  → server.setThreadNum(N)                        // 配置 worker 线程数
  → server.start()
      → threadPool_->start()
          → N × EventLoopThread::startLoop()
              → 创建子线程
              → 子线程: EventLoop → condition_variable 通知 → loop()
      → loop.runInLoop(acceptor_->listen())
          → acceptSocket_.listen()
          → acceptChannel_.enableReading()
              → Poller::updateChannel() → epoll_ctl(EPOLL_CTL_ADD)
  → loop.loop()
      → while(!quit) { epoll_wait → dispatch → doPendingFunctors }
```

## 详细步骤

### 第 1 步：创建 EventLoop

```cpp
EventLoop loop;
// 在构造函数中：
// - 记录 threadId_ = std::this_thread::get_id()
// - 创建 Poller（EPollPoller: epoll_create1）
// - 创建 TimerQueue（timerfd_create）
// - 创建 wakeupFd_（eventfd）
// - wakeupChannel_ 注册到 Poller
```

每个线程只能有一个 EventLoop，通过 `thread_local` 变量 `t_loopInThisThread` 检查。

### 第 2 步：创建 TcpServer

```cpp
TcpServer server(&loop, InetAddress(port, true), "MyServer");
```

TcpServer 构造时：
- 创建 `Acceptor`：创建非阻塞 listen socket，bind 到地址，设置 SO_REUSEADDR/SO_REUSEPORT
- 创建 `EventLoopThreadPool`：此时不启动线程
- 注册 `newConnectionCallback` 到 Acceptor

### 第 3 步：start()

```cpp
server.start();
```

`start()` 是幂等的，用 `std::atomic<bool> started_` 保护：

1. **启动线程池**：创建 N 个 EventLoopThread，每个在独立线程中构造 EventLoop 并进入 `loop()`
2. **启动监听**：通过 `runInLoop` 确保 `acceptor_->listen()` 在 base loop 线程执行

### 第 4 步：进入主循环

```cpp
loop.loop();
// 此后 main 线程进入事件循环，等待连接到来
```

### 时序图

```
Main Thread                     Worker Thread 1          Worker Thread 2
    │                               │                       │
    ├─ new EventLoop                │                       │
    ├─ new TcpServer                │                       │
    │   ├─ new Acceptor             │                       │
    │   └─ new ThreadPool           │                       │
    │                               │                       │
    ├─ server.start()               │                       │
    │   ├─ threadPool->start()      │                       │
    │   │   ├───────────────────────┤                       │
    │   │   │  EventLoop()          │                       │
    │   │   │  cond.notify() ───►   │                       │
    │   │   ├───────────────────────┼───────────────────────┤
    │   │   │                       │  EventLoop()          │
    │   │   │                       │  cond.notify() ───►   │
    │   │                           │                       │
    │   └─ runInLoop(listen)        │                       │
    │       └─ acceptor->listen()   │                       │
    │           └─ epoll_ctl(ADD)   │                       │
    │                               │                       │
    ├─ loop.loop()                  ├─ loop.loop()          ├─ loop.loop()
    │   epoll_wait...               │   epoll_wait...       │   epoll_wait...
```
