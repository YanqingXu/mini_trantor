# 新连接建立流程

## 调用链全景

```
Client connect() → 内核完成三次握手
  → epoll_wait 返回 listen fd EPOLLIN
  → Channel::handleEvent()
  → Acceptor::handleRead()
      → acceptSocket_.accept(&peerAddr)               // 获取 connfd
      → newConnectionCallback_(connfd, peerAddr)       // 回调 TcpServer
  → TcpServer::newConnection(connfd, peerAddr)
      → threadPool_->getNextLoop()                     // round-robin 选择 worker loop
      → new TcpConnection(ioLoop, name, connfd, localAddr, peerAddr)
      → connections_[name] = connection                // 存入连接映射
      → 设置回调 (connection/message/close/write)
      → ioLoop->runInLoop(connection->connectEstablished())
  → TcpConnection::connectEstablished()
      → setState(kConnected)
      → channel_->tie(shared_from_this())              // tie 防止回调中连接被销毁
      → channel_->enableReading()                      // 注册到 worker loop 的 epoll
      → connectionCallback_(shared_from_this())        // 通知上层：连接已建立
```

## 详细步骤

### 第 1 步：内核 accept

Acceptor 的 Channel 监听 listen fd 的 EPOLLIN 事件。当有新连接到达时：

```cpp
void Acceptor::handleRead(Timestamp receiveTime) {
    while (true) {
        InetAddress peerAddr;
        int connfd = acceptSocket_.accept(&peerAddr);
        if (connfd >= 0) {
            newConnectionCallback_(connfd, peerAddr);  // → TcpServer::newConnection
        } else if (errno == EAGAIN) {
            break;   // 所有连接已接受完毕
        }
    }
}
```

注意 `while(true)` —— 一次 EPOLLIN 可能有多个连接等待 accept。

### 第 2 步：TcpServer 创建连接

```cpp
void TcpServer::newConnection(int sockfd, const InetAddress& peerAddr) {
    // 运行在 base loop 线程
    EventLoop* ioLoop = threadPool_->getNextLoop();     // 选择 worker loop
    auto connection = std::make_shared<TcpConnection>(...);
    connections_[connName] = connection;                  // 记录连接

    // 设置回调链...
    connection->setConnectionCallback(connectionCallback_);
    connection->setMessageCallback(messageCallback_);
    connection->setCloseCallback([this](auto& conn) { removeConnection(conn); });

    // TLS 支持
    if (tlsContext_) {
        ioLoop->runInLoop([connection, ctx] {
            connection->startTls(ctx, true);
            connection->connectEstablished();
        });
    } else {
        ioLoop->runInLoop([connection] { connection->connectEstablished(); });
    }
}
```

### 第 3 步：线程切换

`newConnection()` 在 base loop 线程执行，但 `connectEstablished()` 通过 `runInLoop` 投递到 worker loop 线程。

```
Base Loop Thread                    Worker Loop Thread
    │                                   │
    ├─ newConnection()                  │
    │   ├─ getNextLoop() → ioLoop       │
    │   ├─ new TcpConnection            │
    │   ├─ connections_[name] = conn    │
    │   └─ ioLoop->runInLoop(...)  ────►│
    │                                   ├─ connectEstablished()
    │                                   │   ├─ setState(kConnected)
    │                                   │   ├─ channel_->tie()
    │                                   │   ├─ channel_->enableReading()
    │                                   │   │   └─ epoll_ctl(ADD, connfd)
    │                                   │   └─ connectionCallback_()
    │                                   │
```

### 第 4 步：connectEstablished

```cpp
void TcpConnection::connectEstablished() {
    loop_->assertInLoopThread();
    setState(kConnected);
    channel_->tie(shared_from_this());   // weak_ptr 防止回调中 this 被释放
    channel_->enableReading();           // 开始监听数据到达
    if (connectionCallback_) {
        connectionCallback_(shared_from_this());
    }
}
```

`tie()` 机制的关键作用：Channel 回调执行时，通过 `weak_ptr::lock()` 获取 `shared_ptr`，保证 TcpConnection 在回调期间不被销毁。

## 关键不变量

- `connections_` 映射只在 base loop 线程修改
- TcpConnection 的所有可变状态只在 owner (worker) loop 线程修改
- Channel 注册到哪个 EventLoop，就永远在那个线程操作
