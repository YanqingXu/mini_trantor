# 协程恢复流程

## 核心机制

协程在 mini-trantor 中通过 reactor 事件触发恢复，不使用独立的协程调度器。
关键原则：**协程恢复必须在正确的 EventLoop 线程上执行**。

## Read Awaitable 流程

### 挂起阶段

```
用户协程: auto result = co_await conn->coRead(timeout);
  → ReadAwaitable::await_ready() → false (总是挂起)
  → ReadAwaitable::await_suspend(handle)
      → conn_->armReadWaiter(handle, timeout)
          → readWaiterHandle_ = handle     // 保存协程句柄
          → 如果有 timeout:
              readWaiterTimer_ = loop_->runAfter(timeout, [conn] {
                  conn->resumeReadWaiterIfNeeded(/*timedOut=*/true);
              });
          → 如果 inputBuffer 已有数据:
              resumeReadWaiterIfNeeded(false)  // 立即恢复
```

### 数据到达恢复

```
epoll_wait → EPOLLIN
  → TcpConnection::handleRead()
      → readFd() → 数据写入 inputBuffer
      → resumeReadWaiterIfNeeded(false)
          → if (readWaiterHandle_):
              auto h = std::exchange(readWaiterHandle_, nullptr);
              cancelReadWaiterTimer();
              queueResume(h);               // 投递到 loop 中恢复
```

### 超时恢复

```
TimerQueue → timeout 到期
  → resumeReadWaiterIfNeeded(true)
      → if (readWaiterHandle_):
          auto h = std::exchange(readWaiterHandle_, nullptr);
          readWaiterTimedOut_ = true;       // 标记超时
          queueResume(h);
```

### queueResume —— 安全恢复入口

```cpp
void TcpConnection::queueResume(std::coroutine_handle<> h) {
    loop_->queueInLoop([h] { h.resume(); });
    // 恢复操作作为 pending functor 在 doPendingFunctors 中执行
    // 确保在 loop 线程上恢复
}
```

### await_resume 阶段

```
h.resume()
  → ReadAwaitable::await_resume()
      → if (readWaiterTimedOut_) return ReadResult::Timeout
      → if (conn disconnected) return ReadResult::Closed
      → return ReadResult::Data  // inputBuffer 中有数据
```

## Write Awaitable 流程

```
co_await conn->coWrite(data)
  → WriteAwaitable::await_suspend(handle)
      → conn_->sendInLoop(data)
      → if (outputBuffer empty after send):
          return false;                     // 同步完成，不挂起
      → conn_->armWriteWaiter(handle)
          → writeWaiterHandle_ = handle     // 等待 outputBuffer 清空

handleWrite():
  → write() → outputBuffer 清空
  → resumeWriteWaiterIfNeeded()
      → if (writeWaiterHandle_):
          auto h = std::exchange(writeWaiterHandle_, nullptr);
          queueResume(h);

WriteAwaitable::await_resume()
  → return write result
```

## Close Awaitable 流程

```
co_await conn->coClose()
  → CloseAwaitable::await_suspend(handle)
      → conn_->armCloseWaiter(handle)
      → conn_->shutdown()                  // 发起优雅关闭

handleClose():
  → resumeAllWaitersOnClose()
      → resume readWaiterHandle_
      → resume writeWaiterHandle_
      → resume closeWaiterHandle_

CloseAwaitable::await_resume()
  → return
```

## SleepAwaitable 流程

```
co_await mini::coroutine::sleep(loop, 1.0)
  → SleepAwaitable::await_suspend(handle)
      → loop_->runAfter(seconds, [handle] {
            handle.resume();               // 定时器到期直接恢复
        });
```

## 协程恢复时序图

```
协程 Coroutine           TcpConnection         EventLoop          Kernel
    │                         │                     │                │
    ├─ co_await coRead() ────►│                     │                │
    │  (协程挂起)              │                     │                │
    │                         ├─ armReadWaiter()    │                │
    │                         │                     │                │
    │                         │                     ├─ poll() ──────►│
    │                         │                     │◄── EPOLLIN ────┤
    │                         │◄─ handleRead() ─────┤                │
    │                         │                     │                │
    │                         ├─ resumeReadWaiter() │                │
    │                         │   queueResume(h) ──►│                │
    │                         │                     │                │
    │                         │                     ├─ doPendingFunctors()
    │◄──────────── h.resume() ┼─────────────────────┤
    │  (协程恢复)              │                     │
    ├─ await_resume()         │                     │
    │  → ReadResult::Data     │                     │
```

## 单等待者约束

每个 TcpConnection 同时只能有一个 read waiter 和一个 write waiter：

```cpp
void TcpConnection::armReadWaiter(std::coroutine_handle<> h, ...) {
    assert(!readWaiterHandle_);  // 禁止多个协程同时等待同一个连接的读
    readWaiterHandle_ = h;
}
```

这是设计选择，不是限制：一个连接同时只有一个协程处理读写，与 reactor 的单线程模型一致。

## 连接关闭时的协程恢复

```cpp
void TcpConnection::resumeAllWaitersOnClose() {
    if (readWaiterHandle_) {
        auto h = std::exchange(readWaiterHandle_, nullptr);
        queueResume(h);     // 协程恢复后通过 ReadResult::Closed 感知
    }
    if (writeWaiterHandle_) {
        auto h = std::exchange(writeWaiterHandle_, nullptr);
        queueResume(h);
    }
    if (closeWaiterHandle_) {
        auto h = std::exchange(closeWaiterHandle_, nullptr);
        queueResume(h);
    }
}
```
