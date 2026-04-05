# EventLoopThread —— 工程级源码拆解

## 1. 类定位

* **角色**：在独立线程中创建和运行一个 EventLoop
* **层级**：Thread 层
* "one-loop-per-thread" 模型的直接实现

## 2. 解决的问题

将"创建线程 + 在线程中创建 EventLoop + 安全发布 loop 指针"这个模式封装起来。
解决 loop 指针的线程安全发布问题（condition_variable 同步）。

## 3. 对外接口

| 方法 | 用途 |
|------|------|
| `startLoop()` | 启动线程，返回线程中的 EventLoop 指针 |
| 析构函数 | quit loop + join 线程 |

## 4. 核心成员变量

```cpp
EventLoop* loop_;                 // 线程中的 loop 指针（初始 nullptr）
bool exiting_;
std::thread thread_;
std::mutex mutex_;
std::condition_variable condition_;  // 同步 loop 指针发布
ThreadInitCallback callback_;
```

## 5. 执行流程

### startLoop

```
startLoop():
  ├─ thread_ = std::thread(threadFunc)
  ├─ unique_lock(mutex_)
  ├─ condition_.wait([loop_ != nullptr])  // 阻塞等待 loop 指针
  └─ return loop_
```

### threadFunc（在新线程中）

```
threadFunc():
  ├─ EventLoop loop  （栈上创建）
  ├─ if (callback_) callback_(&loop)
  ├─ lock(mutex_)
  ├─ loop_ = &loop
  ├─ condition_.notify_one()        // 唤醒 startLoop
  ├─ unlock
  ├─ loop.loop()                     // 阻塞在事件循环
  │   ...直到 quit()...
  ├─ lock(mutex_)
  └─ loop_ = nullptr                // loop 退出后清空指针
```

### 析构

```
~EventLoopThread():
  ├─ exiting_ = true
  ├─ if (loop_) loop_->quit()      // 请求退出事件循环
  └─ thread_.join()                 // 等待线程结束
```

## 6. 关键设计点

* **条件变量同步**：startLoop 调用者阻塞直到新线程中的 EventLoop 构造完成
* **栈上 EventLoop**：EventLoop 在线程函数的栈上创建，生命周期与线程一致
* **loop 退出后置 nullptr**：防止析构时对已销毁的 EventLoop 调用 quit

## 7. 面试角度

**Q: 为什么 startLoop 需要条件变量？**
A: EventLoop 在新线程中创建，主线程需要安全获取 loop 指针。条件变量保证 loop 完全构造后才返回。

**Q: EventLoop 为什么在栈上创建？**
A: 栈上创建保证 loop 的生命周期与 threadFunc 一致，不需要手动管理内存。loop 退出后自然析构。
