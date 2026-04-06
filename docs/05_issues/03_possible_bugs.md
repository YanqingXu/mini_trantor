# 潜在 Bug 分析

## 概览

以下是通过静态代码审查发现的潜在 bug 和 race condition，按风险等级排序。

## 高风险

### 1. ~~WhenAllState::firstException 的竞态写入~~ ✅ 已修复

**位置**: `mini/coroutine/WhenAll.h` — `WhenAllState::captureException`

**修复方案**: 引入 `std::atomic<bool> exceptionCaptured{false}`，在 `captureException()` 中使用 `compare_exchange_strong`（memory order `acq_rel`）保证只有第一个异常被捕获。`WhenAllState` 和 `WhenAllVoidState` 均采用此模式。`remaining` 计数器也改为 `std::atomic<std::size_t>`。

**状态**: 已修复，无数据竞争风险。

### 2. ~~DnsResolver::cacheEnabled_ 数据竞争~~ ✅ 已修复

**位置**: `mini/net/DnsResolver.h` / `DnsResolver.cc`

**修复方案**: `cacheEnabled_` 已改为 `std::atomic<bool> cacheEnabled_{false}`，跨线程读写安全。

**状态**: 已修复，无数据竞争风险。

### 3. ~~协程双 waiter 无运行时保护~~ ✅ 已修复

**位置**: `TcpConnection` 的 `readAwaiter_` / `writeAwaiter_` / `closeAwaiter_`

**修复方案**: `ReadAwaiterState`、`WriteAwaiterState`、`CloseAwaiterState` 中均添加了 `bool active{false}` 标志。`armReadWaiter`、`armWriteWaiter`、`armCloseWaiter` 在 arm 时检查该标志，若已有等待者则抛出 `std::logic_error`。所有检查在 owning EventLoop 线程上执行（通过 `assertInLoopThread()` 保证），无需 atomic。

**状态**: 已修复，重复 await 将抛出明确异常。

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

| 风险等级 | 数量 | 状态 |
|----------|------|------|
| 高 | 3 | ✅ 全部已修复（WhenAll atomic CAS, cacheEnabled_ atomic, double waiter throw） |
| 中 | 3 | 可接受，但应在 v2 修复 |
| 低 | 3 | 当前实现正确，但值得防御性加固 |
