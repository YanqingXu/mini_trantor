# Acceptor —— 工程级源码拆解

## 1. 类定位

* **角色**：监听 socket 的**事件注册与 accept 路径的薄适配层**
* **层级**：Net 层（在 TcpServer 和 Reactor 核心之间）
* Acceptor 将 `listenfd` 的可读事件转化为 `newConnection(sockfd, peerAddr)` 回调，不做更多事

```
         EventLoop (base loop)
               │
      epoll 监听 listenfd 可读
               │
          Acceptor::handleRead()
               │
        accept() 循环 (while EAGAIN)
               │
          newConnectionCallback_(connfd, peerAddr)
               │
           TcpServer::newConnection()
               │
       选 ioLoop，创建 TcpConnection
```

Acceptor **不拥有 TcpServer**，也**不选择 worker loop**，只是把 accept 到的 fd "递送"出去。

---

## 2. 解决的问题

**核心问题**：监听 socket 的管理和新连接的接收，谁来负责？

如果没有 Acceptor：
- TcpServer 需要直接管理 listenfd 的注册与 accept 循环
- TcpServer 既要管理业务逻辑又要管理 fd，职责混乱
- accept 的边缘触发（ET）模式下需要循环 accept 至 EAGAIN，逻辑不独立

Acceptor 的设计价值：
1. **单一职责**：只做 `socket → bind → listen → accept → cb`
2. **loop 亲和性**：listen/accept 全在 owner loop 上，无跨线程并发
3. **可测试性**：Acceptor 是一个独立的小组件，可以单独验证 accept 行为

---

## 3. 对外接口（API）

| 方法 | 用途 | 调用者 |
|------|------|--------|
| `Acceptor(loop, listenAddr, reusePort)` | 构造：创建 socket，bind，设置读回调 | TcpServer |
| `~Acceptor()` | 析构：disableAll + remove，必须在 loop 线程 | 析构链 |
| `setNewConnectionCallback(cb)` | 设置新连接到达时的回调 | TcpServer |
| `listen()` | 开始 listen + enableReading | TcpServer::start() → runInLoop |
| `listening()` | 查询是否已在监听 | TcpServer 状态检查 |

---

## 4. 核心成员变量

```cpp
EventLoop* loop_;                          // 所属 EventLoop（base loop，不拥有）
Socket acceptSocket_;                      // RAII 封装的 listenfd（拥有，析构时 close）
Channel acceptChannel_;                    // listenfd 对应的 Channel（拥有）
NewConnectionCallback newConnectionCallback_; // 上层回调（TcpServer 注入）
bool listening_;                           // 是否已调用 listen()
```

### 线程归属

- `loop_` 是 TcpServer 的 base loop（也是 `Acceptor` 的 owner loop）
- 所有成员的修改都在 `loop_` 线程执行
- `acceptSocket_` 析构（close fd）也发生在 `loop_` 线程

---

## 5. 执行流程（关键路径）

### 5.1 构造

```cpp
Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reusePort)
    : loop_(loop),
      acceptSocket_(sockets::createNonblockingOrDie()),  // 非阻塞 socket
      acceptChannel_(loop, acceptSocket_.fd()),           // 绑定 Channel
      listening_(false) {
    acceptSocket_.setReuseAddr(true);
    acceptSocket_.setReusePort(reusePort);
    acceptSocket_.bindAddress(listenAddr);                // 此时不 listen
    acceptChannel_.setReadCallback([this](Timestamp t) { handleRead(t); });
}
```

注意：构造时**只 bind，不 listen**。`listen()` 在 `TcpServer::start()` 中显式调用。

### 5.2 启动监听

```cpp
void Acceptor::listen() {
    loop_->assertInLoopThread();      // 必须在 owner loop 上
    listening_ = true;
    acceptSocket_.listen();           // 系统调用 ::listen()
    acceptChannel_.enableReading();   // 注册 EPOLLIN 到 epoll
}
```

`TcpServer::start()` 通过 `runInLoop` 调用此方法，确保线程安全：
```cpp
loop_->runInLoop([this] { acceptor_->listen(); });
```

