# EventLoopThreadPool —— 工程级源码拆解

## 1. 类定位

* **角色**：one-loop-per-thread 执行模型的**扩展层**，管理一组 worker EventLoop 并按轮转策略分发
* **层级**：线程层（在 TcpServer 之下，在 EventLoopThread 之上）
* EventLoopThreadPool 属于 base loop，由 TcpServer 控制

```
         TcpServer (base loop)
               │ owns
         EventLoopThreadPool
               │
    ┌──────────┼──────────┐
    │          │          │
 ioLoop-0  ioLoop-1  ioLoop-N   (worker loops，round-robin 分发)
    │
  若 numThreads_==0：直接返回 baseLoop_
```

EventLoopThreadPool 不做 I/O，不持有连接，只是**按顺序分发 loop 指针**。

---

## 2. 解决的问题

**核心问题**：当连接数增加，单个 EventLoop 的 epoll 成为瓶颈时，如何扩展到多个 EventLoop？

如果没有 EventLoopThreadPool：
- TcpServer 需要手动管理多个 EventLoopThread
- loop 分配策略（round-robin、优先级、hash）分散实现
- 单线程情况（numThreads=0）和多线程情况需要特殊判断
- worker 线程的启动顺序和 loop 指针发布的等待逻辑需要重复编写

EventLoopThreadPool 的设计价值：
1. **统一接口**：无论是 0 个 worker（单线程）还是 N 个，`getNextLoop()` 永远返回一个可用的 loop
2. **可预测的 round-robin 分发**：连接按顺序轮转分配到 worker，负载均衡
3. **封装启动细节**：`start()` 内部启动所有 EventLoopThread，等待所有 loop 就绪后返回

---

## 3. 对外接口（API）

| 方法 | 用途 | 调用线程 |
|------|------|----------|
| `EventLoopThreadPool(baseLoop, name)` | 构造，不启动线程 | base loop |
| `~EventLoopThreadPool()` | 析构（EventLoopThread 析构自动 quit+join） | base loop |
| `setThreadNum(n)` | 设置 worker 线程数（必须在 start() 前） | base loop |
| `start(callback)` | 启动所有 worker 线程，等待全部 loop 就绪 | base loop |
| `getNextLoop()` | round-robin 返回下一个 loop | base loop |
| `getAllLoops()` | 返回所有 loops（包含 baseLoop 若无 worker） | base loop |

---

## 4. 核心成员变量

```cpp
EventLoop* baseLoop_;    // base loop（TcpServer 的 loop），始终存在
std::string name_;       // 线程池名称

bool started_;           // 是否已调用 start()
int numThreads_;         // 设定的 worker 数量（0 = 无 worker，使用 baseLoop）
int next_;               // round-robin 计数器（下一个要分配的 ioLoop 索引）

std::vector<std::unique_ptr<EventLoopThread>> threads_;  // worker 线程（拥有）
std::vector<EventLoop*> loops_;                          // worker loops（不拥有，指向线程栈帧）
```

---

## 5. 执行流程（关键路径）

### 5.1 start —— 启动所有 worker

```cpp
void EventLoopThreadPool::start(const ThreadInitCallback& callback) {
    baseLoop_->assertInLoopThread();   // 必须在 base loop 线程调用
    started_ = true;

    for (int i = 0; i < numThreads_; ++i) {
        auto thread = std::make_unique<EventLoopThread>(
            callback, name_ + std::to_string(i));  // 构造 worker
        loops_.push_back(thread->startLoop());      // 阻塞等待 loop 就绪
        threads_.push_back(std::move(thread));
    }

    if (numThreads_ == 0 && callback) {
        callback(baseLoop_);    // 单线程模式下对 baseLoop 做初始化
    }
}
```

`startLoop()` 是阻塞调用，每启动一个 worker 都等待其 EventLoop 完全就绪后才继续启动下一个。这保证了 start() 返回时，所有 worker loops 都可以使用。

### 5.2 getNextLoop —— round-robin 分发

```cpp
EventLoop* EventLoopThreadPool::getNextLoop() {
    baseLoop_->assertInLoopThread();    // 只能在 base loop 线程调用
    EventLoop* loop = baseLoop_;

    if (!loops_.empty()) {
        loop = loops_[static_cast<size_t>(next_)];
        ++next_;
        if (next_ >= static_cast<int>(loops_.size())) {
            next_ = 0;     // wrap around
        }
    }
    return loop;
}
```

