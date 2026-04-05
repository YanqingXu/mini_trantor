# EventLoopThread —— 工程级源码拆解

## 1. 类定位

* **角色**：在独立线程中创建和运行一个 EventLoop —— "one-loop-per-thread" 模型的直接载体
* **层级**：Thread 层
* 作为 EventLoopThreadPool 的构建块，每个 worker 线程对应一个 EventLoopThread

```
         EventLoopThreadPool (base loop 线程)
                │
                ├── EventLoopThread #0
                │      │
                │      ├── std::thread (worker 线程 0)
                │      │      └── EventLoop (栈上构造)
                │      │              │
                │      │              └── loop() → epoll_wait 循环
                │      │
                │      └── startLoop() → 条件变量同步 → 返回 EventLoop*
                │
                ├── EventLoopThread #1
                │      └── ...
                │
                └── EventLoopThread #N
                       └── ...
```

EventLoopThread **不拥有外部组件**，它只做三件事：
1. 启动线程
2. 在线程中创建 EventLoop
3. 安全发布 loop 指针

## 2. 解决的问题

**核心问题**：如何安全地在一个新线程中创建 EventLoop，并将 loop 指针发布给创建者线程？

如果没有 EventLoopThread：
- 需要手动 `std::thread([]{...})` + 在 lambda 中创建 EventLoop
- loop 指针发布存在竞态：创建者可能在 EventLoop 构造前就使用指针
- 线程 join 时机和 loop quit 需要手动协调
- 每个需要 worker 线程的地方都要重复"创建线程 + 同步发布 + 安全退出"模式

EventLoopThread 封装了：
1. **线程创建** → `std::thread` 管理
2. **loop 指针安全发布** → `condition_variable` 同步
3. **安全退出** → 析构时 quit + join

## 3. 对外接口（API）

| 方法 | 用途 | 线程安全 |
|------|------|----------|
| `EventLoopThread(callback, name)` | 构造，可选传入线程初始化回调 | 构造线程 |
| `~EventLoopThread()` | quit loop + join 线程 | 构造线程 |
| `startLoop()` | 启动线程，阻塞等待 loop 创建完成，返回 EventLoop* | 仅调用一次 |

### 不拥有的 API

EventLoopThread **不提供** `queueWork()` 或 `postTask()` —— 那是 EventLoop 的职责。
使用者拿到 `EventLoop*` 后，通过 `loop->runInLoop(cb)` 投递任务。

## 4. 核心成员变量

```cpp
EventLoop* loop_;                       // 线程中的 loop 指针（初始 nullptr）
bool exiting_;                          // 是否正在退出
std::thread thread_;                    // 工作线程
std::mutex mutex_;                      // 保护 loop_ 指针的发布
std::condition_variable condition_;     // 同步 loop 发布
ThreadInitCallback callback_;           // 线程初始化回调（可选）
std::string name_;                      // 线程名称（调试用）
```

### 所有权关系

| 变量 | 所有权 | 说明 |
|------|--------|------|
| `loop_` | 观察 | 指向 worker 线程栈上的 EventLoop，不拥有 |
| `thread_` | 拥有 | RAII 管理线程生命周期 |
| `callback_` | 拥有 | 可选的初始化回调 |

### 线程安全

| 变量 | 访问线程 | 同步机制 |
|------|----------|----------|
| `loop_` | 创建者线程 + worker 线程 | `mutex_` + `condition_` |
| `exiting_` | 创建者线程 | 仅析构时写 |
| `thread_` | 创建者线程 | 仅启动时和析构时操作 |
| `callback_` | worker 线程 | 构造后不再修改 |

## 5. 执行流程（最重要）

### 5.1 构造

```
EventLoopThread::EventLoopThread(callback, name)
  ├─ loop_ = nullptr
  ├─ exiting_ = false
  ├─ callback_ = std::move(callback)
  └─ name_ = std::move(name)
```

构造时**不启动线程**。延迟到 `startLoop()` 调用时才创建。

### 5.2 startLoop —— 线程安全发布

```
EventLoopThread::startLoop()          // 在创建者线程（通常是 base loop 线程）
  ├─ thread_ = std::thread(threadFunc)  // 启动 worker 线程
  ├─ unique_lock(mutex_)
  ├─ condition_.wait([loop_ != nullptr])  // 阻塞等待 worker 线程完成 loop 创建
  └─ return loop_                         // 此时 loop 已安全发布
```

### 5.3 threadFunc —— worker 线程主体

