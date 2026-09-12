# WhenAny 源码拆解

## 1. 类定位

* **角色**: 协程层（Coroutine）结构化并发原语
* **层级**: 纯协程层，不依赖 EventLoop / Reactor
* WhenAny 是 **竞速等待任一完成** 的组合器——等待 N 个同类型 Task 中第一个完成后恢复父协程

## 2. 解决的问题

* **核心问题**: 多个异步操作竞速，取第一个完成的结果
* 如果没有 WhenAny:
  - 需要手动维护 "是否已有赢家" 的原子标志
  - 败者协程的生命周期管理复杂
  - 超时模式（操作 vs 定时器竞速）需要手工编排

典型用例:
```cpp
// 超时模式：操作 vs 定时器竞速
auto result = co_await whenAny(fetchData(), timeout(5s));
if (result.index == 1) { /* 超时 */ }
```

## 3. 对外接口（API）

| 函数 | 签名 | 场景 |
|------|------|------|
| `whenAny(Task<T>, Task<T>...)` | `Task<WhenAnyResult<T>>` | 同类型值 Task 竞速 |
| `whenAny(Task<void>...)` | `Task<WhenAnyResult<void>>` | void Task 竞速 |

### 返回类型

```cpp
template<typename T>
struct WhenAnyResult {
    size_t index;   // 赢家的索引（0-based）
    T value;        // 赢家的返回值
};

template<>
struct WhenAnyResult<void> {
    size_t index;   // 赢家的索引
};
```

## 4. 核心成员变量

### WhenAnyState\<T, N\>

```
WhenAnyState<T, N>
├── done: atomic<bool>                  // 是否已有赢家
├── parent: coroutine_handle<>          // 父协程句柄
├── winnerIndex: size_t                 // 赢家索引
├── winnerValue: optional<T>            // 赢家返回值
├── winnerException: exception_ptr      // 赢家异常
├── wrappers: Task<void>[N]             // wrapper 协程数组（存在 state 中！）
```

### 关键设计：wrappers 存在 State 中

* 与 WhenAll 不同，WhenAny 的 wrappers 存在共享状态中
* **原因**: 赢家 resume 父协程后，Awaitable 被销毁。如果 wrappers 在 Awaitable 中，败者协程会访问已销毁的 Task\<void\>
* 存在 shared_ptr<State> 中，确保败者协程仍能安全运行完成

## 5. 执行流程

### 5.1 完整调用链

```
co_await whenAny(taskA, taskB, taskC)
  │
  ├─ 1. whenAny() 是协程函数
  │     → co_return co_await WhenAnyAwaitable(move(tasks)...)
  │
  ├─ 2. WhenAnyAwaitable 构造:
  │     → 创建 WhenAnyState(done=false)
  │     → 创建 wrapper 协程存入 state->wrappers[]:
  │        state->wrappers[0] = whenAnyValueWrapper(taskA, 0, state)
  │        state->wrappers[1] = whenAnyValueWrapper(taskB, 1, state)
  │        state->wrappers[2] = whenAnyValueWrapper(taskC, 2, state)
  │
  ├─ 3. await_suspend(parentHandle):
  │     → state->parent = parentHandle
  │     → for each wrapper: wrapper.detach()
  │
  ├─ 4. 每个 wrapper 协程执行:
  │     whenAnyValueWrapper<T, N>(subtask, index, state):
  │       → auto val = co_await move(subtask)
  │       → if (state->tryWin(index)):         // CAS 竞争
  │           → state->winnerValue = val
  │           → state->resumeParent()           // 恢复父协程
  │       → else: // 败者，丢弃结果
  │
  ├─ 5. tryWin() 原子竞争:
  │     → done.compare_exchange_strong(false, true)
  │       → 成功: 设为赢家
  │       → 失败: 已有赢家，静默结束
  │
  └─ 6. await_resume():
        → 检查 winnerException，有异常则 rethrow
        → 返回 WhenAnyResult<T>{winnerIndex, move(winnerValue)}
```

### 5.2 竞争处理

```
     wrapper[0]           wrapper[1]           wrapper[2]
         │                    │                    │
    co_await taskA       co_await taskB       co_await taskC
         │                    │                    │
    (taskB 先完成)            │                    │
         │               tryWin(1)                 │
         │               CAS: false→true ✓         │
         │               winnerValue = val         │
         │               resumeParent()            │
         │                    │                    │
    (taskA 后完成)            │              (taskC 后完成)
    tryWin(0)                 │              tryWin(2)
    CAS: true→true ✗          │              CAS: true→true ✗
    丢弃结果                   │              丢弃结果
```

### 5.3 异常处理

```cpp
// wrapper 中
try {
    auto val = co_await subtask;
    if (state->tryWin(index)) {
        state->winnerValue = val;
        state->resumeParent();
    }
} catch (...) {
    if (state->tryWin(index)) {
        state->winnerException = current_exception();
        state->resumeParent();
    }
    // 败者的异常被静默丢弃
}
```

* 如果第一个完成的子任务抛异常，该异常成为"赢家异常"
* 败者的异常被丢弃（无论成功还是失败）

