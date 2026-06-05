# Task\<T\> 源码拆解

## 1. 类定位

* **角色**: 协程层（Coroutine）核心原语
* **层级**: Coroutine 层，不依赖 Reactor / Net 层
* Task\<T\> 是 mini-trantor 协程体系的基石——它是一个 **lazy 协程句柄包装器**，表示一个延迟执行的异步计算，返回类型为 T

## 2. 解决的问题

* **核心问题**: 提供类型安全的协程返回类型，管理 `coroutine_handle` 的所有权和生命周期
* 如果没有 Task\<T\>:
  - 用户需要手动管理 `coroutine_handle` 的销毁
  - 没有统一的 co_await 挂接协议
  - 父子协程之间的续接（continuation）需要手动编排
  - 返回值传递需要自定义 promise_type

## 3. 对外接口（API）

| 方法 | 场景 | 调用者 |
|------|------|--------|
| `Task(Task&& other)` | 移动构造，转移协程所有权 | 内部 |
| `~Task()` | 销毁协程帧（如果未 detach） | 自动 |
| `start()` | 启动 lazy 协程（resume 一次） | 用户 / 框架 |
| `detach()` | 放弃所有权，协程自行销毁 | WhenAll / WhenAny |
| `result()` | 获取 co_return 结果 | 用户 |
| `operator co_await()` | 使 Task 可被 co_await | 用户协程 |

### 关键说明

* `start()` 只能调用一次（lazy coroutine 的首次 resume）
* `detach()` 后 Task 不再拥有协程帧，FinalAwaiter 负责销毁
* `operator co_await` 返回内部 Awaiter，负责设置 continuation 并 resume 子协程

## 4. 核心成员变量

```
Task<T>
├── handle_: coroutine_handle<TaskPromise<T>>    // 协程句柄，唯一所有权
```

### TaskPromiseBase（promise 基类）

```
TaskPromiseBase
├── continuation_: coroutine_handle<>    // 父协程句柄（被 co_await 时设置）
├── detached_: bool                      // 是否已 detach（FinalAwaiter 用于判断是否自动销毁）
```

### TaskPromise\<T\>（值类型特化）

```
TaskPromise<T> : TaskPromiseBase
├── result_: std::optional<T>            // co_return 值的存储
├── exception_: std::exception_ptr       // co_return 异常的存储
```

### TaskPromise\<void\>（void 特化）

```
TaskPromise<void> : TaskPromiseBase
├── exception_: std::exception_ptr       // 仅存异常
```

**线程安全**: Task 本身不做线程安全保护，设计为单线程使用（协程帧归属于 owner loop 线程）

## 5. 执行流程

### 5.1 Lazy 创建

```
用户调用协程函数
  → 编译器调用 TaskPromise::get_return_object()
    → 创建 Task<T>(handle)
  → initial_suspend() 返回 suspend_always
    → 协程挂起，不执行任何代码
  → 返回 Task<T> 给调用者
```

### 5.2 父协程 co_await 子 Task

```
co_await childTask
  → operator co_await() 返回 Awaiter
    → Awaiter 构造时 exchange(handle_, nullptr) 转移所有权
  → await_ready() → false（始终挂起）
  → await_suspend(parentHandle):
    → child.promise().continuation_ = parentHandle  // 记住父协程
    → return childHandle                            // 对称转移到子协程
  → 子协程执行...
  → co_return value
    → TaskPromise::return_value(value)
      → result_.emplace(value)
  → final_suspend() 返回 FinalAwaiter
    → FinalAwaiter::await_suspend():
      → 如果 continuation_ 存在 → return continuation_（恢复父协程）
      → 否则已 detach → return noop_coroutine()
  → await_resume() 返回 result().value()
```

### 5.3 Detach 模式（用于 WhenAll/WhenAny）

```
task.detach()
  → detached_ = true
  → handle_ = nullptr（放弃所有权）
  → task.start()（之前已被外部调用）
  → 协程执行完成
  → FinalAwaiter::await_suspend():
    → 如果 !continuation_ && detached_
      → 销毁协程帧
      → return noop_coroutine()
```

### 5.4 调用链图

```
           ┌──────────┐
           │ 调用者   │
           └────┬─────┘
                │ co_await task
                ▼
         ┌──────────────┐
         │   Awaiter    │  exchange ownership
         │ await_suspend│──── set continuation ────┐
         └──────────────┘                          │
                │ symmetric transfer               │
                ▼                                  │
         ┌──────────────┐                          │
         │  子协程执行  │                          │
         │  co_return   │                          │
         └──────┬───────┘                          │
                │ final_suspend                    │
                ▼                                  │
         ┌──────────────┐                          │
         │ FinalAwaiter │  resume continuation ────┘
         └──────────────┘         │
                                  ▼
                           父协程 await_resume()
```

## 6. 关键交互关系

