# WhenAll 源码拆解

## 1. 类定位

* **角色**: 协程层（Coroutine）结构化并发原语
* **层级**: 纯协程层，不依赖 EventLoop / Reactor
* WhenAll 是 **并发等待全部完成** 的组合器——等待 N 个 Task 全部完成后恢复父协程

## 2. 解决的问题

* **核心问题**: 并发执行多个异步操作，等待所有操作完成后聚合结果
* 如果没有 WhenAll:
  - 需要手动维护计数器和共享状态
  - 异常传播需要逐个处理
  - 结果聚合需要手动按索引存储

典型用例:
```cpp
auto [a, b] = co_await whenAll(fetchUser(id), fetchOrders(id));
```

## 3. 对外接口（API）

| 函数 | 签名 | 场景 |
|------|------|------|
| `whenAll(Task<Ts>...)` | `Task<tuple<Ts...>>` | 等待多个值类型 Task |
| `whenAll(Task<void>...)` | `Task<void>` | 等待多个 void Task |
| `whenAll()` | `Task<void>` | 零参数，立即完成 |

### 内部类

| 类 | 作用 |
|----|------|
| `WhenAllState<Ts...>` | 共享控制块（值类型），含 atomic 计数器 + results tuple |
| `WhenAllVoidState` | 共享控制块（void 类型），只有计数器 |
| `WhenAllAwaitable<Ts...>` | 值类型 Awaitable |
| `WhenAllVoidAwaitable<N>` | void 类型 Awaitable |

## 4. 核心成员变量

### WhenAllState\<Ts...\>

```
WhenAllState<Ts...>
├── remaining: atomic<size_t>                    // 未完成子任务数
├── parent: coroutine_handle<>                   // 父协程句柄
├── firstException: exception_ptr                // 第一个异常（保留）
├── results: tuple<optional<Ts>...>              // 按索引存储结果
```

### WhenAllAwaitable\<Ts...\>

```
WhenAllAwaitable<Ts...>
├── state_: shared_ptr<WhenAllState<Ts...>>      // 共享控制块
├── wrappers_: Task<void>[N]                     // N 个 wrapper 协程
```

## 5. 执行流程

### 5.1 完整调用链

```
co_await whenAll(taskA, taskB, taskC)
  │
  ├─ 1. whenAll() 是协程函数
  │     → co_return co_await WhenAllAwaitable(move(tasks)...)
  │
  ├─ 2. WhenAllAwaitable 构造:
  │     → 创建 WhenAllState(count=3)
  │     → 为每个 task 创建 whenAllWrapper 协程:
  │        wrappers_[0] = whenAllWrapper<0, A>(taskA, state)
  │        wrappers_[1] = whenAllWrapper<1, B>(taskB, state)
  │        wrappers_[2] = whenAllWrapper<2, C>(taskC, state)
  │
  ├─ 3. await_suspend(parentHandle):
  │     → state->parent = parentHandle
  │     → for each wrapper: wrapper.detach()
  │       // detach 让 wrapper 自行管理生命周期
  │       // 但此时还未 start（lazy coroutine）
  │       // detach 内部会 start
  │
  ├─ 4. 每个 wrapper 协程执行:
  │     whenAllWrapper<I, T>(subtask, state):
  │       → auto val = co_await move(subtask)   // 运行子任务
  │       → state->results.get<I>().emplace(val) // 存储结果
  │       → state->onComplete()                   // 原子递减
  │
  ├─ 5. onComplete() 检查:
  │     → remaining.fetch_sub(1) == 1 ?          // 是最后一个？
  │       → YES: parent.resume()                  // 恢复父协程
  │       → NO:  继续等待
  │
  └─ 6. await_resume():
        → 检查 firstException，有异常则 rethrow
        → extractResults() → tuple<A, B, C>
```

### 5.2 异常处理

```
whenAllWrapper 内部:
  try {
      co_await subtask;
  } catch (...) {
      state->captureException(current_exception());
      // 不中断其他子任务
  }
  state->onComplete();  // 无论成功还是失败都递减计数器
```

* 只保留第一个异常（firstException）
* 所有子任务都会运行完成（不会提前终止）
* await_resume 时 rethrow 第一个异常

### 5.3 序列图

