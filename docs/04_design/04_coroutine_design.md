# 协程设计分析

## 1. 设计意图

mini-trantor 的协程层目标是：**在不破坏 Reactor 调度语义的前提下，提供 co_await 风格的异步 API**。

核心约束：
- 协程恢复必须在 owner loop 线程
- 不绕过 EventLoop，不引入独立调度器
- 回调和协程可以共存

## 2. 协程层架构

```
┌─────────────────────────────────────────────┐
│                协程层 (Coroutine)             │
│                                             │
│  ┌──────────┐  ┌──────────────────────────┐ │
│  │ Task<T>  │  │  组合器                  │ │
│  │ (协程帧  │  │  WhenAll / WhenAny       │ │
│  │  管理器) │  │  (纯协程层，不依赖 Net)  │ │
│  └──────────┘  └──────────────────────────┘ │
│                                             │
│  ┌──────────────────────────────────────┐   │
│  │  桥接 Awaitable                      │   │
│  │  ReadAwaitable / WriteAwaitable      │   │
│  │  CloseAwaitable / SleepAwaitable     │   │
│  │  ResolveAwaitable                    │   │
│  │  (连接 Reactor 层和协程层)           │   │
│  └──────────────────────────────────────┘   │
└──────────────────────┬──────────────────────┘
                       │ 桥接
                       ▼
┌─────────────────────────────────────────────┐
│              Reactor / Net 层               │
│   EventLoop / Channel / TcpConnection       │
└─────────────────────────────────────────────┘
```

## 3. 关键设计决策

### 3.1 Lazy 协程 vs Eager 协程

**选择: Lazy（initial_suspend = always）**

原因：
- 允许父协程在 co_await 前设置 continuation
- 避免子协程在 continuation 设置前就跑完（竞态）
- 控制启动时机（detach 模式需要显式 start）

```
Lazy:   创建 → 挂起 → 调用者设置 continuation → resume → 执行
Eager:  创建 → 立即执行 → 如果跑完了 continuation 还没设置 → 问题
```

### 3.2 对称转移 vs 非对称 resume

**选择: 对称转移**

```cpp
// 非对称（不采用）
void await_suspend(coroutine_handle<> parent) {
    promise.continuation_ = parent;
    childHandle.resume();  // 递归深度 +1
}

// 对称转移（采用）
auto await_suspend(coroutine_handle<> parent) {
    promise.continuation_ = parent;
    return childHandle;    // 编译器通过 tail-call 转移，不增加栈深度
}
```

对称转移避免深度嵌套 co_await 链导致栈溢出。

### 3.3 Awaitable 桥接模式

**设计**: 每个桥接 Awaitable 都遵循相同模式：

```
await_suspend(handle):
  1. 保存 coroutine_handle 到某个状态
  2. 注册回调到 Reactor（runAfter / handleRead / resolve）
  3. 协程挂起

Reactor 回调触发时:
  1. 更新结果数据
  2. queueInLoop(handle.resume)  // 不直接 resume，投递到 loop

await_resume():
  1. 返回结果数据
```

**关键**: 使用 `queueInLoop` 而不是直接 `handle.resume()`

原因：
```
如果在 handleRead 中直接 resume:
  resume → 协程体中做 asyncWrite → sendInLoop
  → 如果 sendInLoop 需要 enableWriting → updateChannel
  → 此时还在 handleRead 的栈帧中
  → 重入问题

使用 queueInLoop:
  handleRead 正常返回 → Phase 3 执行 resume
  → 协程体操作不会重入 Phase 2 的事件处理
```

### 3.4 单等待者约束

```
TcpConnection 内部:
  readAwaiter_:  { handle, minBytes, active }  // 只有一个 slot
  writeAwaiter_: { handle, active }
  closeAwaiter_: { handle, active }
```

**设计选择**: 不支持多个协程同时 co_await 同一个 TcpConnection 的 read/write

原因：
- 简化实现（无需队列/公平调度）
- 网络编程中极少需要多个协程读同一个连接
- 如需多路复用，用 WhenAny 在协程层组合

### 3.5 组合器不依赖 EventLoop

WhenAll 和 WhenAny 是**纯协程层构造**：
- 不引用 EventLoop
- 通过 atomic 计数器 / CAS 同步
- 通过 wrapper 协程 + detach 管理子任务

**好处**: 组合器可以在任意 C++20 环境中使用，不绑定 Reactor

## 4. 协程与回调的共存

```cpp
// 回调模式
server.setMessageCallback([](auto conn, auto* buf, auto ts) {
    conn->send(buf->retrieveAllAsString());
});

// 协程模式
server.setCoroutineHandler([](TcpConnectionPtr conn) -> Task<void> {
    while (true) {
        auto data = co_await conn->asyncReadSome();
        if (data.empty()) break;
        co_await conn->asyncWrite(data);
    }
});
```

**共存机制**: TcpServer 根据是否设置了 coroutineHandler 选择模式
- 有 coroutineHandler: 新连接启动协程 session
- 无 coroutineHandler: 走传统回调路径

两种模式不能混用于同一个连接。

## 5. 生命周期与内存管理

```
协程帧生命周期:
  ┌─────────────────────────────────┐
  │ Task<T> 拥有 coroutine_handle   │
  │                                 │
  │ co_await 时:                    │
  │   所有权转移到 Awaiter          │
  │   FinalAwaiter 负责续接/销毁    │
  │                                 │
  │ detach 时:                      │
  │   Task 放弃所有权               │
  │   FinalAwaiter 在 final_suspend │
  │   自动 destroy 协程帧           │
  └─────────────────────────────────┘
```

### Awaitable 如何延长连接生命

```
ReadAwaitable 持有 TcpConnectionPtr (shared_ptr)
  → 协程挂起期间连接不会被析构
  → 连接关闭时 resumeAllWaitersOnClose 恢复协程
  → 协程结束 → Awaitable 析构 → shared_ptr 释放
```

## 6. WhenAny 中败者的命运

```
co_await whenAny(taskA, taskB)
  │
  ├─ taskA 先完成（赢家）→ resume 父协程
  │
  └─ taskB 仍在运行（败者）
     → 继续运行直到 co_return
     → FinalAwaiter 因 detached_ = true 自动 destroy
     → 败者的结果被丢弃
```

**注意**: 败者不会被取消。如果需要取消：
- SleepAwaitable: 调用 cancel()
- ReadAwaitable: 关闭连接
- 通用方案: 引入 cancellation token（未实现）

## 7. 未来扩展

| 扩展 | 难度 | 说明 |
|------|------|------|
| cancellation token | 中 | 允许取消挂起的协程 |
| async generator | 中 | co_yield 下的流式处理 |
| coroutine pool | 低 | 复用协程帧避免频繁分配 |
| awaitable pipeline | 高 | 类似 ranges 的 awaitable 组合 |
