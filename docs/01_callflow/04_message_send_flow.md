# 消息发送流程

## 调用链

```
用户调用: conn->send("Hello")
  → TcpConnection::send(data, len)
      → isInLoopThread?
          YES → sendInLoop(data, len) 直接执行
          NO  → 拷贝 data，runInLoop(sendInLoop) 投递
  → sendInLoop()
      → 尝试直接 write()
          成功且全部写完 → writeCompleteCallback_()
          部分写入 → 剩余追加到 outputBuffer_, enableWriting()
      → (等待 EPOLLOUT)
  → handleWrite() (EPOLLOUT 事件)
      → write(fd, outputBuffer_.peek(), readableBytes())
      → outputBuffer_.retrieve(n)
      → 全部写完 → disableWriting(), writeCompleteCallback_()
      → 如果 kDisconnecting → shutdownInLoop()
```

## 详细步骤

### 第 1 步：跨线程安全的 send

```cpp
void TcpConnection::send(const void* data, size_t len) {
    if (state_ != kConnected) return;

    if (loop_->isInLoopThread()) {
        sendInLoop(static_cast<const char*>(data), len);
    } else {
        // 跨线程：必须拷贝数据（原始指针可能失效）
        auto self = shared_from_this();
        std::string payload(data, len);
        loop_->runInLoop([self, payload]() {
            self->sendInLoop(payload.data(), payload.size());
        });
    }
}
```

### 第 2 步：sendInLoop —— 核心发送逻辑

```cpp
void TcpConnection::sendInLoop(const char* data, size_t len) {
    // 场景 1：outputBuffer 为空且未在写 → 尝试直接写
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
        ssize_t nwrote = ::write(fd, data, len);
        size_t remaining = len - nwrote;

        if (remaining == 0) {
            // 全部写完，触发写完成回调
            writeCompleteCallback_();
            return;
        }
    }

    // 场景 2：有剩余数据，追加到 outputBuffer
    outputBuffer_.append(data + nwrote, remaining);
    if (!channel_->isWriting()) {
        channel_->enableWriting();  // 注册 EPOLLOUT
    }

    // 高水位检查
    if (highWaterMarkCallback_ && newLen >= highWaterMark_) {
        highWaterMarkCallback_(self, newLen);
    }

    // 背压策略
    applyBackpressurePolicy();
}
```

### 第 3 步：handleWrite —— 处理 EPOLLOUT

```cpp
void TcpConnection::handleWrite() {
    if (!channel_->isWriting()) return;

    ssize_t n = outputBuffer_.writeFd(fd, &savedErrno);
    if (n > 0) {
        outputBuffer_.retrieve(n);
        applyBackpressurePolicy();

        if (outputBuffer_.readableBytes() == 0) {
            channel_->disableWriting();       // 不再关注可写事件
            writeCompleteCallback_();          // 通知写完成
            if (state_ == kDisconnecting) {
                shutdownInLoop();              // 延迟关闭
            }
        }
    }
}
```

## 发送流程图

```
用户 send("Hello World")
    │
    ▼ isInLoopThread?
    ├── YES ──► sendInLoop()
    │               │
    │               ├─ outputBuffer 空？
    │               │   YES → write(fd, "Hello World", 11)
    │               │         ├─ 全部写完 → writeCompleteCallback ✓
    │               │         └─ 写了 5 字节 → "World" 追加到 outputBuffer
    │               │                         → enableWriting()
    │               │   NO → 追加到 outputBuffer → enableWriting()
    │               │
    │               ▼
    │           (等待 EPOLLOUT)
    │               │
    │               ▼
    │           handleWrite()
    │               ├─ write(fd, "World", 5)
    │               └─ retrieve(5)
    │                   └─ buffer 空 → disableWriting ✓
    │
    └── NO ──► 拷贝 data 到 string
               runInLoop(sendInLoop)
               wakeup(eventfd) → 目标线程执行
```

## 背压策略

当 outputBuffer 增长过快时启动背压：

```
outputBuffer >= highWaterMark → 停止读取（disableReading）
outputBuffer <= lowWaterMark  → 恢复读取（enableReading）
```

这防止快速发送慢速消费导致内存无限增长。

## TLS 路径

TLS 连接的 sendInLoop 使用 `SSL_write()` 而非 `::write()`：

```cpp
if (ssl_) {
    int n = SSL_write(ssl_, data, len);
    // SSL_ERROR_WANT_WRITE → 数据放入 outputBuffer 等待
}
```

`handleWrite()` 中使用 `sslWriteFromBuffer()` 代替 `outputBuffer_.writeFd()`。
