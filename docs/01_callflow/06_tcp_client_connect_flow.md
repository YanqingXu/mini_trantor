# TcpClient 连接流程

## 整体流程

```
用户代码:
  TcpClient client(loop, serverAddr, "client");
  client.setConnectionCallback(onConnection);
  client.setMessageCallback(onMessage);
  client.connect();

connect() 内部:
  → hostname? → DnsResolver::resolve() (异步)
  → IP地址?  → connector_->start()
```

## Connector 连接状态机

```
States::kDisconnected
    │
    ├─ start() → runInLoop(startInLoop)
    │
States::kConnecting
    │
    ├─ connect()
    │   ├─ socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK)
    │   └─ ::connect(sockfd, addr, ...)
    │       │
    │       ├─ 返回 0 (立即连接成功，常见于 localhost)
    │       │   └─ connecting(sockfd)
    │       │
    │       ├─ errno == EINPROGRESS (正在连接，最常见)
    │       │   └─ connecting(sockfd)
    │       │
    │       └─ 其他 errno (连接失败)
    │           ├─ close(sockfd)
    │           └─ retry(sockfd)
    │
    ├─ connecting(sockfd):
    │   ├─ new Channel(loop, sockfd)
    │   ├─ channel_->setWriteCallback(handleWrite)
    │   ├─ channel_->setErrorCallback(handleError)
    │   └─ channel_->enableWriting()  // 等待可写 = 连接完成
    │
    ├─ handleWrite():               // EPOLLOUT 到达
    │   ├─ getsockopt(SO_ERROR) 检查连接结果
    │   │   ├─ err == 0 → 连接成功
    │   │   │   ├─ removeAndResetChannel()
    │   │   │   └─ newConnectionCallback_(sockfd)
    │   │   └─ err != 0 → 连接失败
    │   │       └─ retry(sockfd)
    │   │
States::kConnected
    │
    └─ handleError():
        └─ getsockopt(SO_ERROR)
        └─ retry(sockfd)
```

## 重试与指数退避

```cpp
void Connector::retry(int sockfd) {
    ::close(sockfd);
    if (state_ == States::kConnecting) {
        // 指数退避：500ms → 1000ms → 2000ms → ... → 30000ms (上限)
        loop_->runAfter(retryDelayMs_ / 1000.0, [this] { startInLoop(); });
        retryDelayMs_ = std::min(retryDelayMs_ * 2, kMaxRetryDelayMs);
    }
}
```

退避参数：
| 参数 | 值 |
|------|-----|
| 初始延迟 | 500ms |
| 最大延迟 | 30s |
| 增长方式 | 每次翻倍 |

## DNS 解析路径（TcpClient）

当连接目标是主机名时：

```
TcpClient::connect()
  → DnsResolver::resolve(hostname, [this](InetAddress addr) {
        serverAddr_ = addr;
        connector_->start();
    });
```

`resolveGuard_` 保护机制：TcpClient 析构前 `resolveGuard_.reset()`，
DNS 回调中通过 `weak_ptr::lock()` 检查 TcpClient 是否还存活。

## removeAndResetChannel 细节

连接完成后,需要重置 Channel，但不能在 Channel 的回调中销毁自己：

```cpp
void Connector::removeAndResetChannel() {
    channel_->disableAll();
    channel_->remove();
    // 不能直接 channel_.reset()，因为当前在 Channel::handleEvent 调用栈中
    loop_->queueInLoop([this] { channel_.reset(); });  // 延迟销毁
}
```

## TcpClient 新连接建立

```
Connector::newConnectionCallback_(sockfd)
  → TcpClient::newConnection(sockfd)
      → 创建 TcpConnection
      → 设置 callbacks
      → conn->connectEstablished()
          → channel_->enableReading()
          → connectionCallback_()
```

## 断线重连

```cpp
TcpClient::connect() {
    connect_ = true;            // 标记需要重连
    connector_->start();
}

// 连接断开后：
TcpClient::removeConnection(conn) {
    if (connect_) {                   // 需要重连
        connector_->restart();        // 重新发起连接
    }
}
```
