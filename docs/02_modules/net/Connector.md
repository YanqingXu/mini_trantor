# Connector —— 工程级源码拆解

## 1. 类定位

* **角色**：TcpClient 的**主动连接适配器**，与 Acceptor 对称
* **层级**：Net 层（TcpClient 的内部组件）

## 2. 解决的问题

封装非阻塞 connect 的完整流程：创建 socket → 发起 connect → 处理 EINPROGRESS → 检测连接就绪 → 交付 fd。
包含指数退避重试策略。

## 3. 对外接口

| 方法 | 用途 |
|------|------|
| `start()` | 发起连接（通过 runInLoop） |
| `stop()` | 停止连接/取消重试 |
| `restart()` | 重新发起连接（重置退避） |
| `setNewConnectionCallback(cb)` | 设置连接成功回调 |
| `setRetryDelay(initial, max)` | 配置退避参数 |

## 4. 核心成员变量

```cpp
EventLoop* loop_;
InetAddress serverAddr_;
StateE state_;                     // kDisconnected / kConnecting / kConnected
bool connect_;                     // 是否应该连接（stop 设为 false）
std::unique_ptr<Channel> channel_; // 连接中临时使用的 Channel
Duration retryDelayMs_;            // 当前退避延迟（500ms 起步）
Duration maxRetryDelayMs_;         // 最大退避延迟（30s）
TimerId retryTimerId_;             // 重试定时器 ID
```

## 5. 执行流程

### connect

```
connect():
  ├─ sockfd = createNonblockingOrDie()
  ├─ ::connect(sockfd, serverAddr)
  │   ├─ errno == 0/EINPROGRESS/EINTR/EISCONN → connecting(sockfd)
  │   ├─ EAGAIN/ECONNREFUSED/... → retry(sockfd)
  │   └─ EACCES/EPERM/... → close(sockfd) (不可恢复)
```

### connecting

```
connecting(sockfd):
  ├─ state_ = kConnecting
  ├─ channel_ = new Channel(loop, sockfd)
  ├─ channel_->setWriteCallback(handleWrite)
  ├─ channel_->setErrorCallback(handleError)
  └─ channel_->enableWriting()     // 等待 EPOLLOUT = 连接完成
```

### handleWrite

```
handleWrite():
  ├─ sockfd = removeAndResetChannel()
  ├─ getsockopt(SOL_SOCKET, SO_ERROR)
  │   ├─ err != 0 → retry(sockfd)
  │   └─ err == 0 → 连接成功
  ├─ 检查 self-connect (localAddr == peerAddr) → retry
  ├─ state_ = kConnected
  └─ newConnectionCallback_(sockfd)     // 交给 TcpClient
```

### retry（指数退避）

```
retry(sockfd):
  ├─ close(sockfd)
  ├─ state_ = kDisconnected
  ├─ runAfter(retryDelayMs_, startInLoop)   // 定时重试
  └─ retryDelayMs_ = min(retryDelayMs_ * 2, maxRetryDelayMs_)
```

退避序列：500ms → 1s → 2s → 4s → 8s → 16s → 30s → 30s → ...

### removeAndResetChannel

```cpp
int removeAndResetChannel() {
    channel_->disableAll();
    channel_->remove();
    int sockfd = channel_->fd();
    // 延迟 reset：当前在 Channel::handleEvent 调用栈中，不能立即销毁
    loop_->queueInLoop([self] { self->resetChannel(); });
    return sockfd;
}
```

## 6. 关键设计点

* **enable_shared_from_this**：Connector 通过 shared_ptr 管理，因为 retry timer 需要持有引用
* **self-connect 检测**：本地端口 == 对端端口 → 说明连接到了自己（TCP 同时打开），auto retry
* **Channel 延迟销毁**：在 handleWrite 回调中不能直接 reset Channel

## 7. 面试角度

**Q: 非阻塞 connect 的完整流程？**
A: connect() 返回 EINPROGRESS → 注册 EPOLLOUT → epoll_wait → getsockopt(SO_ERROR) 检查结果。

**Q: 如何检测 self-connect？**
A: 连接成功后比较 local addr 和 peer addr，相同则说明连接到了自己。

**Q: 为什么 Channel 要延迟 reset？**
A: handleWrite 在 Channel::handleEvent 调用链中执行，此时 Channel 正在被使用，不能立即销毁。