```
EventLoopThread::threadFunc()          // 在 worker 线程中
  ├─ EventLoop loop                     // 栈上构造 EventLoop
  ├─ if (callback_) callback_(&loop)    // 执行初始化回调
  │
  ├─ {
  │   lock_guard(mutex_)
  │   loop_ = &loop                     // 发布 loop 指针
  │   condition_.notify_one()            // 唤醒 startLoop
  │ }
  │
  ├─ loop.loop()                         // 进入事件循环（阻塞直到 quit）
  │
  └─ {
      lock_guard(mutex_)
      loop_ = nullptr                    // loop 即将析构，清空指针
    }
```

**时间线**：

```
创建者线程                    Worker 线程
    │                             │
    ├─ startLoop()                │
    │   ├─ new thread ──────────►│
    │   │                         ├─ EventLoop loop (栈上)
    │   │                         ├─ callback_(&loop)
    │   │   ┌─── wait ◄──────────├─ loop_ = &loop
    │   │   │                     ├─ notify_one
    │   │   └── waken ────────►  │
    │   ├─ return loop_           ├─ loop.loop() ── 阻塞
    │   │                         │   ...
    │   │                         │
    │   │   (使用 loop指针)        │
    │   │                         │
    │   │ ~EventLoopThread()      │
    │   ├─ loop_->quit() ────────► 唤醒 epoll_wait
    │   │                         ├─ loop.loop() 退出
    │   │                         ├─ loop_ = nullptr
    │   │                         └─ EventLoop 析构
    │   ├─ thread_.join() ────── 等待线程结束
    │   └─ 完成
```

### 5.4 析构

```
~EventLoopThread()
  ├─ exiting_ = true
  ├─ if (loop_ != nullptr):
  │   └─ loop_->quit()            // 请求退出事件循环
  └─ if (thread_.joinable()):
      └─ thread_.join()           // 等待线程结束
```

**析构顺序保证**：
1. `loop_->quit()` 通过 eventfd wakeup 唤醒可能阻塞在 `epoll_wait` 中的 worker 线程
2. `loop.loop()` 退出后，worker 线程执行 `loop_ = nullptr`（此时创建者已拿到 loop，但不应再用）
3. EventLoop 在 worker 线程栈上析构
4. `thread_.join()` 等待 worker 线程退出
5. EventLoopThread 本身析构完成

## 6. 关键交互关系

```
EventLoopThreadPool
    │ 拥有
    ▼
EventLoopThread ×N
    │ 启动
    ▼
std::thread
    │ 栈上构造
    ▼
EventLoop
    │ 使用
    ├── Poller
    ├── wakeupChannel
    ├── TimerQueue
    └── pendingFunctors
```

| 类 | 关系 |
|----|------|
| **EventLoopThreadPool** | 拥有 EventLoopThread（unique_ptr） |
| **EventLoop** | worker 线程栈上创建，EventLoopThread 观察其指针 |
| **TcpConnection** | 绑定到 worker EventLoop，通过 runInLoop 投递任务 |
| **TcpServer** | 间接使用（通过 ThreadPool） |

## 7. 关键设计点

### 7.1 栈上 EventLoop

```cpp
void EventLoopThread::threadFunc() {
    EventLoop loop;   // 栈上创建
    // ...
    loop.loop();      // 阻塞
    // loop 在离开作用域时自动析构
}
```

**为什么不 new EventLoop？**
- 栈上创建保证 loop 的生命周期完全与 threadFunc 一致
- 不需要手动管理内存，不会泄漏
- EventLoop 析构时可以安全地 assert 线程匹配（因为析构发生在 worker 线程中）

### 7.2 条件变量同步

```cpp
// startLoop（创建者线程）
condition_.wait(lock, [this] { return loop_ != nullptr; });

// threadFunc（worker 线程）
loop_ = &loop;
condition_.notify_one();
```

**为什么不用 promise/future？**

`condition_variable` 更轻量，且与 `mutex_` 共用。
`loop_` 指针既是同步信号又是有效数据，不需要额外的 future 对象。

### 7.3 loop 退出后置 nullptr

```cpp
// threadFunc 末尾
{
    std::lock_guard lock(mutex_);
    loop_ = nullptr;     // EventLoop 即将析构
}
```

**防止析构时对已销毁的 EventLoop 调用 quit**：
```cpp
~EventLoopThread() {
    if (loop_ != nullptr) {     // 如果 loop 还活着
        loop_->quit();          // 才去 quit
    }
}
```

