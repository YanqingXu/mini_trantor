# EventLoopThread —— 工程级源码拆解

## 1. 类定位

* **角色**：后台线程中**单个 EventLoop 的生命周期管理者**
* **层级**：线程层（在 EventLoop 之上，在 EventLoopThreadPool 之下）
* 每个 EventLoopThread 管理一个 `std::jthread` 和一个 `EventLoop`，前者拥有后者的栈帧

```
  creator thread (base loop)
         │
         │  startLoop()
         │    ├── 创建 thread_（worker thread）
         │    └── condition_.wait(loop_ != nullptr)
         │
         │  (阻塞等待)
         │
  worker thread:
    threadFunc()
      ├── EventLoop loop  ← 分配在栈上（worker thread 的栈）
      ├── callback_(&loop) ← 用户初始化回调
      ├── lock_guard → loop_ = &loop
      ├── condition_.notify_one() → [creator 解阻塞，获得 loop 指针]
      └── loop.loop()  ← 进入事件循环，直到 quit()

  析构 (EventLoopThread::~EventLoopThread):
      loop_->quit()    ← 请求 worker 退出（析构函数体）
      ~std::jthread()  ← 自动 join，等待 worker 线程结束（成员析构）
```

EventLoop 的生命周期**完全由 worker thread 的栈帧控制**，EventLoopThread 不持有 EventLoop 的所有权，只持有指向它的原始指针。

---

## 2. 解决的问题

**核心问题**：如何在后台线程中启动一个 EventLoop，并且安全地把 EventLoop 的指针发布给 creator 线程？

如果没有 EventLoopThread：
- 用户需要手动 `std::thread`、手动等待 EventLoop 启动（condition variable）
- 没有统一的 quit + join 析构语义 → 容易线程泄漏
- EventLoop 的指针发布存在竞态（worker 还没初始化完，creator 就开始用指针）

EventLoopThread 提供的保证：
1. **无竞态的指针发布**：`startLoop()` 用 condition variable 等待 `loop_ != nullptr` 后才返回
2. **RAII 式析构**：析构时 quit → join，保证 worker 线程通过 `loop.loop()` 正常退出
3. **用户初始化时机**：`threadFunc` 在调用 `loop.loop()` 之前执行 `callback_`，允许在 loop 启动前做 per-thread 初始化

---

## 3. 对外接口（API）

| 方法 | 用途 | 调用线程 |
|------|------|----------|
| `EventLoopThread(callback, name)` | 构造，不启动线程 | 任意 |
| `~EventLoopThread()` | 停止并 join worker 线程 | 同构造线程 |
| `startLoop()` | 启动 worker 线程，返回 EventLoop 指针 | creator 线程（一次） |

---

## 4. 核心成员变量

```cpp
EventLoop* loop_;           // 指向 worker thread 栈上的 EventLoop（不拥有）
std::jthread thread_;       // worker 线程（C++20，析构时自动 join）
std::mutex mutex_;          // 保护 loop_ 的初始发布
std::condition_variable condition_;  // 用于 creator 等待 loop 就绪
ThreadInitCallback callback_;  // per-thread 初始化回调（可空）
std::string name_;          // 线程名称（调试用）
```

### 关键所有权关系

| 对象 | 持有者 | 生命周期 |
|------|--------|----------|
| `EventLoop` | worker thread 栈帧 | `~jthread()` 之前有效 |
| `loop_` 指针 | EventLoopThread | `~jthread()` 之前有效 |
| `thread_` | EventLoopThread | 析构时自动 join（`std::jthread`） |

---

## 5. 执行流程（关键路径）

### 5.1 startLoop —— 发布 loop 指针

```cpp
EventLoop* EventLoopThread::startLoop() {
    thread_ = std::jthread([this] { threadFunc(); });  // 启动 worker 线程

    EventLoop* loop = nullptr;
    {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return loop_ != nullptr; });  // 等待 worker 初始化
        loop = loop_;
    }
    return loop;  // 发布 loop 指针给 creator（此时 worker 已进入 loop.loop()）
}
```

**为什么需要 condition variable？**
worker 线程需要先分配 EventLoop、运行 callback_、然后才 publish `loop_`。
如果 creator 在 worker 初始化前就使用 `loop_`，会访问未初始化的 EventLoop。

### 5.2 threadFunc —— worker 线程主体