### 5.3 accept 循环

```cpp
void Acceptor::handleRead(Timestamp) {
    loop_->assertInLoopThread();
    while (true) {
        InetAddress peerAddr;
        const int connfd = acceptSocket_.accept(&peerAddr);  // 非阻塞 accept
        if (connfd >= 0) {
            if (newConnectionCallback_) {
                newConnectionCallback_(connfd, peerAddr);    // 上传给 TcpServer
            } else {
                sockets::close(connfd);  // 没有上层 → 立即关闭，防 fd 泄漏
            }
            continue;
        }
        // 错误处理
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 没有更多连接
        if (errno == EINTR) continue;                         // 信号打断，重试
        break;                                                // 其他错误，退出
    }
}
```

**为什么是 while 循环而不是单次 accept？**
epoll 默认 LT 模式下单次就够，但为了支持 ET 模式（边缘触发只通知一次），必须循环 accept 至 EAGAIN。

### 5.4 析构

```cpp
Acceptor::~Acceptor() {
    if (!loop_->isInLoopThread()) {
        std::fputs("Acceptor destroyed from non-owner thread\n", stderr);
        std::abort();
    }
    if (listening_) {
        acceptChannel_.disableAll();  // 取消 EPOLLIN
        acceptChannel_.remove();      // 从 epoll 移除
    }
}
```

析构时**必须在 owner loop 线程**执行 `disableAll + remove`，否则 epoll 持有悬空 Channel 指针。
TcpServer 的析构在 base loop 线程，天然满足此约束。

---

## 6. 协作关系

```
         TcpServer (base loop)
              │
              │ 构造时注入 newConnectionCallback_
              │
           Acceptor
           ├── Socket (listenfd)
           └── Channel (listenfd 的事件代理)
                    │ EPOLLIN → handleRead()
                    │
                    ▼ newConnectionCallback_(connfd, peerAddr)
                    │
              TcpServer::newConnection()  (在 base loop 线程执行)
```

| 关系 | 描述 |
|------|------|
| `TcpServer` → `Acceptor` | 拥有（`unique_ptr`），注入回调，调用 `listen()` |
| `Acceptor` → `Socket` | 拥有 listenfd 的 RAII 包装 |
| `Acceptor` → `Channel` | 拥有 listenfd 的事件订阅代理 |
| `Acceptor` → `EventLoop` | 借用（原始指针），不拥有 |
| `Channel` → `EPollPoller` | 通过 EventLoop 注册 EPOLLIN |

---

## 7. 关键设计点

### 7.1 只 bind 不 listen 的原因

构造函数只做 socket + bind，`listen()` 被拆分出去。这样做的好处：
- 允许 TcpServer 在 `start()` 之前设置所有回调（若 listen 在构造时触发，可能在回调设置前就来了连接）
- `start()` 通过 `runInLoop` 调度 `listen()`，确保线程安全

### 7.2 accept 后立即 "传递 or 关闭"

```cpp
if (connfd >= 0) {
    if (newConnectionCallback_) {
        newConnectionCallback_(connfd, peerAddr);  // 传递
    } else {
        sockets::close(connfd);                    // 关闭，防泄漏
    }
}
```

Acceptor 本身**不决定如何处理连接**，只做两件事之一：交给上层，或立刻关闭。fd 所有权通过 callback 参数传递给 TcpServer。

### 7.3 非阻塞 socket

`createNonblockingOrDie()` 创建的是非阻塞 socket（带 `SOCK_NONBLOCK | SOCK_CLOEXEC`）。
非阻塞保证 `accept()` 在没有新连接时立即返回 `EAGAIN`，while 循环才有意义。

### 7.4 析构线程保护

```cpp
if (!loop_->isInLoopThread()) {
    std::fputs("Acceptor destroyed from non-owner thread\n", stderr);
    std::abort();
}
```

这是 **crash-fast 策略**。错误的析构线程意味着程序员犯了严重错误，直接 abort 比悄悄运行更安全。

---

## 8. 核心模块改动闸门（5 问）

