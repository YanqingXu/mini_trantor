# TcpConnection 拆分重构方案

## 1. 目标

本方案不改变 `TcpConnection` 的对外定位：

- 它仍然表示“一个绑定到单个 EventLoop 的 TCP 连接”
- 它仍然是连接生命周期和 public API 的中心
- 它仍然负责把 close / error / forceClose 收敛到一条安全关闭路径

本方案要做的，是把已经塞进 `TcpConnection` 的三个“可分离变化轴”拆出去：

1. transport 变化轴：plain TCP / TLS
2. coroutine 变化轴：read / write / close waiter 状态机
3. backpressure 变化轴：threshold + paused-reading enforcement

目标不是“把类切碎”，而是把变化原因从一个类里分离出来，
让 `TcpConnection` 回到“连接生命周期中心”的角色。

---

## 2. 当前问题

当前 `TcpConnection` 同时承担：

- 连接状态机
- Socket / Channel / Buffer 所有权
- public callback API
- plain TCP I/O
- TLS 握手和 SSL I/O
- coroutine awaitable facade
- waiter 挂起/恢复状态
- backpressure threshold enforcement

这会带来三个工程问题：

1. 一个改动容易跨多个语义域
   例如改 TLS 写路径时，还需要重新验证 write waiter、high-water、shutdown 语义。

2. public 生命周期模型被实现细节淹没
   读 `TcpConnection` 时，很难快速看清“连接对象到底负责什么”。

3. 测试边界不够清晰
   目前契约测试很强，但内部逻辑没有被拆成更容易做 unit 测试的模块。

---

## 3. 重构后的模块边界

### 3.1 TcpConnection

保留职责：

- public API：`send` / `shutdown` / `forceClose`
- public callback 注册
- 连接状态机：`kConnecting -> kConnected -> kDisconnecting -> kDisconnected`
- `Socket` / `Channel` / `Buffer` 所有权
- `connectEstablished()` / `connectDestroyed()`
- `handleClose()` / `handleError()` 作为唯一生命周期收敛入口

不再内联承担：

- TLS/plain transport 细节
- waiter 存储和恢复细节
- backpressure paused-reading enforcement 细节

### 3.2 ConnectionTransport

职责：

- plain TCP 或 TLS transport 的 non-blocking read/write/handshake/shutdown
- 向 `TcpConnection` 报告 transport 结果
- 不拥有 public lifecycle，只作为 transport 适配器

### 3.3 ConnectionAwaiterRegistry

职责：

- 管理 read/write/close waiter 的挂起与恢复
- 保证 resume 仍然经过 EventLoop 调度
- 在 close/error 时统一恢复并清空 waiter

### 3.4 ConnectionBackpressureController

职责：

- 管理 high-water / low-water 阈值
- 依据 output buffer 大小 pause/resume read interest
- 把 paused-reading 这种临时策略状态从 `TcpConnection` 主体中移走

---

## 4. 类草图

下面是建议的结构草图，不是最终代码：

```cpp
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    void send(std::string_view message);
    void shutdown();
    void forceClose();

    void setConnectionCallback(ConnectionCallback cb);
    void setMessageCallback(MessageCallback cb);
    void setHighWaterMarkCallback(HighWaterMarkCallback cb, std::size_t highWaterMark);
    void setWriteCompleteCallback(WriteCompleteCallback cb);
    void setCloseCallback(CloseCallback cb);

    void setBackpressurePolicy(std::size_t highWaterMark, std::size_t lowWaterMark);
    void startTls(std::shared_ptr<TlsContext> ctx, bool isServer, const std::string& hostname = "");

    ReadAwaitable asyncReadSome(std::size_t minBytes = 1);
    WriteAwaitable asyncWrite(std::string data);
    CloseAwaitable waitClosed();

    void connectEstablished();
    void connectDestroyed();

private:
    void handleRead(Timestamp receiveTime);
    void handleWrite();
    void handleClose();
    void handleError(int savedErrno = 0);

    void sendInLoop(const char* data, std::size_t len);
    void shutdownInLoop();
    void forceCloseInLoop();

    void onBytesRead(std::size_t n);
    void onOutputDrained();
    void notifyWriteQueuedOrBuffered();

private:
    EventLoop* loop_;
    StateE state_;
    std::unique_ptr<Socket> socket_;
    std::unique_ptr<Channel> channel_;
    Buffer inputBuffer_;
    Buffer outputBuffer_;
    ConnectionCallbacks callbacks_;
    std::unique_ptr<ConnectionTransport> transport_;
    std::unique_ptr<ConnectionAwaiterRegistry> awaiters_;
    std::unique_ptr<ConnectionBackpressureController> backpressure_;
    std::any context_;
};
```