## 6. 关键交互关系

```
┌────────────┐  co_await whenAny   ┌───────────────────┐
│ 父协程     │─────────────────────▶│ WhenAnyAwaitable │
│            │◀─────────────────────│                  │
└────────────┘  first complete      └────────┬─────────┘
                                             │ detach
                                             ▼
                                    ┌─────────────────┐
                                    │ Wrapper[0..N-1] │
                                    │ CAS 竞争 tryWin │
                                    └────────┬────────┘
                                             │ co_await
                                             ▼
                ┌───────────┬────────────────┐
                │ Task<T>   │    Task<T>     │  ... 同类型
                └───────────┴────────────────┘
```

* **约束**: 所有子 Task 必须是同一类型 T（编译期检查 `same_as<T, Rest> && ...`）
* **与 WhenAll 区别**: WhenAny 只要一个完成就恢复；当前实现会向败者发出协作式取消请求，败者在自己的挂起点观察取消并完成清理

## 7. 关键设计点

### 7.1 CAS 原子竞争

```cpp
bool tryWin(size_t index) {
    bool expected = false;
    if (done.compare_exchange_strong(expected, true, memory_order_acq_rel)) {
        winnerIndex = index;
        return true;
    }
    return false;
}
```

* 只有一个 wrapper 能 CAS 成功
* `memory_order_acq_rel` 确保赢家写入的 value/exception 对 parent 可见

### 7.2 Wrappers 在 State 中的生命周期

```cpp
template<typename T, size_t N>
struct WhenAnyState {
    // ...
    Task<void> wrappers[N];  // 关键：wrappers 在 shared state 中
};
```

* 赢家 resume 父协程 → 父协程运行 → Awaitable 被销毁
* 败者仍在运行（可能还在 co_await 子任务）
* 如果 wrappers 在 Awaitable 中，败者协程的 Task\<void\> 已被析构 → UB
* 存在 shared_ptr<State> 中，败者安全运行到结束

### 7.3 败者的命运

* 赢家确定后，WhenAny 会向败者发出协作式取消请求
* 如果败者 awaitable 支持当前 token（如 `SleepAwaitable`、`TcpConnection` awaitable），会尽快以显式取消结果恢复
* 如果败者已经完成或尚未消费 token，取消请求是安全 no-op，最终结果仍会被丢弃
* 协程帧通过 detach 模式由 FinalAwaiter 自动销毁，shared state 负责把 wrapper 活到清理完成

### 7.4 同类型约束

```cpp
template <typename T, typename... Rest>
requires(sizeof...(Rest) >= 0 && (same_as<T, Rest> && ...) && !is_void_v<T>)
Task<WhenAnyResult<T>> whenAny(Task<T> first, Task<Rest>... rest);
```

* 所有 Task 必须返回相同类型 T
* 因为 winnerValue 只有一个 `optional<T>`
* 如需异类型，用户需自行包装为统一类型

## 8. 潜在问题

| 问题 | 描述 | 严重程度 |
|------|------|----------|
| 协作式取消依赖 awaitable 配合 | 败者是否能尽快退出，取决于子任务 awaitable 是否消费当前 token | 中（设计限制） |
| 败者异常被静默丢弃 | 败者抛出的异常无人处理 | 低（设计选择） |
| 同类型限制 | 不支持 whenAny(Task\<int\>, Task\<string\>) | 低（提供 variant 包装即可） |
| 败者清理仍可能延后 | 即使收到取消请求，败者也可能在自己的下一次挂起点才完成清理 | 中（协作式取消特性） |

## 9. 极简实现骨架

```cpp
template<typename T>
struct WhenAnyResult { size_t index; T value; };

template<typename T, typename... Rest>
Task<WhenAnyResult<T>> whenAny(Task<T> first, Task<Rest>... rest) {
    struct State {
        atomic<bool> done{false};
        coroutine_handle<> parent;
        size_t winnerIndex;
        optional<T> winnerValue;
    };
    auto state = make_shared<State>();

    // 为每个 task 创建 wrapper，co_await 后 CAS 竞争
    // 赢家 cancel losers + resume parent，败者在自己的挂起点观察取消
    // ...
    co_return WhenAnyResult<T>{state->winnerIndex, move(*state->winnerValue)};
}
```

## 10. 面试角度总结

| 问题 | 答案要点 |
|------|----------|
| whenAny 的核心同步机制？ | atomic CAS (compare_exchange_strong)，第一个 CAS 成功的是赢家 |
| 败者协程怎么处理？ | 继续运行到完成，结果被丢弃，协程帧由 FinalAwaiter 自动销毁 |
| 为什么 wrappers 放在 State 中？ | 赢家 resume 后 Awaitable 被销毁，State 通过 shared_ptr 延长败者 wrapper 寿命 |
| 和 select / epoll 的区别？ | whenAny 是协程层竞速，select/epoll 是 fd 层事件复用；概念类似但抽象层级不同 |
| WhenAny 能否提前取消败者？ | 不能，需外部机制配合（如 cancellation token 或 SleepAwaitable::cancel） |