**单线程降级**：`loops_` 为空时直接返回 `baseLoop_`，单线程和多线程对调用方透明。

**round-robin 的特性**：
- 顺序：conn-0→ioLoop-0，conn-1→ioLoop-1，...，conn-N→ioLoop-0（循环）
- 长期均匀分布，但不考虑当前各 loop 的负载（无智能均衡）
- `next_` 只在 base loop 线程访问，无锁安全

### 5.3 析构

```cpp
EventLoopThreadPool::~EventLoopThreadPool() = default;
```

析构自动销毁 `threads_` 容器，每个 `unique_ptr<EventLoopThread>` 析构时触发：
```
~EventLoopThread()
  → loop_->quit()      // 通知 worker EventLoop 退出
  → thread_.join()     // 等待 worker 线程结束
  // EventLoop 的栈帧此时回收
```

---

## 6. 协作关系

```
TcpServer
    │ owns (shared_ptr)
EventLoopThreadPool
    │ owns (unique_ptr×N)
EventLoopThread × N
    │ (worker thread 栈帧内)
EventLoop × N  ←── loops_[0..N-1]
```

| 关系 | 描述 |
|------|------|
| `TcpServer` → `EventLoopThreadPool` | `shared_ptr`，TcpServer 控制生命周期 |
| `EventLoopThreadPool` → `EventLoopThread` | `unique_ptr`，拥有所有 worker 线程 |
| `EventLoopThreadPool` → `EventLoop*`（loops_） | 原始指针，不拥有，指向 worker 栈帧 |
| `EventLoopThreadPool` → `EventLoop*`（baseLoop_） | 借用，指向 TcpServer 的 base loop |

---

## 7. 关键设计点

### 7.1 zero-thread 模式

```
numThreads_ = 0
  → loops_ 为空
  → getNextLoop() 返回 baseLoop_
  → 所有连接的 I/O 在 base loop 处理
  → 适合单核、低并发、测试场景
```

单线程模式下，base loop 同时承担监听（Acceptor）和 I/O（TcpConnection），
是功能完整但吞吐量有限的单线程 Reactor。

### 7.2 round-robin 的局限性

round-robin 按连接数均衡，但不考虑：
- 活跃连接 vs 空闲连接（一个活跃 + 999 空闲 vs 1000 个均等活跃）
- 每条连接的数据量
- ioLoop 的当前 pending functor 压力

对于大多数 echo-style / 短连接场景，round-robin 已经足够。
若需要负载感知分发，需要在此基础上扩展（当前版本不支持）。

### 7.3 loops_ 指针的有效性保证

`loops_[i]` 指向 `threads_[i]` 内部的 EventLoop 栈帧，有效性由以下保证：
- `threads_[i]`（EventLoopThread）析构时 join worker 线程，EventLoop 才回收
- `EventLoopThreadPool` 析构先于 `baseLoop_` 析构（TcpServer 控制顺序）

一旦 `threads_[i]` 析构，`loops_[i]` 变为悬空指针。
因此**在 EventLoopThreadPool 析构之后，不能使用任何 loops_ 指针**。

### 7.4 start() 只调用一次

start() 没有 atomic 保护（不像 TcpServer::start()），因为它的调用点（TcpServer::start() 内部）已有保护。
重复调用 start() 会创建重复的 worker 线程并泄漏。

---

## 8. 核心模块改动闸门（5 问）

1. **哪个 loop/线程拥有此模块？**
   base loop 线程拥有 EventLoopThreadPool 及其所有方法的访问权。`getNextLoop`、`start`、`getAllLoops` 都必须在 base loop 线程调用。

2. **谁拥有它，谁释放它？**
   TcpServer 以 `shared_ptr<EventLoopThreadPool>` 持有，TcpServer 析构时释放。所有 EventLoopThread 随之析构（quit + join）。

3. **哪些回调可能重入？**
   `start()` 中的 `threadInitCallback` 在各 worker 线程中执行，对 EventLoopThreadPool 无访问，不存在重入。

4. **哪些操作允许跨线程，如何 marshal？**
   EventLoopThreadPool 的所有方法仅在 base loop 线程调用。向 worker loops 投递任务通过 `loop->runInLoop()`（EventLoop 的跨线程安全接口）完成，不经过 EventLoopThreadPool。