### 4.1 ConnectionTransport 草图

```cpp
struct TransportReadResult {
    ssize_t bytesRead{0};
    int savedErrno{0};
    bool peerClosed{false};
    bool needWriteInterest{false};
};

struct TransportWriteResult {
    ssize_t bytesWritten{0};
    int savedErrno{0};
    bool faultError{false};
    bool needReadInterest{false};
};

class ConnectionTransport {
public:
    virtual ~ConnectionTransport() = default;

    virtual bool handshakePending() const noexcept = 0;
    virtual bool established() const noexcept = 0;

    virtual TransportReadResult readInto(Buffer& input, Channel& channel) = 0;
    virtual TransportWriteResult writeFrom(Buffer& output, Channel& channel) = 0;

    virtual void beginHandshake(Channel& channel) = 0;
    virtual bool advanceHandshake(Channel& channel) = 0;
    virtual void shutdownWrite(Socket& socket) = 0;
};
```

### 4.2 ConnectionAwaiterRegistry 草图

```cpp
class ConnectionAwaiterRegistry {
public:
    explicit ConnectionAwaiterRegistry(EventLoop* loop);

    void armReadWaiter(TcpConnection& conn, std::coroutine_handle<> handle, std::size_t minBytes);
    void armWriteWaiter(TcpConnection& conn, std::coroutine_handle<> handle, std::string data);
    void armCloseWaiter(TcpConnection& conn, std::coroutine_handle<> handle);

    void onReadable(TcpConnection& conn);
    void onOutputDrained(TcpConnection& conn);
    void onClosed();

private:
    void queueResume(std::coroutine_handle<> handle);
};
```

### 4.3 ConnectionBackpressureController 草图

```cpp
class ConnectionBackpressureController {
public:
    explicit ConnectionBackpressureController(EventLoop* loop);

    void configure(std::size_t highWaterMark, std::size_t lowWaterMark);
    void onConnectionEstablished(Channel& channel);
    void onOutputBufferedBytesChanged(std::size_t readableBytes, Channel& channel);
    void onClosed();

    bool readingPausedByBackpressure() const noexcept;
};
```

---

## 5. 调用关系草图

### 5.1 读路径

```text
Channel readable event
  -> TcpConnection::handleRead()
  -> transport_->readInto(inputBuffer_, *channel_)
  -> awaiters_->onReadable(*this)
  -> 若没有 read waiter 消费该事件，则 messageCallback_
  -> 出错时仍然走 handleError() / handleClose()
```

### 5.2 写路径

```text
TcpConnection::send()
  -> 跨线程时 runInLoop()
  -> TcpConnection::sendInLoop()
  -> transport_->writeFrom(outputBuffer_, *channel_) 或先尝试直写
  -> 若仍有剩余数据，写入 outputBuffer_
  -> backpressure_->onOutputBufferedBytesChanged(...)
  -> 缓冲清空时 awaiters_->onOutputDrained()
```

### 5.3 关闭路径

```text
read EOF / write fault / forceClose / shutdown completion
  -> TcpConnection::handleClose()
  -> channel_->disableAll()
  -> awaiters_->onClosed()
  -> backpressure_->onClosed()
  -> connectionCallback_(disconnected)
  -> closeCallback_()
```

