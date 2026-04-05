# 潜在 Bug 分析

## 概览

以下是通过静态代码审查发现的潜在 bug 和 race condition，按风险等级排序。

## 高风险

### 1. WhenAllState::firstException 的竞态写入

**位置**: `mini/coroutine/WhenAll.h` — `WhenAllState::captureException`

```cpp
void captureException(std::exception_ptr e) {
    if (!firstException) {          // 非 atomic 读
        firstException = std::move(e);  // 非 atomic 写
    }
}
```

**问题**: 如果多个 wrapper 协程在不同 EventLoop 线程并发完成并抛异常，
两个线程可能同时进入 if 分支 → 数据竞争

**触发条件**: 多个子任务在不同线程执行且同时抛异常

**修复建议**:
```cpp
void captureException(std::exception_ptr e) {
    std::exception_ptr expected{nullptr};
    std::atomic_compare_exchange_strong(&firstException, &expected, std::move(e));
}
```

或者使用 `std::atomic_flag` 做 guard。

### 2. DnsResolver::cacheEnabled_ 数据竞争

**位置**: `mini/net/DnsResolver.h` / `DnsResolver.cc`

```cpp
bool cacheEnabled_{false};  // 非 atomic

// 主线程写:
void enableCache(seconds ttl) { cacheEnabled_ = true; ... }

// 工作线程读:
void workerThread() {
    if (cacheEnabled_ && !cacheAddrs.empty()) { ... }  // 数据竞争
}

// 任意线程读:
void resolve(...) {
    if (cacheEnabled_) { ... }  // 数据竞争
}
```

**修复建议**: 改为 `std::atomic<bool> cacheEnabled_{false}`

### 3. 协程双 waiter 无运行时保护

**位置**: `TcpConnection` 的 `readAwaiter_` / `writeAwaiter_` / `closeAwaiter_`

```cpp
// 如果两个协程同时 co_await conn->asyncReadSome():
// 第二个 await_suspend 会覆盖 readAwaiter_.handle
// 第一个协程的 handle 丢失 → 协程帧泄漏 + 永远不会被 resume
```

**触发条件**: 用户错误使用，同一连接上同时两个协程读

**建议**: 在 await_suspend 中检查 `readAwaiter_.active`，如果已有等待者则 throw 或 assert

## 中等风险

### 4. WhenAny 败者写 wrappers 数组可能竞争

**位置**: `mini/coroutine/WhenAny.h` — `WhenAnyState`

```cpp
template<typename T, size_t N>
struct WhenAnyState {
    Task<void> wrappers[N];  // 败者协程完成时 FinalAwaiter destroy 帧
};
```

**问题**: 败者协程在运行完成后，FinalAwaiter 会 destroy 协程帧。
如果多个败者在不同线程同时完成 → 多个 Task\<void\>::~Task() 并发访问 wrappers 数组中不同元素。
这在语义上是安全的（不同索引），但需要确保 shared_ptr 析构不重入。

**风险**: 低，因为每个 wrapper 独立访问自己的索引

### 5. Connector 自连接检测依赖 getsockname

**位置**: `mini/net/Connector.cc`

```cpp
// 检查是否连接到了自己（同一个 port 自连接）
sockaddr_in localAddr, peerAddr;
getsockname(sockfd, ...);
getpeername(sockfd, ...);
if (localAddr == peerAddr) {
    // 自连接，重试
}
```

**问题**: 如果 connect 到 localhost 且端口被耗尽，可能出现自连接。
检测依赖 getsockname/getpeername，在某些极端时序下可能失效。

**影响**: 极低概率

### 6. TimerQueue cancel 后 reset 的时序

**位置**: `mini/net/TimerQueue.cc`

```cpp
void TimerQueue::reset(const std::vector<TimerPtr>& expired) {
    for (const auto& timer : expired) {
        if (timer->repeat() && !timer->canceled()) {
            // 重新添加
        }
    }
}
```

**问题**: 如果在 getExpired 和 reset 之间，另一个线程（通过 runInLoop）cancel 了一个 repeating timer，
timer->canceled() 在 reset 时可能已经是 true → 正确行为。

**风险**: 无实际问题，但依赖于 runInLoop 的 FIFO 顺序

## 低风险

### 7. Buffer::makeSpace memmove 的 off-by-one

**位置**: `mini/net/Buffer.cc`

```cpp
void Buffer::makeSpace(size_t len) {
    if (writableBytes() + prependableBytes() < len + kCheapPrepend) {
        buffer_.resize(writerIndex_ + len);
    } else {
        const size_t readable = readableBytes();
        std::copy(begin() + readerIndex_, begin() + writerIndex_, begin() + kCheapPrepend);
        readerIndex_ = kCheapPrepend;
        writerIndex_ = readerIndex_ + readable;
    }
}
```

**审查结果**: 逻辑正确。`std::copy` 的源和目标区域不重叠（因为 readerIndex >= kCheapPrepend，
copy 向前移动）。

### 8. EventLoop::quit() 的半异步语义

**位置**: `mini/net/EventLoop.cc`

```cpp
void EventLoop::quit() {
    quit_.store(true, std::memory_order_release);
    if (!isInLoopThread()) {
        wakeup();
    }
}
```

**问题**: 如果在 loop 线程内调用 quit()，不会立即退出（需要等本轮循环结束）。
但 quit_ 是 atomic store，下一轮检查 while (!quit_) 会退出。

**影响**: 符合设计意图，但用户可能误以为 quit 是立即的

### 9. SleepState::resumed 非 atomic

**位置**: `mini/coroutine/SleepAwaitable.h`

```cpp
struct SleepState {
    bool resumed{false};   // 非 atomic
};
```

**分析**: 所有访问都通过 runInLoop 序列化到同一线程 → 安全。
但如果未来重构改变了线程模型 → 可能引入竞争。

**建议**: 如果想防御性编程，改为 atomic<bool>

## 总结

| 风险等级 | 数量 | 需要立即修复 |
|----------|------|-------------|
| 高 | 3 | WhenAll exception race, cacheEnabled_ race, double waiter |
| 中 | 3 | 可接受，但应在 v2 修复 |
| 低 | 3 | 当前实现正确，但值得防御性加固 |