1. **哪个 loop/线程拥有此模块？**
   TcpServer 的 base loop（即 TcpServer 构造时传入的 `EventLoop*`）。

2. **谁拥有它，谁释放它？**
   TcpServer 以 `unique_ptr<Acceptor>` 拥有，TcpServer 析构时释放。析构必须在 base loop 线程。

3. **哪些回调可能重入？**
   `newConnectionCallback_` 在 `handleRead()` 内被调用，即在 epoll 事件分发期间。此回调（`TcpServer::newConnection`）本身必须是 loop-safe 的，不能调用任何会导致 `handleRead` 重新被调用的操作。

4. **哪些操作允许跨线程，如何 marshal？**
   `listen()` 通过 `loop_->runInLoop()` 在 base loop 上调用，是唯一需要 marshal 的操作。`handleRead` 和析构都在 base loop 线程直接执行。

5. **哪个测试文件验证此改动？**
   `tests/contract/test_tcp_server.cc`（集成路径）

---

## 9. 从极简到完整的演进路径

```cpp
// 极简版本
class MinimalAcceptor {
    int listenfd_;
    std::function<void(int)> cb_;
public:
    MinimalAcceptor(uint16_t port) {
        listenfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        // bind, listen...
    }
    void accept() {
        int fd = ::accept(listenfd_, nullptr, nullptr);
        if (cb_) cb_(fd);
    }
};
```

从极简 → 完整，需要加：

1. **Channel 注册** → 不轮询，由 epoll 通知
2. **非阻塞 + while EAGAIN** → 边缘触发安全
3. **构造/监听分离** → 允许在 listen 前设置回调
4. **析构线程检查 + disableAll/remove** → 防悬空 epoll 指针
5. **无 callback 时关闭 fd** → 防 fd 泄漏

---

## 10. 易错点与最佳实践

### 易错点

| 错误 | 后果 |
|------|------|
| 在非 owner 线程析构 | abort（有检查）；或 epoll 持有悬空 Channel 指针 |
| 未设置 callback 就 start | 每个新连接都会被立即 close（有兜底，但 server 不工作） |
| 在 callback 前调用 listen() | 可能在回调设置前到来连接（start() 的 runInLoop 确保顺序） |

### 最佳实践

```cpp
// ✓ 正确的 TcpServer 启动顺序：
TcpServer server(loop, addr, "name");
server.setConnectionCallback(onConn);    // 1. 先设置回调
server.setMessageCallback(onMsg);
server.start();                          // 2. 再 start（内部调用 listen）

// ✗ 错误：在另一个线程析构 TcpServer（即 Acceptor）
// Acceptor 的析构必须在 base loop 线程执行
```

---

## 11. 面试角度总结

**Q1: Acceptor 的职责是什么？**
A: 管理 listenfd 的 socket/bind/listen/accept 全流程，将 accept 到的 connfd 通过回调传递给 TcpServer。是 TcpServer 和 Reactor 之间的薄适配层。

**Q2: 为什么 listen() 和构造函数分开？**
A: 允许用户在 `start()` 之前设置所有回调，避免在回调注册完成之前就接收连接。同时 `start()` 通过 `runInLoop` 保证 `listen()` 在 loop 线程执行。

**Q3: handleRead 为什么是 while 循环？**
A: 为支持 ET（边缘触发）模式，必须循环 accept 至 EAGAIN。LT 模式下单次也可以，但 while 循环对两种模式都正确。

**Q4: Acceptor 析构时为什么要 abort？**
A: crash-fast 策略。在错误的线程析构 Acceptor 是程序员的严重错误，直接崩溃比带着错误状态继续运行更安全（防止 epoll 悬空指针导致随机崩溃）。

**Q5: 如果没有 newConnectionCallback 会怎样？**
A: accept 到的 connfd 会被立即 close，不会泄漏 fd。

**Q6: Acceptor 的线程模型是什么？**
A: 完全单线程，属于 TcpServer 的 base loop。所有操作（listen/accept/析构）都在 base loop 线程执行。