```cpp
void EventLoopThread::threadFunc() {
    EventLoop loop;          // 分配在 worker 线程的栈上

    if (callback_) {
        callback_(&loop);    // 用户初始化（在 loop.loop() 之前）
    }

    {
        std::lock_guard lock(mutex_);
        loop_ = &loop;           // 发布指针
        condition_.notify_one(); // 通知 creator: loop 已就绪
    }

    loop.loop();             // 进入事件循环（阻塞直到 quit()）

    {
        std::lock_guard lock(mutex_);
        loop_ = nullptr;     // loop 退出后清空指针（防止析构后访问）
    }
}
```

**EventLoop 在栈上而不是堆上**：生命周期与 worker 线程完全绑定，无需 `unique_ptr`，也不需要手动 delete。

### 5.3 析构 —— quit + join

```cpp
EventLoopThread::~EventLoopThread() {
    if (loop_ != nullptr) {
        loop_->quit();  // 请求 EventLoop 退出（跨线程安全，quit 使用 atomic）
    }
    // thread_（std::jthread）在此之后作为成员析构，自动 join
}
```

析构序列（按 C++ 规则：析构函数体先执行，成员按声明逆序销毁）：
```
① ~EventLoopThread() 函数体：loop_->quit()
      → EventLoop::quit_ = true（atomic）
      → wakeup()（写 eventfd，唤醒 epoll_wait）
② ~std::jthread()：自动 join（成员析构，声明顺序第 2 位）
      → 等待 loop.loop() 检测到 quit_ == true 并退出
      → threadFunc 返回
      → join 完成
③ EventLoop loop 析构（worker thread 栈展开）
④ ~mutex_, ~condition_variable_, ~callback_, ~name_（逆序）
```

**顺序为何正确**：`quit()` 在析构函数体中执行，早于 `~jthread()` 的自动 join。
若顺序颠倒（先 join 再 quit），线程永远无法退出，造成死锁。
`std::jthread` 的声明位置（第 2 个成员）不影响这个关键顺序，
因为关键路径是「析构函数体」vs「成员析构」，而非两个成员之间的顺序。

---

## 6. 协作关系

```
EventLoopThreadPool
    │
    │  for i in 0..N:
    │      thread = make_unique<EventLoopThread>(callback, name+i)
    │      loops_.push_back(thread->startLoop())  ← 阻塞等待 loop 就绪
    │      threads_.push_back(move(thread))
    │
    ▼ 使用：
    loops_[i]->runInLoop(...)  // 向 worker loop 投递任务
```

| 关系 | 描述 |
|------|------|
| `EventLoopThreadPool` → `EventLoopThread` | `unique_ptr`，管理 N 个 worker |
| `EventLoopThread` → `EventLoop` | 原始指针，指向 worker 栈帧 |
| `EventLoopThread` → `std::thread` | 成员变量，RAII joinable |

---

## 7. 关键设计点

### 7.1 栈上 EventLoop 的运行时语义

EventLoop 在 `threadFunc` 的栈帧上。这意味着：
- EventLoop 在 `loop.loop()` 阻塞期间存活
- `thread_.join()` 确保在析构 EventLoopThread 时 worker 线程已完全退出
- EventLoop 析构（即 timerQueue、wakeupChannel 等清理）在 worker 线程上发生

这是 one-loop-one-thread 最干净的实现方式。

### 7.2 为什么使用 std::jthread 而非 std::thread

`std::jthread`（C++20）相比 `std::thread` 的唯一使用目的是：
**析构时自动 join**，无需显式 `if (thread_.joinable()) thread_.join()`。

```cpp
// std::thread 时代需要的防御代码（已删除）：
// exiting_ = true;               // 仅写不读，死代码
// if (thread_.joinable()) {      // joinable 检查
//     thread_.join();            // 显式 join
// }

// std::jthread：析构函数体执行完后，成员析构自动 join，等效但更简洁
```

**未使用 std::jthread 的停止令牌（stop_token）机制**：
`EventLoop::quit()` 已通过 `atomic<bool> + eventfd` 实现完整的跨线程停止信号路径，
不需要 `request_stop()` / `stop_token`，引入它们只会产生职责重叠的噪声。

### 7.3 loop_ 的 null 保护

`threadFunc` 在 `loop.loop()` 返回后将 `loop_` 置 `nullptr`。
`~EventLoopThread()` 检查 `if (loop_ != nullptr)` 再调用 `quit()`，
防止 loop 已退出后重复 quit。

注意：这里存在一个极小的竞态窗口（loop 置空和 quit 调用之间），
但由于 `quit()` 是 atomic + wakeup，多次调用幂等，不会产生实际问题。

### 7.4 threadInitCallback 的时机

```
callback_(&loop)    ← 在 loop.loop() 之前
                       在 loop_ 发布之前
```

