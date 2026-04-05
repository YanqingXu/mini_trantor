# 协程模型

## 设计哲学

mini-trantor 的协程设计遵循一个核心原则：**协程不替代 Reactor，协程桥接 Reactor**。

这意味着：
- 所有协程的恢复（resume）都通过 `EventLoop::queueInLoop()` 投递
- 协程执行在正确的 loop 线程上，不违反 thread-affinity
- 协程是让用户代码更"线性"的语法糖，底层调度语义不变

## 核心组件

### Task\<T\> —— 协程结果对象

```cpp
mini::coroutine::Task<int> computeAsync() {
    co_return 42;
}

// 使用方式一：start + result
auto task = computeAsync();
task.start();   // 开始执行
int v = task.result();  // 获取结果

// 使用方式二：co_await 组合
Task<void> caller() {
    int v = co_await computeAsync();  // 挂起等待子任务
}

// 使用方式三：detach（fire-and-forget）
computeAsync().detach();
```

Task 的实现要点：
- `initial_suspend` 返回 `suspend_always` —— 惰性启动，不创建就执行
- `final_suspend` 中检查 continuation —— 子任务完成时恢复父协程
- detach 模式下，`final_suspend` 中自动 destroy

### SleepAwaitable —— 定时器桥接

```cpp
Task<void> example(EventLoop* loop) {
    bool completed = co_await asyncSleep(loop, 100ms);
    // completed == true: 正常超时
    // completed == false: 被 cancel()
}
```

实现原理：
1. `await_suspend` 时注册 `EventLoop::runAfter()` 定时器
2. 定时器到期时在 loop 线程 `handle.resume()`
3. cancel 时取消定时器并立即恢复协程

### TcpConnection Awaitables —— I/O 桥接

```cpp
Task<void> handleConnection(TcpConnectionPtr conn) {
    // 异步读
    std::string data = co_await conn->asyncReadSome(1);

    // 异步写（等待写完成）
    co_await conn->asyncWrite("response");

    // 等待连接关闭
    co_await conn->waitClosed();
}
```

三种 awaitable：

| Awaitable | 挂起条件 | 恢复条件 |
|-----------|---------|---------|
| `ReadAwaitable` | inputBuffer 数据不足 minBytes | 数据到达或连接关闭 |
| `WriteAwaitable` | 数据未发送完毕 | outputBuffer 清空 |
| `CloseAwaitable` | 连接未断开 | 连接状态变为 kDisconnected |

实现要点：
- 每种 waiter 同一时刻最多一个（armXxxWaiter 检查冲突）
- 恢复通过 `EventLoop::queueInLoop()` 而非直接 `handle.resume()`，确保线程正确性
- 连接关闭时 `resumeAllWaitersOnClose()` 清理所有挂起的协程

### WhenAll —— 并行等待全部完成

```cpp
Task<void> parallel(EventLoop* loop) {
    auto [a, b] = co_await whenAll(
        taskReturningInt(),     // Task<int>
        taskReturningString()   // Task<std::string>
    );
    // a: int, b: std::string
}
```

实现原理：
1. 为每个子任务创建包装协程（wrapper coroutine）
2. 共享 `WhenAllState`，包含 `atomic<size_t> remaining`
3. 每个包装协程完成时 `remaining.fetch_sub(1)`
4. 最后一个完成时（remaining == 0）恢复父协程

### WhenAny —— 竞争等待首个完成

```cpp
Task<void> raceExample(EventLoop* loop) {
    auto [index, result] = co_await whenAny(
        asyncSleep(loop, 100ms),
        asyncSleep(loop, 200ms)
    );
    // index: 首个完成的任务索引
    // result: variant<bool, bool> 结果
}
```

实现原理：
1. 共享 `WhenAnyState`，包含 `atomic<bool> done`
2. 首个完成的子任务设置 `done = true` 并恢复父协程
3. 后续完成的子任务发现 `done == true`，不做任何操作

## 协程与回调的共存

在 TcpConnection 中，协程和回调可以共存（但同一连接上通常选择一种风格）：

```
回调模式：
  TcpServer → setMessageCallback → 用户回调函数

协程模式：
  coroutine → co_await conn->asyncReadSome() → 内部通过回调驱动恢复
```

协程模式下，`handleRead()` 中的逻辑：
```cpp
void TcpConnection::handleRead(Timestamp t) {
    ssize_t n = inputBuffer_.readFd(fd, &savedErrno);
    if (n > 0) {
        resumeReadWaiterIfNeeded();        // 优先恢复协程 waiter
        if (messageCallback_ && !hasReadWaiter) {
            messageCallback_(shared_from_this(), &inputBuffer_);  // 否则走回调
        }
    }
}
```

## 线程安全保证

```
arm*Waiter() 流程：

  任意线程调用 co_await conn->asyncReadSome()
       │
       ▼
  await_suspend() → armReadWaiter(handle, minBytes)
       │
       ├── isInLoopThread? → 直接设置 waiter
       │
       └── 否 → loop_->queueInLoop([设置 waiter])
                     │
                     ▼
              wakeup eventfd → loop 线程执行设置
```

恢复也是通过 `queueInLoop`：
```cpp
void TcpConnection::queueResume(std::coroutine_handle<> handle) {
    loop_->queueInLoop([handle] { handle.resume(); });
    // 始终在 loop 线程恢复，保持 thread-affinity
}
```

## 协程生命周期注意事项

1. **不要在协程中持有裸指针到 stack 对象** —— 协程可能跨越多次 loop 迭代
2. **detach 的协程自动销毁** —— `final_suspend` 中 `handle.destroy()`
3. **Task 析构时销毁未完成的协程** —— RAII 语义，不会泄漏
4. **每个连接同时只能有一个 ReadAwaitable** —— 多个 co_await 会 throw
