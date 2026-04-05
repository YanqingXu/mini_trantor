# EventLoopThreadPool —— 工程级源码拆解

## 1. 类定位

* **角色**：管理多个 EventLoopThread，提供 round-robin 分配
* **层级**：Thread 层
* TcpServer 通过它创建 worker loops

## 2. 解决的问题

multi-reactor 模型中，base loop 负责 accept，worker loops 负责 I/O。
ThreadPool 封装了创建 N 个 EventLoopThread + round-robin 分配。

## 3. 对外接口

| 方法 | 用途 |
|------|------|
| `setThreadNum(n)` | 设置线程数（start 前调用） |
| `start(callback)` | 启动所有线程 |
| `getNextLoop()` | round-robin 返回下一个 loop |
| `getAllLoops()` | 返回所有 loop 指针 |

## 4. 核心成员变量

```cpp
EventLoop* baseLoop_;                              // base loop（不在线程池中）
std::vector<std::unique_ptr<EventLoopThread>> threads_;
std::vector<EventLoop*> loops_;                     // 每个线程的 loop 指针
int numThreads_;
int next_;                                          // round-robin 索引
```

## 5. 执行流程

### start

```
start(callback):
  ├─ for i in 0..numThreads:
  │   ├─ thread = new EventLoopThread(callback, name+i)
  │   ├─ loops_.push_back(thread->startLoop())  // 阻塞直到 loop 创建完成
  │   └─ threads_.push_back(thread)
  └─ if numThreads == 0 && callback:
      callback(baseLoop_)                        // 单线程模式也调用初始化回调
```

### getNextLoop

```
getNextLoop():
  ├─ assertInLoopThread()        // 只在 base loop 线程调用
  ├─ if loops_ 为空 → return baseLoop_  // numThreads=0 共用 base loop
  └─ loop = loops_[next_]
     next_ = (next_ + 1) % loops_.size()
     return loop
```

## 6. 关键设计点

* **numThreads=0 时共用 base loop**：所有连接和 accept 在同一个线程
* **round-robin**：简单均匀，不考虑 loop 负载
* **assertInLoopThread()**：getNextLoop 只在 base loop 的 newConnection 中调用

## 7. 面试角度

**Q: numThreads=0 时什么效果？**
A: 所有连接在 base loop 上处理，单线程 Reactor。

**Q: 为什么用 round-robin 而不是最少连接？**
A: v1 追求简单。round-robin 在连接生命周期均匀时效果足够好。