callback 在 loop 就绪之前执行（甚至在 creator 得到 loop 指针之前），
适合做 per-thread 的全局初始化（如日志上下文、OpenSSL 线程初始化等），
但**不能在 callback 中调用 loop->runInLoop**（loop 还没开始运行）。

---

## 8. 核心模块改动闸门（5 问）

1. **哪个 loop/线程拥有此模块？**
   EventLoopThread 由 creator 线程（通常是 base loop 线程）拥有和析构。EventLoop 由 worker thread 的栈帧拥有。

2. **谁拥有它，谁释放它？**
   EventLoopThreadPool 以 `unique_ptr<EventLoopThread>` 持有（默认情况）。析构 EventLoopThread 时会 quit + join，随后 EventLoop 的栈帧析构。

3. **哪些回调可能重入？**
   `threadInitCallback_` 在 worker 线程中 `loop.loop()` 之前调用，不存在重入。

4. **哪些操作允许跨线程，如何 marshal？**
   `startLoop()` 是 creator 线程的操作，内部用 condition variable 同步。析构（quit + join）也在 creator 线程执行，`loop_->quit()` 是唯一跨线程操作，由 EventLoop 的 atomic quit_ 保护。

5. **哪个测试文件验证此改动？**
   `tests/contract/test_event_loop_thread_pool.cc`、`tests/unit/test_event_loop.cc`

---

## 9. 从极简到完整的演进路径

```cpp
// 极简版本（无同步，不安全）
class MinimalLoopThread {
    std::thread t_;
    EventLoop* loop_{nullptr};
public:
    void start() {
        t_ = std::thread([this] {
            EventLoop loop;
            loop_ = &loop;   // 竞态！
            loop.loop();
        });
    }
    EventLoop* get() { return loop_; }  // 可能返回 nullptr 或未初始化值
};
```

从极简 → 完整，需要加：

1. **condition variable 同步** → 无竞态的 loop 指针发布
2. **callback 时机** → per-thread 初始化在 loop 启动前
3. **loop_ 置 nullptr**（退出后） → 防析构后访问
4. **quit + jthread 自动 join** → 清洁析构语义（`std::jthread` 替代显式 joinable + join）

---

## 10. 易错点与最佳实践

### 易错点

| 错误 | 后果 |
|------|------|
| startLoop() 调用两次 | 创建两个 thread_，第一个 EventLoop 无人管理 |
| 在 callback_ 中调用 loop->runInLoop | loop 还未 loop()，任务永不执行 |
| 析构前不 join | worker 线程继续运行，访问已析构的对象 |
| 持有 loop 指针超过 thread_.join() | 悬空指针（EventLoop 已回收） |

### 最佳实践

```cpp
// ✓ 标准使用方式（EventLoopThreadPool 内部）：
auto t = std::make_unique<EventLoopThread>(initCallback, "worker-0");
EventLoop* loop = t->startLoop();  // 阻塞直到 worker loop 就绪
// 此后可以安全使用 loop 指针（至少直到 t 析构前）

// ✓ 析构顺序：先让 EventLoopThreadPool 析构（会析构 EventLoopThread），
//   再析构 base loop
```

---

## 11. 面试角度总结

**Q1: EventLoopThread 中 EventLoop 为什么分配在栈上？**
A: EventLoop 的生命周期与 worker 线程完全绑定 —— loop 在 `loop.loop()` 中运行，线程退出即 loop 销毁。栈上分配无需手动管理内存，比 `unique_ptr` 更清晰。

**Q2: startLoop() 如何保证无竞态地返回 EventLoop 指针？**
A: 用 condition variable。startLoop() 阻塞在 `condition_.wait(lock, [this]{ return loop_ != nullptr; })`，直到 worker 线程执行完初始化并 `notify_one()`，才解阻塞并返回指针。

**Q3: threadInitCallback 的执行时机是什么？**
A: 在 `loop_` 发布（`condition_.notify_one()`）之前，在 `loop.loop()` 之前，在 worker 线程中执行。适合做 per-thread 初始化，但不能在其中向 loop 投递任务（loop 还没运行）。

**Q4: 析构时 join 之前 EventLoop 还活着吗？**
A: 是的。`loop_->quit()` 触发 loop 退出，`thread_.join()` 等待 worker 线程完全结束（包括 `loop.loop()` 返回和栈上 EventLoop 的析构），join 之后 EventLoop 才真正销毁。

**Q5: 如果 EventLoopThread 析构时 worker 线程正在执行回调会怎样？**
A: `quit()` 设置 atomic quit_ 标志，observer 检测到后退出 `loop.loop()`。但当前轮的分发（handleEvent）会执行完。`thread_.join()` 保证等待当前回调执行完毕后才返回，不会提前析构。
