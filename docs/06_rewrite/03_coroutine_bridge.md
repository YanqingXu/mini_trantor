# 极简协程桥接重写指南

## 1. 目标

在极简 TCP Server 基础上，添加协程支持，使 echo server 可以用 `co_await` 风格编写。
新增组件：Task\<T\>, ReadAwaitable, WriteAwaitable, SleepAwaitable。

约 300 行增量代码。

## 2. 核心设计决策

### 为什么用 Lazy Coroutine

```
Eager coroutine: co_await someTask();  // 立刻开始执行，可能跨线程
Lazy  coroutine: co_await someTask();  // 不启动，直到 caller co_await 它

mini-trantor 选择 lazy：
  ✓ 调度由 EventLoop 控制，不会意外跨线程
  ✓ 不需要互斥锁保护 promise
  ✓ 与 reactor 的单线程语义天然兼容
```

### 为什么用 queueResume 而不是直接 resume

```
直接 resume 的问题：
  callback 内 resume → 协程继续执行 → 调用新的 async op → 修改 Channel
  此时仍在 Channel::handleEvent 内部 → 重入！

queueResume 方案：
  callback 内调用 loop->queueInLoop([handle]{ handle.resume(); })
  → 延迟到 pendingFunctors 阶段 resume
  → 不会重入 Channel::handleEvent
```

## 3. Task\<T\> 实现（~100 行）

### Promise

```cpp
template<typename T>
struct TaskPromise {
    std::suspend_always initial_suspend() noexcept { return {}; }  // lazy
    FinalAwaiter       final_suspend()    noexcept { return {}; }

    Task<T> get_return_object() {
        return Task<T>{
            std::coroutine_handle<TaskPromise>::from_promise(*this)
        };
    }

    void return_value(T value) { result_ = std::move(value); }
    void unhandled_exception()  { exception_ = std::current_exception(); }

    T result_;
    std::exception_ptr exception_;
    std::coroutine_handle<> continuation_;
};
```

### FinalAwaiter（对称转移）

```cpp
struct FinalAwaiter {
    bool await_ready() noexcept { return false; }

    std::coroutine_handle<> await_suspend(
        std::coroutine_handle<TaskPromise<T>> h) noexcept
    {
        auto& promise = h.promise();
        if (promise.continuation_) {
            return promise.continuation_;     // 对称转移：跳到 caller
        }
        return std::noop_coroutine();         // detached：回到调度器
    }

    void await_resume() noexcept {}
};
```

### Awaiter（co_await Task\<T\> 时触发）

```cpp
struct Awaiter {
    std::coroutine_handle<TaskPromise<T>> handle_;

    bool await_ready() noexcept { return false; }

    std::coroutine_handle<> await_suspend(
        std::coroutine_handle<> caller) noexcept
    {
        handle_.promise().continuation_ = caller;
        return handle_;  // 对称转移：跳到 task 开始执行
    }

    T await_resume() {
        if (handle_.promise().exception_)
            std::rethrow_exception(handle_.promise().exception_);
        return std::move(handle_.promise().result_);
    }
};
```

### Task

```cpp
template<typename T>
class Task {
public:
    using promise_type = TaskPromise<T>;

    explicit Task(std::coroutine_handle<promise_type> h) : handle_(h) {}
    ~Task() { if (handle_) handle_.destroy(); }

    // move only
    Task(Task&& o) noexcept : handle_(std::exchange(o.handle_, nullptr)) {}
    Task& operator=(Task&&) = delete;

    Awaiter operator co_await() noexcept {
        return Awaiter{handle_};
    }

    void start() {
        handle_.resume();  // 首次启动 lazy coroutine
    }

private:
    std::coroutine_handle<promise_type> handle_;
};
```

## 4. ReadAwaitable（~50 行）