```
                    ┌───────────────────────┐
                    │      WhenAll          │  detach() + start()
                    │      WhenAny          │─────────────────────┐
                    └───────────────────────┘                     │
                                                                  ▼
┌────────────┐   co_await   ┌─────────────┐               ┌──────────┐
│ 用户协程   │─────────────▶│  Task<T>    │───────────────▶│ Promise  │
│            │◀─────────────│  (Awaiter)  │  owns handle   │ (帧内)   │
└────────────┘  resume via  └─────────────┘               └──────────┘
               FinalAwaiter        │
                                   │ 被 co_await 时
                                   ▼
                          ┌─────────────────┐
                          │  SleepAwaitable │  (子协程中可 co_await)
                          │  ReadAwaitable  │
                          │  WriteAwaitable │
                          └─────────────────┘
```

* **上游**: WhenAll, WhenAny 通过 detach()+start() 使用 Task
* **下游**: Task 内部可 co_await 任何符合 awaitable 协议的对象
* **不依赖**: EventLoop, Channel 等 Reactor 层（纯协程层构造）

## 7. 关键设计点

### 7.1 Lazy 协程（initial_suspend = always）

* **为什么**: 允许调用者在 co_await 前设置 continuation，并控制启动时机
* 如果用 eager 协程（initial_suspend = never），子协程可能在 continuation 设置前就跑完了

### 7.2 对称转移（symmetric transfer）

```cpp
auto await_suspend(std::coroutine_handle<> parent) {
    promise.continuation_ = parent;
    return childHandle;  // 不是 void，而是返回 handle → 对称转移
}
```

* **好处**: 避免递归 resume 导致栈溢出（深度嵌套协程链）
* C++20 对称转移是关键性能和正确性保证

### 7.3 FinalAwaiter 的双重角色

```cpp
auto await_suspend(std::coroutine_handle<Promise> h) noexcept {
    if (h.promise().continuation_)
        return h.promise().continuation_;     // 恢复父协程
    if (h.promise().detached_) {
        h.destroy();                          // detach 模式自销毁
        return std::noop_coroutine();
    }
    return std::noop_coroutine();             // 无 continuation，等待外部 destroy
}
```

* 三种情况完全覆盖：有父协程 / detach 模式 / 独立 Task

### 7.4 Awaiter 的所有权转移

```cpp
struct Awaiter {
    Handle handle_;
    Awaiter(Handle h) : handle_(h) {}
};

auto operator co_await() && noexcept {
    return Awaiter(std::exchange(handle_, nullptr));
}
```

* `exchange` 确保所有权从 Task 转移到 Awaiter，Task 析构不会 double-destroy

## 8. 潜在问题

| 问题 | 描述 | 严重程度 |
|------|------|----------|
| 未检查 double start | `start()` 被调用两次会 resume 已运行的协程 → UB | 中 |
| result() 在未完成时调用 | 如果协程未 co_return 就调用 result()，optional 为空 → UB | 中 |
| 非线程安全 | 不同线程 co_await 同一个 Task → 数据竞争 | 低（设计约束） |
| detach 后忘记 start | detach() 放弃所有权但不自动 start → 协程帧泄漏 | 低（使用者责任） |

## 9. 极简实现骨架

```cpp
template<typename T>
class Task {
    using Handle = std::coroutine_handle<TaskPromise<T>>;
    Handle handle_;
public:
    explicit Task(Handle h) : handle_(h) {}
    ~Task() { if (handle_) handle_.destroy(); }
    Task(Task&& o) : handle_(std::exchange(o.handle_, nullptr)) {}

    void start() { handle_.resume(); }
    void detach() { handle_.promise().detached_ = true; handle_ = nullptr; }
    T result() { return std::move(*handle_.promise().result_); }

    auto operator co_await() && {
        struct Awaiter {
            Handle h;
            bool await_ready() { return false; }
            auto await_suspend(std::coroutine_handle<> parent) {
                h.promise().continuation_ = parent;
                return h;
            }
            T await_resume() { return std::move(*h.promise().result_); }
        };
        return Awaiter{std::exchange(handle_, nullptr)};
    }
};
```

## 10. 面试角度总结

| 问题 | 答案要点 |
|------|----------|
| Task 为什么是 lazy 的？ | initial_suspend=always，让父协程先设置 continuation 再 resume 子协程 |
| 对称转移解决什么问题？ | 避免 resume 链递归爆栈，深度 co_await 链安全 |
| FinalAwaiter 的作用？ | 协程结束时自动恢复父协程 / 自销毁 detached 协程 / 悬停等待外部 |
| Task 的所有权模型？ | 唯一所有权，移动语义转移 handle，detach 放弃所有权由 FinalAwaiter 销毁 |
| 和 std::future 的区别？ | Task 是零开销抽象（无堆分配 shared state），依赖编译器协程帧，支持对称转移 |
