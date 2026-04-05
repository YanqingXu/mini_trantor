# Awaitable 协程桥接类族 源码拆解

## 1. 类定位

* **角色**: 协程层（Coroutine）与 Net 层的桥接适配器
* **层级**: 横跨 Coroutine 层 和 Net 层
* Awaitable 类族包括 TcpConnection 内嵌的三个 Awaitable 和两个独立 Awaitable：
  - `TcpConnection::ReadAwaitable` — 异步读桥接
  - `TcpConnection::WriteAwaitable` — 异步写桥接
  - `TcpConnection::CloseAwaitable` — 等待关闭桥接
  - `SleepAwaitable` — 定时器桥接（详见 [SleepAwaitable.md](SleepAwaitable.md)）
  - `ResolveAwaitable` — DNS 解析桥接

本文聚焦 **TcpConnection 的三个内嵌 Awaitable** 和 **ResolveAwaitable**。

## 2. 解决的问题

* **核心问题**: 将 Reactor 回调语义（handleRead/handleWrite/onClose）转换为协程 co_await 语义
* 如果没有 Awaitable:
  - 协程中做网络 I/O 需要手动保存 coroutine_handle，手动在回调中 resume
  - 缺少 "单等待者约束" 的编译/运行时保护
  - 回调和协程生命周期交织导致难以维护

## 3. TcpConnection 内嵌 Awaitable

### 3.1 ReadAwaitable

```cpp
class ReadAwaitable {
    TcpConnectionPtr connection_;
    size_t minBytes_;
public:
    ReadAwaitable(TcpConnectionPtr conn, size_t minBytes);
    bool await_ready() const noexcept;    // inputBuffer 已有足够数据？
    void await_suspend(coroutine_handle<> h);  // 设置 readAwaiter_.handle
    string await_resume();                // 提取 inputBuffer 全部内容
};
```

**调用链**:
```
co_await conn->asyncReadSome(minBytes)
  → await_ready(): inputBuffer_.readableBytes() >= minBytes? YES→不挂起
  → await_suspend(h): readAwaiter_ = {h, minBytes, active=true}
  → ... EPOLLIN 触发 ...
  → handleRead():
    → readFd() 读入 inputBuffer_
    → if readAwaiter_.active && readable >= minBytes:
      → readAwaiter_.active = false
      → queueResume(readAwaiter_.handle)  // queueInLoop 投递 resume
  → await_resume(): return inputBuffer_.retrieveAllAsString()
```

### 3.2 WriteAwaitable

```cpp
class WriteAwaitable {
    TcpConnectionPtr connection_;
    string data_;
public:
    WriteAwaitable(TcpConnectionPtr conn, string data);
    bool await_ready() const noexcept;    // 始终 false
    void await_suspend(coroutine_handle<> h);  // sendInLoop + 设置 writeAwaiter_
    void await_resume() const;
};
```

**调用链**:
```
co_await conn->asyncWrite(data)
  → await_ready(): false
  → await_suspend(h):
    → writeAwaiter_ = {h, active=true}
    → sendInLoop(data)  // 尝试直接写
    → 如果 outputBuffer_ 为空（写完了）:
      → writeAwaiter_.active = false
      → queueResume(h)              // 立即恢复
    → 否则等 handleWrite 完成
  → handleWrite():
    → write() 输出缓冲区
    → if outputBuffer_ empty && writeAwaiter_.active:
      → writeAwaiter_.active = false
      → queueResume(writeAwaiter_.handle)
  → await_resume(): void
```

### 3.3 CloseAwaitable

```cpp
class CloseAwaitable {
    TcpConnectionPtr connection_;
public:
    explicit CloseAwaitable(TcpConnectionPtr conn);
    bool await_ready() const noexcept;    // state_ == kDisconnected?
    void await_suspend(coroutine_handle<> h);  // 设置 closeAwaiter_
    void await_resume() const noexcept;
};
```

**调用链**:
```
co_await conn->waitClosed()
  → await_ready(): state_ == kDisconnected? YES→不挂起
  → await_suspend(h): closeAwaiter_ = {h, active=true}
  → ... 对端关闭 / shutdown ...
  → handleClose():
    → resumeAllWaitersOnClose()       // 恢复所有等待者
      → readAwaiter_.active → queueResume
      → writeAwaiter_.active → queueResume
      → closeAwaiter_.active → queueResume
  → await_resume(): void
```

## 4. ResolveAwaitable

```cpp
class ResolveAwaitable {
    shared_ptr<DnsResolver> resolver_;
    shared_ptr<ResolveState> state_;
    string hostname_;
    uint16_t port_;
};
```

### ResolveState

```
ResolveState
├── loop: EventLoop*                   // 回调投递目标
├── handle: coroutine_handle<>         // 挂起的协程
├── result: vector<InetAddress>        // 解析结果
├── resumed: bool                      // 防止 double resume
```