如果 loop 已经自行退出（例如外部调用了 quit），此时 `loop_` 已是 nullptr，
析构时跳过 quit 调用，避免访问已析构的 EventLoop。

### 7.4 ThreadInitCallback

```cpp
if (callback_) {
    callback_(&loop);    // 在 worker 线程中执行，loop 已构造但未 loop()
}
```

用途：
- 设置线程名称（pthread_setname_np）
- 设置线程优先级
- 初始化 thread-local 资源

## 8. 从极简到完整的演进路径

```cpp
// 极简版
class MinimalEventLoopThread {
public:
    EventLoop* start() {
        thread_ = std::thread([this] {
            EventLoop loop;
            loop_ = &loop;
            ready_.store(true);
            loop.loop();
        });
        while (!ready_.load()) {}  // 忙等（不好！）
        return loop_;
    }
    ~MinimalEventLoopThread() {
        loop_->quit();
        thread_.join();
    }
private:
    EventLoop* loop_ = nullptr;
    std::atomic<bool> ready_{false};
    std::thread thread_;
};
```

完整版改进：
1. **condition_variable** → 替代忙等，CPU 友好
2. **mutex 保护 loop_** → 防止读写竞态
3. **loop 退出后置 nullptr** → 安全析构
4. **ThreadInitCallback** → 线程初始化钩子
5. **name** → 调试友好

## 9. 易错点与最佳实践

### 易错点

| 问题 | 后果 | 防护 |
|------|------|------|
| startLoop 未调用就析构 | thread_ 不可 joinable，安全 | joinable 检查 |
| 重复调用 startLoop | 覆盖 thread_（前一个线程 detach） | 当前无防护（应由调用者保证） |
| 在 loop_ 析构后访问 | 段错误 | loop 退出后置 nullptr |
| callback_ 中抛异常 | 线程终止，notify 未执行，startLoop 永久阻塞 | 应确保 callback 无异常 |

### 最佳实践

- 只调用一次 `startLoop()`
- `startLoop()` 返回后立即使用 loop 指针是安全的
- 不要缓存 loop 指针到 EventLoopThread 析构之后
- ThreadInitCallback 应该轻量且不抛异常

## 10. 面试角度总结

### 核心模块改动闸门

| 问题 | 答案 |
|------|------|
| 1. 哪个 loop/thread 拥有此模块？ | EventLoopThread 由创建者线程（base loop 线程）拥有 |
| 2. 谁拥有它，谁释放它？ | EventLoopThreadPool 通过 unique_ptr 拥有，析构时 quit + join |
| 3. 哪些回调可能重入？ | 无回调重入问题（callback_ 只在 worker 线程执行一次） |
| 4. 哪些操作允许跨线程，如何投递？ | loop_->quit() 跨线程安全（atomic + eventfd wakeup） |
| 5. 哪个测试文件验证此变更？ | tests/contract/test_event_loop_thread_pool.cc |

### 高频面试问题

**Q1: 为什么 startLoop 需要条件变量？**
A: EventLoop 在 worker 线程中构造，创建者线程需要安全获取 loop 指针。条件变量保证 loop 指针在完全构造后才被发布。

**Q2: EventLoop 为什么在栈上创建而不是 new？**
A: 栈上创建保证 loop 的生命周期与 threadFunc 完全一致，不需要手动管理内存。loop 退出后自然析构，且析构一定发生在 worker 线程中（满足 EventLoop 的线程亲和性要求）。

**Q3: 析构时如果 loop 已经退出会怎样？**
A: threadFunc 末尾已将 `loop_` 置为 nullptr。析构函数检查 `loop_` 非空才调用 quit，因此不会访问已析构的 EventLoop。

**Q4: 如果 ThreadInitCallback 抛异常会怎样？**
A: worker 线程异常终止，`condition_.notify_one()` 不会执行，`startLoop()` 永久阻塞。因此 callback 必须保证不抛异常。

**Q5: 能否多次调用 startLoop？**
A: 不应该。当前实现未做防护，重复调用会覆盖 thread_ 成员。应由调用者保证只调用一次（EventLoopThreadPool 保证了这一点）。

**Q6: EventLoopThread 和 EventLoop 的生命周期关系？**
A: EventLoopThread 拥有 thread，EventLoop 在 thread 栈上，生命周期是 thread 的子集。EventLoopThread 析构时先 quit(loop)，再 join(thread)，loop 在 thread 退出前析构。
