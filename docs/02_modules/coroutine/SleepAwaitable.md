# SleepAwaitable 源码拆解

## 1. 类定位

* **角色**: 协程层（Coroutine）桥接工具
* **层级**: Coroutine 层，连接 Reactor 层 TimerQueue
* SleepAwaitable 是 **定时器到协程的桥接器**——将 EventLoop::runAfter 的回调语义转换为 co_await 语义

## 2. 解决的问题

* **核心问题**: 让协程可以 `co_await asyncSleep(loop, 100ms)` 而不是注册回调
* 如果没有 SleepAwaitable:
  - 协程中需要延时必须手动注册定时器回调 + 手动 resume 协程句柄
  - 取消逻辑散落在回调链中，容易泄漏协程帧
  - 没有统一的"睡眠是否被取消"返回值

## 3. 对外接口（API）

| 方法 | 场景 | 调用者 |
|------|------|--------|
| `SleepAwaitable(loop, duration)` | 构造 | asyncSleep 工厂 |
| `await_ready()` | 始终返回 false | 协程机制 |
| `await_suspend(handle)` | 注册定时器，保存 handle | 协程机制 |
| `await_resume() → bool` | 返回是否正常完成（false = 被取消） | 协程机制 |
| `cancel()` | 取消定时器并恢复协程 | 用户 / WhenAny |
| `state()` | 获取共享状态（外部取消协调用） | 高级用户 |

### 工厂函数

```cpp
SleepAwaitable asyncSleep(EventLoop* loop, Duration duration);
```

## 4. 核心成员变量

```
SleepAwaitable
├── state_: shared_ptr<SleepState>     // 共享状态，连接 timer 回调和协程
├── duration_: Duration                 // 定时时长
```

### SleepState（共享状态）

```
SleepState
├── loop: EventLoop*                   // 所属 EventLoop
├── handle: coroutine_handle<>         // 挂起的协程句柄
├── timerId: TimerId                   // 定时器 ID，用于取消
├── resumed: bool                      // 是否已恢复（防止 double resume）
├── cancelled: bool                    // 是否被取消
```

**线程安全**: SleepState 通过 `runInLoop` 保证所有操作都在 owner loop 线程执行，无需锁

## 5. 执行流程

### 5.1 正常完成路径

```
co_await asyncSleep(loop, 100ms)
  → SleepAwaitable 构造，创建 SleepState
  → await_ready() → false
  → await_suspend(handle):
    → state_->handle = handle
    → loop->runAfter(duration, callback)
      → callback 捕获 shared_ptr<SleepState>
    → 协程挂起
  → ... 100ms 后 ...
  → TimerQueue 触发
    → callback():
      → if (!state->resumed)
        → state->resumed = true
        → state->handle.resume()    // 在 owner loop 线程
  → await_resume() → true（正常完成）
```

### 5.2 取消路径

```
sleep.cancel()
  → loop->runInLoop([state] {       // 确保在 owner loop 线程
      → if (state->resumed) return   // 已经触发，无操作
      → state->resumed = true
      → state->cancelled = true
      → loop->cancel(state->timerId) // 取消定时器
      → state->handle.resume()       // 恢复协程，避免句柄泄漏
    })
  → await_resume() → false（被取消）
```

### 5.3 调用链图

```
  ┌──────────┐
  │ 协程体   │  co_await asyncSleep(loop, 100ms)
  └────┬─────┘
       │ await_suspend
       ▼
  ┌──────────────────┐       runAfter        ┌────────────┐
  │ SleepAwaitable   │─────────────────────▶ │ TimerQueue │
  │                  │                        └─────┬──────┘
  │  SleepState      │◀─── timer callback ──────────┘
  │  {handle,resumed}│
  └────────┬─────────┘
           │ handle.resume()
           ▼
  ┌──────────┐
  │ 协程体   │  await_resume() → true/false
  └──────────┘
```

## 6. 关键交互关系

```
┌──────────┐     co_await      ┌─────────────────┐
│ Task<T>  │──────────────────▶│ SleepAwaitable  │
│ 协程体   │◀──────────────────│                 │
└──────────┘    resume         └────────┬────────┘
                                        │ runAfter
                                        ▼
                               ┌─────────────────┐
                               │   EventLoop     │
                               │   TimerQueue    │
                               └─────────────────┘
```

* **上游**: 任何 Task\<T\> 协程体内均可 co_await
* **下游**: 依赖 EventLoop::runAfter / cancel
* **取消**: WhenAny 等组合器可通过 cancel() 取消等待中的 sleep

## 7. 关键设计点

### 7.1 shared_ptr 共享状态

* **为什么**: timer 回调和 cancel() 可能并发访问 SleepState
* shared_ptr 确保无论谁先完成，状态都不会被销毁
* `resumed` 标志确保 exactly-once resume

### 7.2 cancel() 通过 runInLoop 序列化

```cpp
void cancel() {
    state->loop->runInLoop([state] {
        if (state->resumed) return;  // 防止 double resume
        state->resumed = true;
        state->cancelled = true;
        state->loop->cancel(state->timerId);
        state->handle.resume();
    });
}
```

* 所有状态修改都在 owner loop 线程
* 即使从其他线程调用 cancel()，也通过 runInLoop 投递到正确线程

### 7.3 cancel 后必须 resume

* 取消时仍然 `handle.resume()`，因为协程帧处于挂起状态
* 如果不 resume，协程帧永远不会运行到 final_suspend → 内存泄漏
* `await_resume()` 返回 false 通知协程体"被取消了"

## 8. 潜在问题

| 问题 | 描述 | 严重程度 |
|------|------|----------|
| resumed 非 atomic | SleepState::resumed 是 bool 而非 atomic<bool>，但所有访问都在同一线程 | 低（设计保证） |
| cancel 后 await_resume 语义 | 调用者需要检查返回值判断是否被取消 | 低（接口清晰） |
| loop 销毁后 cancel | 如果 EventLoop 已销毁再调用 cancel → UB | 中（生命周期约束） |

## 9. 极简实现骨架

```cpp
class SleepAwaitable {
    struct State {
        EventLoop* loop;
        std::coroutine_handle<> handle;
        TimerId timerId;
        bool resumed = false, cancelled = false;
    };
    std::shared_ptr<State> state_;
    Duration duration_;

public:
    SleepAwaitable(EventLoop* loop, Duration d)
        : state_(std::make_shared<State>()), duration_(d) {
        state_->loop = loop;
    }

    bool await_ready() { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        state_->handle = h;
        auto s = state_;
        state_->timerId = state_->loop->runAfter(duration_, [s] {
            if (!s->resumed) { s->resumed = true; s->handle.resume(); }
        });
    }

    bool await_resume() { return !state_->cancelled; }

    void cancel() {
        auto s = state_;
        s->loop->runInLoop([s] {
            if (s->resumed) return;
            s->resumed = s->cancelled = true;
            s->loop->cancel(s->timerId);
            s->handle.resume();
        });
    }
};
```

## 10. 面试角度总结

| 问题 | 答案要点 |
|------|----------|
| 为什么用 shared_ptr 包装状态？ | timer 回调和 cancel 可能并发访问，shared_ptr 保证状态存活 |
| 为什么 cancel 必须 resume？ | 挂起的协程帧不 resume 就永远不会运行到 final_suspend，导致内存泄漏 |
| 如何保证 exactly-once resume？ | resumed 标志位 + 所有访问通过 runInLoop 序列化到同一线程 |
| 这个设计和 Reactor 的关系？ | 不绕过 EventLoop，timer 回调和 cancel 都在 owner loop 线程执行 |