**调用链**:
```
co_await asyncResolve(resolver, loop, "example.com", 80)
  → await_ready(): false
  → await_suspend(h):
    → state->handle = h
    → resolver->resolve(hostname, port, loop, callback)
      → callback 在 DnsResolver worker 线程执行
      → 通过 loop->runInLoop 投递到 owner loop 线程
      → callback:
        → state->result = addrs
        → state->handle.resume()      // 在 owner loop 线程
  → await_resume(): return move(state->result)
```

## 5. 核心设计模式

### 5.1 单等待者约束（Single-Waiter）

```
TcpConnection 内部:
├── readAwaiter_:  { handle, minBytes, active }
├── writeAwaiter_: { handle, active }
├── closeAwaiter_: { handle, active }
```

* 每种 awaiter 只有一个 slot（不是队列）
* 同一时间只允许一个协程 co_await asyncReadSome / asyncWrite / waitClosed
* 违反此约束（两个协程同时 co_await read）会覆盖前一个 handle → UB

### 5.2 queueResume 投递

```cpp
void TcpConnection::queueResume(coroutine_handle<> h) {
    loop_->queueInLoop([h] { h.resume(); });
}
```

* **不直接 resume**，而是投递到 EventLoop 的 pendingFunctors
* **原因**: resume 可能触发协程链中的 I/O 操作，必须在 loop 线程
* 避免在 handleRead 中 resume → 协程做 write → 重入 handleWrite

### 5.3 resumeAllWaitersOnClose

```cpp
void TcpConnection::resumeAllWaitersOnClose() {
    if (readAwaiter_.active) {
        readAwaiter_.active = false;
        queueResume(readAwaiter_.handle);
    }
    if (writeAwaiter_.active) {
        writeAwaiter_.active = false;
        queueResume(writeAwaiter_.handle);
    }
    if (closeAwaiter_.active) {
        closeAwaiter_.active = false;
        queueResume(closeAwaiter_.handle);
    }
}
```

* 连接关闭时恢复所有挂起的协程
* ReadAwaitable 恢复后 await_resume 返回空字符串（或部分数据）
* 协程体可通过检查返回值 + 连接状态判断是否是正常结束

## 6. 关键交互关系

```
┌────────────────┐  asyncReadSome    ┌──────────────────┐
│ 协程体         │──────────────────▶│ ReadAwaitable    │
│ (Task<T>)      │  asyncWrite       │ WriteAwaitable   │
│                │──────────────────▶│ CloseAwaitable   │
│                │◀─ resume ─────────│                  │
└────────────────┘                   └────────┬─────────┘
                                              │ 委托
                                              ▼
                                     ┌─────────────────┐
                                     │ TcpConnection   │
                                     │ handleRead()    │
                                     │ handleWrite()   │
                                     │ handleClose()   │
                                     └────────┬────────┘
                                              │ queueResume
                                              ▼
                                     ┌─────────────────┐
                                     │  EventLoop      │
                                     │  queueInLoop    │
                                     └─────────────────┘
```

## 7. 与 SleepAwaitable/ResolveAwaitable 的对比

| 特性 | Read/Write/Close Awaitable | SleepAwaitable | ResolveAwaitable |
|------|---------------------------|----------------|------------------|
| 状态存储 | TcpConnection 内嵌 struct | shared_ptr\<SleepState\> | shared_ptr\<ResolveState\> |
| 触发源 | epoll 事件 | timerfd 到期 | DnsResolver worker 线程 |
| 可取消 | 否（连接关闭时自动恢复） | 是（cancel()） | 否 |
| 线程安全 | owner loop 单线程 | runInLoop 序列化 | runInLoop 序列化 |
| 多等待者 | 不支持（单 slot） | N/A（一次性） | N/A（一次性） |

## 8. 潜在问题

| 问题 | 描述 | 严重程度 |
|------|------|----------|
| 单等待者无运行时检查 | 两个协程同时 co_await read 会静默覆盖 handle | 高 |
| 连接关闭后 Read 返回空 | 调用者需检查返回值，否则可能误解为"读到空数据" | 中 |
| ResolveAwaitable 不可取消 | DNS 解析可能阻塞很久，无法中断 | 低 |

## 9. 面试角度总结

| 问题 | 答案要点 |
|------|----------|
| Awaitable 如何桥接 Reactor 和协程？ | 在 await_suspend 中注册回调/状态，在 Reactor 回调中 queueResume 恢复协程 |
| 为什么用 queueResume 而不是直接 resume？ | 避免在回调中 resume 导致重入（协程可能做 I/O → 触发另一个回调） |
| 单等待者约束的意义？ | 简化实现，避免多等待者时的公平性和唤醒顺序问题 |
| 连接关闭时如何通知协程？ | resumeAllWaitersOnClose 恢复所有挂起的协程，协程通过返回值/状态判断 |
