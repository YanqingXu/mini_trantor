# 线程模型设计分析

## 1. 设计意图

mini-trantor 采用 **one-loop-per-thread** 线程模型：
每个线程最多拥有一个 EventLoop，所有对 EventLoop 管辖对象的操作都在该线程执行。

## 2. 线程拓扑

```
┌─────────────────────────────────────────────────────────┐
│                    Main Thread                          │
│                                                         │
│  ┌───────────────────┐                                  │
│  │ Base EventLoop    │ ← TcpServer/TcpClient 生命管理   │
│  │                   │ ← Acceptor 监听                  │
│  │                   │ ← 定时器管理                     │
│  └───────────────────┘                                  │
└─────────────────────────────────────────────────────────┘
        │ EventLoopThreadPool
        │
        ├── ┌──────────────────────────────────────┐
        │   │ Worker Thread 0                      │
        │   │ ┌────────────────┐                   │
        │   │ │ Worker Loop 0  │ ← TcpConnection 1│
        │   │ │                │ ← TcpConnection 3│
        │   │ └────────────────┘                   │
        │   └──────────────────────────────────────┘
        │
        └── ┌──────────────────────────────────────┐
            │ Worker Thread 1                      │
            │ ┌────────────────────┐               │
            │ │ Worker Loop 1     │ ← TcpConn 2   │
            │ │                   │ ← TcpConn 4   │
            │ └────────────────────┘               │
            └──────────────────────────────────────┘

        ┌──────────────────────────────────────────┐
        │ DnsResolver Worker Threads (独立线程池)  │
        │ Thread 0: getaddrinfo 阻塞调用           │
        │ Thread 1: getaddrinfo 阻塞调用           │
        └──────────────────────────────────────────┘
```

## 3. 线程亲和性规则

### 绝对规则

| 规则 | 说明 | 违反后果 |
|------|------|----------|
| EventLoop::loop() 只在 owner 线程调用 | thread_local 检查 | assert 失败 |
| Channel 操作只在 owner loop 线程 | enableReading 等 | assert 失败 |
| handleRead/handleWrite/handleClose 只在 owner loop 线程 | Poller 分发 | 数据竞争 |
| sendInLoop 只在 owner loop 线程 | 由 send() 保证 | 数据竞争 |

### 跨线程安全操作

| 操作 | 机制 | 说明 |
|------|------|------|
| EventLoop::queueInLoop | mutex + eventfd wakeup | 任意线程可调用 |
| EventLoop::runInLoop | 同线程直接执行 / 跨线程 queueInLoop | 通用入口 |
| TcpConnection::send | runInLoop(sendInLoop) | 用户常用跨线程 API |
| TcpConnection::forceClose | queueInLoop(forceCloseInLoop) | 安全关闭 |

## 4. EventLoopThread 启动同步

```
Main Thread                      Worker Thread
    │                                 │
    │ startLoop()                     │
    │ ├─ create thread           ────▶│
    │ │                               │ threadFunc():
    │ │                               │   EventLoop loop (栈上)
    │ │                               │   {
    │ │                               │     lock(mutex)
    │ │  wait(cond)                   │     loop_ = &loop
    │ │  ◀─────────────────────────── │     cond.notify()
    │ │                               │   }
    │ │  return loop_                 │   loop.loop()  // 开始循环
    │ │                               │
    └─┘                               │
```

* 使用 mutex + condition_variable 确保 startLoop 返回时 loop 已构造
* EventLoop 在 worker 线程栈上创建（不是堆），由 thread 函数作用域管理

## 5. EventLoopThreadPool 连接分配

```cpp
EventLoop* EventLoopThreadPool::getNextLoop() {
    assert(baseLoop_->isInLoopThread());
    EventLoop* loop = baseLoop_;  // 默认返回 base loop

    if (!loops_.empty()) {
        loop = loops_[next_];          // round-robin
        next_ = (next_ + 1) % loops_.size();
    }

    return loop;
}
```

* **numThreads = 0**: 所有连接在 base loop（单线程模式）
* **numThreads > 0**: 新连接 round-robin 分配到 worker loop
* 分配发生在 base loop（Acceptor 的 newConnectionCallback）

## 6. 跨线程操作流程

### TcpServer 新连接（base → worker）

```
Base Loop (main)                        Worker Loop
    │                                       │
    │ Acceptor::handleRead()                │
    │ → newConnection(connfd, peerAddr)     │
    │ → loop = pool->getNextLoop()          │
    │ → create TcpConnection(loop, ...)     │
    │ → loop->runInLoop(                    │
    │     conn->connectEstablished)    ────▶│
    │                                       │ connectEstablished()
    │                                       │ → channel_->enableReading()
    │                                       │ → connectionCallback_()
```

### TcpServer 移除连接（worker → base → worker）

```
Worker Loop                     Base Loop                   Worker Loop
    │                              │                            │
    │ handleClose()                │                            │
    │ → closeCallback_(conn) ─────▶│                            │
    │                              │ removeConnection(conn)    │
    │                              │ → erase from map          │
    │                              │ → conn->getLoop()->       │
    │                              │   runInLoop(              │
    │                              │     connectDestroyed) ───▶│
    │                              │                            │ connectDestroyed()
```

三次线程跳转：worker → base → worker

### DnsResolver 解析（any → worker → callback loop）

```
Any Thread          DnsResolver Worker         Callback Loop
    │                      │                        │
    │ resolve()            │                        │
    │ → queue push  ──────▶│                        │
    │                      │ getaddrinfo()          │
    │                      │ → callbackLoop->       │
    │                      │   runInLoop(cb) ──────▶│
    │                      │                        │ cb(addrs)
```

## 7. 线程安全保障清单

| 共享资源 | 保护机制 |
|----------|----------|
| EventLoop::pendingFunctors_ | mutex_ |
| TcpServer::connections_ | 只在 base loop 线程访问 |
| TcpConnection::outputBuffer_ | 只在 owner loop 线程访问 |
| DnsResolver::requestQueue_ | queueMutex_ |
| DnsResolver::cache_ | cacheMutex_ |
| EventLoopThreadPool::loops_ | start() 后只读 |

## 8. 常见线程模型问题

| 问题 | 防护 |
|------|------|
| 多线程访问同一 TcpConnection | send() 通过 runInLoop 序列化 |
| 连接在错误线程析构 | tie() + lifetimeToken_ + queueInLoop |
| EventLoop::quit() 的线程安全 | quit_ 是 atomic，eventfd wakeup |
| 回调在析构后执行 | weak_ptr guard (tie / lifetimeToken) |