5. **哪个测试文件验证此改动？**
   `tests/contract/test_event_loop_thread_pool.cc`

---

## 9. 从极简到完整的演进路径

```cpp
// 极简版本（无 start 控制，无 zero-thread 降级）
class MinimalThreadPool {
    std::vector<EventLoop*> loops_;
    int next_{0};
public:
    void start(int n) {
        for (int i = 0; i < n; ++i) {
            // 启动 EventLoopThread，获取 loop 指针
            loops_.push_back(startOneThread());
        }
    }
    EventLoop* next() {
        return loops_[next_++ % loops_.size()];  // 若 loops_ 为空：崩溃
    }
};
```

从极简 → 完整，需要加：

1. **zero-thread 降级** → `loops_` 空时返回 `baseLoop_`
2. **assertInLoopThread** → 防止 getNextLoop 被错误线程调用
3. **threadInitCallback** → per-thread 初始化支持
4. **started_ 标志** → 防止 start 前调用 getNextLoop
5. **unique_ptr owner** → 保证 EventLoopThread 正确析构（quit+join）
6. **round-robin wrap-around** → `if (next_ >= loops_.size()) next_ = 0`

---

## 10. 易错点与最佳实践

### 易错点

| 错误 | 后果 |
|------|------|
| 在非 base loop 线程调用 getNextLoop | assertInLoopThread abort |
| EventLoopThreadPool 析构后使用 loops_[i] | 悬空指针 |
| start() 调用两次 | 重复创建 worker 线程，泄漏 EventLoopThread |
| setThreadNum 在 start() 后调用 | 此时已经启动的线程数不变，numThreads_ 被修改但无效 |

### 最佳实践

```cpp
// ✓ 标准使用顺序（TcpServer 内部）：
auto pool = std::make_shared<EventLoopThreadPool>(baseLoop, "server");
pool->setThreadNum(std::thread::hardware_concurrency());  // 1. 设线程数
pool->start(initCallback);                                 // 2. 启动（所有 loop 就绪后返回）
EventLoop* ioLoop = pool->getNextLoop();                   // 3. 分配 loop

// ✓ 单元测试 / 单线程场景：
pool->setThreadNum(0);     // zero-thread 模式
pool->start({});
EventLoop* loop = pool->getNextLoop();  // 返回 baseLoop_
assert(loop == baseLoop);
```

---

## 11. 面试角度总结

**Q1: EventLoopThreadPool 的 zero-thread 模式是什么？**
A: `numThreads_=0` 时，`loops_` 为空，`getNextLoop()` 直接返回 `baseLoop_`。所有连接都在 base loop 上处理，退化为单线程 Reactor，适合低并发场景。

**Q2: getNextLoop 为什么只能在 base loop 线程调用？**
A: `next_` 计数器无锁保护，只有 base loop 线程访问才安全。round-robin 分发决策由 base loop 的 `newConnection` 调用，天然在正确线程。

**Q3: round-robin 的缺点是什么？**
A: 按连接数均衡，不感知各连接的实际负载。一个传输大文件的连接和一个空闲 keepalive 连接被同等对待，可能造成某个 ioLoop 过忙。改进需要引入负载感知分发。

**Q4: loops_ 指针何时失效？**
A: EventLoopThreadPool 析构时，`threads_` 中的 EventLoopThread 依次析构（quit+join），对应的 worker thread 栈帧回收，`loops_[i]` 变为悬空指针。TcpServer 的析构顺序保证 EventLoopThreadPool 先于 baseLoop 释放。

**Q5: start() 内部如何保证所有 worker 就绪？**
A: `EventLoopThread::startLoop()` 是阻塞调用，内部等待 condition variable，直到 worker 线程的 `loop_` 被赋值（即 EventLoop 完成初始化且 callback 执行完）才返回。`start()` 串行调用 N 次 `startLoop()`，全部返回后所有 worker 就绪。

**Q6: 与 TcpServer 的所有权关系？**
A: TcpServer 以 `shared_ptr<EventLoopThreadPool>` 持有，其他地方（如连接的 closeCallback lambda）也可持有 shared_ptr。TcpServer 析构时先重置 lifetimeToken，再清理 connections_，最后 shared_ptr 引用为 0 时 EventLoopThreadPool 析构，worker 线程随之 quit+join。