```
  Parent        whenAll         Wrapper0       Wrapper1       Wrapper2
    │              │               │              │              │
    │─co_await────▶│               │              │              │
    │              │─ construct ──▶│              │              │
    │              │               │──detach+start│              │
    │              │               │              │──detach+start│
    │              │               │              │              │──detach+start
    │  (suspended) │               │              │              │
    │              │          co_await taskA  co_await taskB co_await taskC
    │              │               │              │              │
    │              │          result[0]=val  result[1]=val  result[2]=val
    │              │          onComplete()   onComplete()   onComplete()
    │              │               │              │         remaining==1!
    │              │               │              │         parent.resume()
    │◀─────────────────────────────────────────────────────────────┘
    │  await_resume → tuple<A,B,C>
```

## 6. 关键交互关系

```
┌────────────┐  co_await whenAll   ┌──────────────────┐
│ 父协程     │────────────────────▶│ WhenAllAwaitable │
│            │◀────────────────────│                  │
└────────────┘  resume when done   └────────┬─────────┘
                                            │ detach + start
                                            ▼
                                   ┌─────────────────┐
                                   │ Wrapper[0..N-1] │  Task<void>
                                   │ (whenAllWrapper) │
                                   └────────┬────────┘
                                            │ co_await
                                            ▼
                                   ┌─────────────────┐
                                   │ 子 Task<T>      │
                                   └─────────────────┘
```

* **不依赖 EventLoop**: 纯协程层构造
* **依赖 Task\<T\>**: 使用 Task 的 co_await 和 detach 机制
* **结果通过 shared_ptr 共享**: wrapper 和 awaitable 通过 WhenAllState 通信

## 7. 关键设计点

### 7.1 Wrapper 协程模式

* 不直接 co_await 子任务，而是包装成 `whenAllWrapper` 协程
* **好处**: 每个 wrapper 负责 try/catch + 存储结果 + onComplete，逻辑清晰
* wrapper 被 `detach()` 后自行管理生命周期

### 7.2 atomic 计数器

```cpp
void onComplete() {
    if (remaining.fetch_sub(1, memory_order_acq_rel) == 1) {
        parent.resume();
    }
}
```

* `fetch_sub` 返回旧值，== 1 表示"我是最后一个"
* `memory_order_acq_rel` 确保 results/exception 写入对 parent 可见

### 7.3 void 特化

* 全部为 `Task<void>` 时使用 `WhenAllVoidAwaitable`
* 不需要 results tuple，只有计数器和异常
* 通过 `requires` 约束在编译期选择正确的重载

### 7.4 shared_ptr 生命周期保障

* WhenAllState 被 shared_ptr 持有，wrapper 协程捕获它
* 即使 WhenAllAwaitable 被销毁（父协程被 resume 后），wrapper 仍能安全访问 state

## 8. 潜在问题

| 问题 | 描述 | 严重程度 |
|------|------|----------|
| 只保留第一个异常 | 后续异常被丢弃，可能丢失诊断信息 | 低 |
| 所有子任务必须完成 | 无法 short-circuit（一个失败不会取消其他） | 中（设计选择） |
| wrappers_ 固定数组 | `Task<void> wrappers_[sizeof...(Ts)]`，编译期固定大小 | 低 |
| firstException 非 atomic | 多个 wrapper 可能在不同线程产生异常（如果子任务跨线程） | 中 |

## 9. 极简实现骨架

```cpp
template<typename... Ts>
Task<tuple<Ts...>> whenAll(Task<Ts>... tasks) {
    struct State {
        atomic<size_t> remaining{sizeof...(Ts)};
        coroutine_handle<> parent;
        tuple<optional<Ts>...> results;
        exception_ptr ex;
    };
    auto state = make_shared<State>();

    auto wrap = [&]<size_t I>(Task<auto> task, auto state) -> Task<void> {
        try {
            get<I>(state->results) = co_await move(task);
        } catch (...) {
            state->ex = current_exception();
        }
        if (state->remaining.fetch_sub(1) == 1) state->parent.resume();
    };

    // ... create and detach wrappers ...
    // co_await suspend → set state->parent → detach all wrappers
    if (state->ex) rethrow_exception(state->ex);
    co_return extractResults(state->results);
}
```

## 10. 面试角度总结

| 问题 | 答案要点 |
|------|----------|
| whenAll 的核心同步机制？ | atomic 计数器 fetch_sub，最后一个完成者 resume 父协程 |
| 为什么用 wrapper 协程？ | 隔离 try/catch + 结果存储 + 计数逻辑，每个子任务独立 |
| 如何处理异常？ | 所有子任务都运行完成，只保留第一个异常 |
| 为什么不用 mutex？ | 各 wrapper 写不同的 optional slot，atomic 计数器足够 |
| 和 std::when_all (提案) 的区别？ | mini-trantor 的实现更简单，只支持同质/异质 Task，不支持任意 awaitable |