```cpp
class ReadAwaitable {
public:
    ReadAwaitable(TcpConnection* conn) : conn_(conn) {}

    bool await_ready() noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        conn_->setMessageCallback(
            [this, h](TcpConnectionPtr, Buffer* buf) {
                result_ = buf->retrieveAllAsString();
                // 关键：不直接 resume，通过 queueInLoop 延迟
                conn_->getLoop()->queueInLoop([h] { h.resume(); });
            });
    }

    std::string await_resume() {
        return std::move(result_);
    }

private:
    TcpConnection* conn_;
    std::string result_;
};
```

### 使用方式

```cpp
Task<void> handleConnection(TcpConnectionPtr conn) {
    while (true) {
        std::string data = co_await ReadAwaitable(conn.get());
        if (data.empty()) break;  // 连接关闭
        conn->send(data);         // echo 回去
    }
}
```

## 5. SleepAwaitable（~40 行）

```cpp
class SleepAwaitable {
public:
    SleepAwaitable(EventLoop* loop, double seconds)
        : loop_(loop), seconds_(seconds) {}

    bool await_ready() noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        loop_->runAfter(seconds_, [h] { h.resume(); });
    }

    void await_resume() noexcept {}

private:
    EventLoop* loop_;
    double seconds_;
};
```

### 使用方式

```cpp
Task<void> delayedEcho(EventLoop* loop, TcpConnectionPtr conn) {
    std::string data = co_await ReadAwaitable(conn.get());
    co_await SleepAwaitable(loop, 1.0);  // 延迟 1 秒
    conn->send(data);
}
```

## 6. 协程 Echo Server

```cpp
Task<void> handleClient(TcpConnectionPtr conn) {
    while (true) {
        std::string data = co_await ReadAwaitable(conn.get());
        if (data.empty()) break;
        conn->send(data);
    }
}

int main() {
    EventLoop loop;
    InetAddress listenAddr(9999);
    TcpServer server(&loop, listenAddr);

    server.setConnectionCallback([](TcpConnectionPtr conn) {
        if (conn->connected()) {
            auto task = handleClient(conn);
            task.start();  // 首次 resume → 挂起在 ReadAwaitable
        }
    });

    server.start();
    loop.loop();
}
```

## 7. 执行流程图

```
main() → loop.loop()
  ↓
epoll_wait → Acceptor fd 可读
  ↓
Acceptor::handleRead → newConnection(fd, addr)
  ↓
TcpServer::newConnection → 创建 TcpConnection
  ↓
connectionCallback → handleClient(conn)  [协程创建，lazy 不执行]
  ↓
task.start() → 首次 resume
  ↓
进入 handleClient → co_await ReadAwaitable
  ↓
ReadAwaitable::await_suspend → 设置 messageCallback → 挂起
  ↓
... 回到 loop.loop() ...
  ↓
epoll_wait → conn fd 可读
  ↓
Channel::handleEvent → TcpConnection::handleRead
  ↓
messageCallback 被调用 → result_ = data
  ↓
queueInLoop([h]{ h.resume(); })  ← 不直接 resume！
  ↓
doPendingFunctors → h.resume()
  ↓
ReadAwaitable::await_resume 返回 data
  ↓
conn->send(data)   ← echo
  ↓
co_await ReadAwaitable  ← 再次挂起等待下一次数据
```

## 8. 演进路径

```
回调 echo server (02_minimal_tcp_server.md)
    ↓ + Task<T>
    ↓ + ReadAwaitable
协程 echo server (本文)
    ↓ + WriteAwaitable（输出缓冲区满时挂起）
    ↓ + CloseAwaitable（等待关闭完成）
    ↓ + WhenAll / WhenAny 组合器
完整协程网络库
```

## 9. 关键学习要点

| 概念 | 要点 |
|------|------|
| Lazy coroutine | initial_suspend 返回 suspend_always，需要手动 start() |
| 对称转移 | FinalAwaiter::await_suspend 返回 handle 而非 void |
| queueResume | 避免在 Channel::handleEvent 内部重入 |
| 单等待者约束 | 一个 Awaitable 同时只能有一个协程等待 |
| 协程生命周期 | Task 析构时 destroy handle，detach 时 FinalAwaiter 自毁 |
| 桥接本质 | Awaitable 本质是把 callback→resume 的适配器 |