---

## 6. 分阶段迁移建议

### Phase 1: 内部搬运，不改 public API

- 保持 `TcpConnection.h` 的 public API 不变
- 新增三个 helper 类，但只做内部接管
- 现有契约测试必须保持全绿

这一阶段的收益最大，风险最低。

### Phase 2: 缩减 `TcpConnection.h`

- 保留 awaitable facade，但把 waiter 状态和 helper 函数移出头文件
- 让 `TcpConnection.h` 不再暴露 TLS 内部状态枚举和 waiter state 结构

目标是让头文件重新体现“连接对象的公开职责”。

### Phase 3: 再决定是否继续外提 awaitable facade

- 如果后续 coroutine API 继续扩大，可考虑把 awaitable facade 迁到
  `mini/coroutine/`
- 但这一步不是现在的必须项

不要在第一阶段就同时改 internal decomposition 和 public coroutine API。

---

## 7. 测试清单

本次重构建议把测试拆成三层：

### 7.1 Unit

新增建议：

- `tests/unit/net/test_connection_transport_plain.cpp`
  验证 plain transport 的读写返回语义、无 TLS 时的基线行为
- `tests/unit/net/test_connection_awaiter_registry.cpp`
  验证 duplicate waiter 拒绝、close 时 waiter 清空、resume once 语义
- `tests/unit/net/test_connection_backpressure_controller.cpp`
  验证 high-water pause、low-water resume、disabled policy 恢复常态

### 7.2 Contract

继续保留并扩展：

- `tests/contract/tcp_connection/test_tcp_connection.cpp`
  继续作为 `TcpConnection` 的主契约文件，验证：
  - cross-thread send / forceClose 线程语义
  - 统一 close 路径
  - awaiter resume 语义不变
  - backpressure 行为对外不变

- `tests/contract/tls/test_tls_handshake.cpp`
  继续验证 TLS 接入后 callback 顺序和 owner-loop handshake 语义不变

### 7.3 Integration

继续回归：

- `tests/integration/tls/test_tls_echo.cpp`
- `tests/integration/tcp_server/test_tcp_server_backpressure_policy.cpp`
- `tests/integration/coroutine/test_coroutine_echo_server.cpp`
- `tests/integration/coroutine/test_coroutine_idle_timeout.cpp`

这几组是防止“拆完内部结构但主链路被改坏”的关键回归点。

---

## 8. 改动闸门答案

1. 哪个 loop / thread 拥有这些新模块？
   所有新 helper 都属于同一个 `TcpConnection` owner loop，没有新增线程域。

2. 谁拥有它，谁释放它？
   `TcpConnection` 统一拥有并释放 `ConnectionTransport`、
   `ConnectionAwaiterRegistry`、`ConnectionBackpressureController`。

3. 哪些回调可能重入？
   `connectionCallback_`、`messageCallback_`、`writeCompleteCallback_`、
   `closeCallback_`，以及 coroutine resume 后的用户协程逻辑都可能重入连接相关代码。

4. 哪些操作允许跨线程，如何投递？
   仍然只允许通过 `runInLoop()` / `queueInLoop()` 进入 owner loop；
   helper 不提供额外跨线程入口。

5. 哪些测试文件验证？
   主验证文件应为：
   - `tests/contract/tcp_connection/test_tcp_connection.cpp`
   - `tests/contract/tls/test_tls_handshake.cpp`
   - `tests/integration/tls/test_tls_echo.cpp`
   - `tests/integration/tcp_server/test_tcp_server_backpressure_policy.cpp`

---

## 9. 最终建议

如果只做一件事，优先做 Phase 1：

- 不改 public API
- 先把 transport / awaiter / backpressure 三类内部逻辑抽成 loop-owned helper
- 用现有 contract/integration 测试兜住行为

这是最符合 mini-trantor 方法论的路径：

- 先 intent
- 再边界
- 再测试
- 最后实现
