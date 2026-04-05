# 消息接收流程

## 调用链

```
内核: 数据到达 socket 接收缓冲区
  → epoll_wait 返回 connfd EPOLLIN
  → Channel::handleEvent(timestamp)
      → handleEventWithGuard()
          → readCallback_(timestamp)
  → TcpConnection::handleRead(timestamp)
      → inputBuffer_.readFd(fd, &savedErrno)    // 从 fd 读数据到 Buffer
      → messageCallback_(shared_from_this(), &inputBuffer_)  // 通知用户
```

## 详细步骤

### 第 1 步：Poller 检测可读事件

```cpp
// EPollPoller::poll()
int numEvents = epoll_wait(epollfd_, events_.data(), events_.size(), timeoutMs);
// 遍历活跃事件，设置 Channel::revents_
for (int i = 0; i < numEvents; ++i) {
    Channel* channel = static_cast<Channel*>(events_[i].data.ptr);
    channel->setRevents(events_[i].events);
    activeChannels->push_back(channel);
}
```

### 第 2 步：Channel 分发

```cpp
void Channel::handleEventWithGuard(Timestamp receiveTime) {
    eventHandling_ = true;
    // EPOLLIN → readCallback_
    if (revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
        if (readCallback_) {
            readCallback_(receiveTime);
        }
    }
    eventHandling_ = false;
}
```

### 第 3 步：TcpConnection::handleRead

```cpp
void TcpConnection::handleRead(Timestamp receiveTime) {
    int savedErrno = 0;
    ssize_t n;

    if (ssl_) {
        n = sslReadIntoBuffer(&savedErrno);   // TLS 路径
    } else {
        n = inputBuffer_.readFd(channel_->fd(), &savedErrno);  // 普通路径
    }

    if (n > 0) {
        // 有数据
        resumeReadWaiterIfNeeded();              // 先检查协程 waiter
        if (messageCallback_ && !hasReadWaiter) {
            messageCallback_(shared_from_this(), &inputBuffer_);  // 回调模式
        }
    } else if (n == 0) {
        handleClose();  // 对端关闭
    } else {
        handleError(savedErrno);  // 读取错误
    }
}
```

### 第 4 步：Buffer::readFd —— scatter/gather I/O

```cpp
ssize_t Buffer::readFd(int fd, int* savedErrno) {
    char extraBuffer[65536];       // 64KB 栈上临时缓冲区
    struct iovec vec[2];
    const size_t writable = writableBytes();

    vec[0].iov_base = beginWrite();    // Buffer 已有空间
    vec[0].iov_len = writable;
    vec[1].iov_base = extraBuffer;     // 栈上额外空间
    vec[1].iov_len = sizeof(extraBuffer);

    // readv: 一次系统调用读到两块内存
    const ssize_t n = ::readv(fd, vec, writable < sizeof(extraBuffer) ? 2 : 1);

    if (n <= writable) {
        writerIndex_ += n;              // 数据全部在 Buffer 内
    } else {
        writerIndex_ = buffer_.size();  // Buffer 满了
        append(extraBuffer, n - writable);  // 额外数据追加
    }
    return n;
}
```

**为什么用 readv？** 一次系统调用最多读 `Buffer 剩余空间 + 64KB`，避免 Buffer 预分配过大，又不浪费可读数据。

## 数据流图

```
内核 socket 接收缓冲区
    │
    │ readv()
    ▼
┌─────────────────────────────────────────────┐
│ Buffer                                       │
│ ┌──────────┬──────────────┬────────────────┐│
│ │ prepend  │ readable     │ writable       ││
│ │ (8 bytes)│ (已读数据)    │ (可写空间)     ││
│ └──────────┴──────────────┴────────────────┘│
│             ▲              ▲                 │
│         readerIndex_    writerIndex_         │
└─────────────────────────────────────────────┘
    │
    │ messageCallback_(conn, &buffer)
    ▼
用户代码 → buffer->retrieveAllAsString() 取出数据
```

## TLS 路径

TLS 连接的读取走 `sslReadIntoBuffer()`：

```cpp
ssize_t TcpConnection::sslReadIntoBuffer(int* savedErrno) {
    char buf[16384];  // 16KB 块
    while (true) {
        int n = SSL_read(ssl_, buf, sizeof(buf));
        if (n > 0) {
            inputBuffer_.append(buf, n);  // 解密后数据追加到 Buffer
        } else {
            int err = SSL_get_error(ssl_, n);
            if (err == SSL_ERROR_WANT_READ) break;       // 需要更多数据
            if (err == SSL_ERROR_ZERO_RETURN) return 0;   // close_notify
            return -1;  // 错误
        }
    }
}
```

区别：普通路径 `readv()` 直接读原始字节；TLS 路径 `SSL_read()` 读解密后的明文。
