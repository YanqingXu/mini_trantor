# mini/net/detail 框架理解文档

## 0. 文档摘要

- `mini/net/detail/` 不是“零碎内部实现”目录，而是 `TcpConnection` 的内部协作者层。
- 它解决的是 `TcpConnection` 容易膨胀成神类的问题，把 4 类不同变化轴拆开。
- 这 4 类变化轴分别是：传输层差异、协程等待状态、背压控制、回调投递。
- 目录里的对象都不是独立对外模块，它们都从属于单个 `TcpConnection`，并共享同一个 owner `EventLoop`。
- `TcpConnection` 仍然是连接生命周期中心，`detail/` 里的类不能绕过它直接改变 public lifecycle。
- `ConnectionTransport` 隐藏 plain TCP 与 TLS 的非阻塞 I/O 和握手差异。
- `ConnectionAwaiterRegistry` 隐藏 read/write/close awaiter 的保存、恢复、取消与 close 清理。
- `ConnectionBackpressureController` 隐藏高低水位背压的迟滞控制和读事件暂停/恢复。
- `ConnectionCallbackDispatcher` 隐藏 callback 存储与“立即调用/排队调用”的分发策略。
- 阅读这个目录时，最先要抓住的是三条线：谁拥有这些 helper、谁能改状态、它们如何回到 `TcpConnection` 的统一关闭路径。

## 1. 框架整体定位

### 1.1 这层代码是什么

`mini/net/detail/` 是 `mini/net/TcpConnection` 的内部实现分解层。  
它的定位不是对外 API，也不是新的 reactor 核心，而是把“连接内部的可变复杂性”拆成几个 loop-owned helper。

### 1.2 它解决什么问题

如果没有这一层，`TcpConnection` 需要同时内联处理：

- plain/TLS 两套 I/O 行为
- 握手状态推进
- coroutine waiter 的保存、恢复、取消
- 高低水位背压状态
- write complete / high water mark 的回调投递细节

这些逻辑都与连接有关，但它们变化的原因不同。`detail/` 的作用就是把这些变化原因拆开，让 `TcpConnection` 保持“生命周期协调者”身份。

### 1.3 这一层的边界

它负责：

- 协助 `TcpConnection` 完成 transport、awaiter、backpressure、callback dispatch
- 封装局部状态机和状态存储
- 在 owner loop 线程内执行局部策略

它不负责：

- 拥有 `EventLoop`
- 拥有 `TcpConnection` 的 public state machine
- 直接调用 `TcpServer/TcpClient` 的连接移除逻辑
- 绕过 `EventLoop` 直接恢复协程
- 自己发明另一套关闭/错误路径

### 1.4 最核心的运行模型

这一层的运行模型可以概括成一句话：

`TcpConnection` 持有 helper，helper 在 owner loop 上执行局部逻辑，再把结果交回 `TcpConnection` 统一收口。

也就是说：

1. 所有 helper 都被 `TcpConnection::Impl` 的 `unique_ptr` 拥有
2. 所有可变操作都遵守同一个 loop thread
3. 真正改变连接 public state 的还是 `TcpConnection`
4. close/error 最终仍然走 `runCloseSequence()`

## 2. 目录结构总览

### 2.1 目录树摘要

```text
mini/net/detail/
├── ConnectionTransport.h/.cc
├── ConnectionAwaiterRegistry.h/.cc
├── ConnectionBackpressureController.h/.cc
└── ConnectionCallbackDispatcher.h/.cc
```

### 2.2 每个文件为什么存在

#### `ConnectionTransport.h/.cc`

存在原因：
把 plain TCP 与 TLS 的读、写、握手、shutdown 差异封装在一个内部 transport 适配层里，避免 `TcpConnection` 直接堆满 `SSL_*` 细节。

#### `ConnectionAwaiterRegistry.h/.cc`

存在原因：
把 coroutine handle 的注册、恢复、取消、close 清理集中管理，避免 `TcpConnection` 同时维护多组 waiter 状态和恢复策略。

#### `ConnectionBackpressureController.h/.cc`

存在原因：
把 output buffer 高低水位阈值、读暂停/恢复的迟滞逻辑独立出来，避免背压策略污染连接主状态机。

#### `ConnectionCallbackDispatcher.h/.cc`

存在原因：
把 callback slot 和分发方式封装起来，尤其是“哪些回调立即触发，哪些必须 `queueInLoop()` 延后触发”的策略。

### 2.3 目录边界

- `mini/net/`：公开网络模块和生命周期中心
- `mini/net/detail/`：`TcpConnection` 的内部协作者
- `mini/coroutine/`：通用协程承载原语
- `tests/unit/net/`：这些 helper 的局部语义验证
- `tests/contract/tcp_connection/` 与 `tests/integration/`：验证 helper 接入 `TcpConnection` 后的整体行为

## 3. 核心模块地图

### 3.1 模块一：ConnectionTransport

- 模块职责：执行非阻塞 read/write/handshake/shutdown，并把 transport 结果显式返回给 `TcpConnection`
- 为什么需要：plain TCP 和 TLS 的行为差异很大，但连接关闭、回调顺序、owner-loop 规则不能因此分裂
- 依赖哪些模块：`Buffer`、`Channel`、`Socket`、`TlsContext`、OpenSSL
- 哪些模块依赖它：`TcpConnection`
- 暴露什么能力：`enableTls()`、`advanceHandshake()`、`readInto()`、`writeRaw()`、`writeFromBuffer()`、`shutdownWrite()`
- 隐藏什么复杂性：`SSL_do_handshake()`、`SSL_read()`、`SSL_write()`、`WANT_READ/WANT_WRITE` 与 channel interest 的联动
- 系统地位：内部 transport 适配层

典型入口：

- `TcpConnection::startTls()`
- `TcpConnection::connectEstablished()`
- `TcpConnection::handleRead()`
- `TcpConnection::handleWrite()`
- `TcpConnection::shutdownInLoop()`

关键数据结构：

- `Status`：`kOk / kWouldBlock / kPeerClosed / kError`
- `ReadResult`、`WriteResult`、`HandshakeResult`
- `tlsContext_`、`ssl_`、`tlsState_`

最容易误解的点：

- 它可以修改 `Channel` 的读写关注位，但它不能决定连接状态是否关闭。
- TLS 失败不会自己销毁连接，只是把失败结果交回 `TcpConnection::handleError()`。
- plain path 是基线实现，TLS path 是可选叠加，而不是另一套连接生命周期。

阅读建议：
先看头文件结果类型，再看 `.cc` 中 plain path，最后看 `sslReadIntoBuffer()` / `sslWriteFromBuffer()` 如何处理 `WANT_READ/WANT_WRITE`。

### 3.2 模块二：ConnectionAwaiterRegistry

- 模块职责：管理 read/write/close awaiter 的注册、恢复、取消
- 为什么需要：协程桥接必须保留 reactor 线程语义，但 coroutine handle 存储与恢复不应该挤在 `TcpConnection` 主体里
- 依赖哪些模块：`EventLoop`、`std::coroutine_handle`
- 哪些模块依赖它：`TcpConnection` 的 `ReadAwaitable`、`WriteAwaitable`、`CloseAwaitable`
- 暴露什么能力：`arm*Waiter()`、`resume*()`、`cancel*Waiter()`
- 隐藏什么复杂性：单等待者约束、close 清理、恢复必须 `queueInLoop()`
- 系统地位：内部 coroutine bookkeeping 层

典型入口：

- `TcpConnection::armReadWaiter()`
- `TcpConnection::armWriteWaiter()`
- `TcpConnection::armCloseWaiter()`
- `TcpConnection::handleRead()`
- `TcpConnection::finishPendingWrite()`
- `TcpConnection::runCloseSequence()`

关键数据结构：

- `ReadAwaiterState`：额外保存 `minBytes`
- `WaiterState`：write/close 通用 waiter 状态

最容易误解的点：

- `await_ready()` 的 fast path 只在 owner loop 上判断；真正注册 waiter 时仍会回流到 owner loop。
- registry 不直接 `resume()` 协程，而是 `loop_->queueInLoop([handle] { handle.resume(); })`。
- `cancel*Waiter()` 不是静默清理，而是通过恢复协程让上层在 `await_resume()` 里看到 `NetError::Cancelled`。

阅读建议：
把它当成“每个连接一个小型 awaiter 状态机”，重点看 `armReadWaiter()`、`resumeAllOnClose()`、`queueResume()`。

### 3.3 模块三：ConnectionBackpressureController

- 模块职责：根据 output buffer 大小执行高低水位背压
- 为什么需要：背压不是连接状态本身，而是连接处于高输出积压时的临时读节流策略
- 依赖哪些模块：`EventLoop`、`Channel`
- 哪些模块依赖它：`TcpConnection`
- 暴露什么能力：阈值校验、配置、连接建立时激活、buffer 变化时应用、关闭时清理
- 隐藏什么复杂性：迟滞控制、策略启停、重新启用读事件
- 系统地位：内部流控策略层

典型入口：

- `TcpConnection::setBackpressurePolicy()`
- `TcpConnection::connectEstablished()`
- `TcpConnection::sendInLoop()`
- `TcpConnection::handleWrite()`
- `TcpConnection::runCloseSequence()`

关键数据结构：

- `highWaterMark_`
- `lowWaterMark_`
- `active_`
- `readingEnabled_`

最容易误解的点：

- 它控制的是“是否继续从 socket 读”，不是“是否允许写”。
- `highWaterMark == 0` 表示禁用策略，不是零阈值背压。
- `onClosed()` 只是结束策略状态，不负责重新注册/移除 channel。

阅读建议：
先看 `validateThresholds()`，再看 `apply()` 里的 3 个分支：未激活、禁用策略、迟滞切换。

### 3.4 模块四：ConnectionCallbackDispatcher

- 模块职责：统一持有连接相关 callback，并决定立即调用还是排队调用
- 为什么需要：callback slot 本身不复杂，但把它们从 `TcpConnection` 主体挪出后，连接主路径更容易阅读
- 依赖哪些模块：`Callbacks.h`、`EventLoop`
- 哪些模块依赖它：`TcpConnection`
- 暴露什么能力：设置 callback、发送 connected/disconnected/message/close 通知、排队 high-water/write-complete 通知
- 隐藏什么复杂性：高水位只在跨越阈值时触发、write complete 通过 owner loop 排队而不是内联重入
- 系统地位：内部分发/胶水层

典型入口：

- `TcpConnection::set*Callback()`
- `TcpConnection::notifyConnected()`
- `TcpConnection::notifyDisconnected()`
- `TcpConnection::finishPendingWrite()`
- `TcpConnection::maybeQueueHighWaterMark()`

最容易误解的点：

- `notifyConnected()` 与 `notifyDisconnected()` 是同步调用；`queueWriteComplete()` 与 `queueHighWaterMark()` 是异步排队。
- `notifyMessage(..., suppressed)` 中的 `suppressed` 是为 coroutine read waiter 服务的，避免“同一批数据同时喂给 awaiter 和 message callback”。
- 这个类目前没有独立 intent 文件，说明它仍然是一个轻量胶水模块；如果后续继续长大，值得补 intent 和更直接的单测。

阅读建议：
它很短，直接结合 `TcpConnection::finishPendingWrite()`、`handleRead()` 一起看效果最好。

## 4. 启动流程 / 生命周期分析

这一层没有自己的“程序入口”，它的生命从 `TcpConnection` 构造开始。

### 4.1 创建阶段

`mini/net/TcpConnection.cc` 中的 `TcpConnection::Impl` 构造时，会同时创建 4 个 helper：

```text
TcpConnection::Impl
  ├── callbacks    : unique_ptr<ConnectionCallbackDispatcher>
  ├── backpressure : unique_ptr<ConnectionBackpressureController>
  ├── awaiters     : unique_ptr<ConnectionAwaiterRegistry>
  └── transport    : unique_ptr<ConnectionTransport>
```

这一步说明：

- owner 是 `TcpConnection`
- helper 与连接同生共死
- helper 不共享给别的连接

### 4.2 连接建立阶段

`connectEstablished()` 做 4 件关键事：

1. 状态切到 `kConnected`
2. `channel->tie(shared_from_this())`
3. `channel->enableReading()`
4. `backpressure->onConnectionEstablished(...)`

如果之前已经 `startTls()`，这时不会立刻对外报告 connected，而是先进入 `advanceTransportHandshake()`。

### 4.3 稳定运行阶段

稳定运行期里，4 个 helper 的分工如下：

- `ConnectionTransport` 处理读写和 TLS 握手推进
- `ConnectionAwaiterRegistry` 处理 coroutine suspend/resume/cancel
- `ConnectionBackpressureController` 处理 outputBuffer 变化后的读节流
- `ConnectionCallbackDispatcher` 处理 callback 的同步/异步派发

`TcpConnection` 负责在这些 helper 之间编排顺序，并决定是否进入 `handleClose()` / `handleError()`。

### 4.4 关闭阶段

关闭统一收敛到 `runCloseSequence()`：

1. `state = kDisconnected`
2. `backpressure->onClosed()`
3. `channel->disableAll()`
4. `awaiters->resumeAllOnClose()`
5. `notifyDisconnected(connection)`
6. 需要时 `notifyClose(connection)`

这里最关键的设计是：

- helper 可以清理自己的局部状态
- 真正的连接断开通知顺序仍由 `TcpConnection` 控制
- `channel->remove()` 不是 helper 做，而是后续 `connectDestroyed()` 做

### 4.5 销毁阶段

最终在 `connectDestroyed()` 中 `channel->remove()`，随后 `TcpConnection` 析构。  
helper 作为 `unique_ptr` 成员一起析构，其中 `ConnectionTransport::~ConnectionTransport()` 会释放 `SSL*`。

## 5. 主调用链分析

### 5.1 主链路一：连接建立到 TLS 握手完成

```text
TcpConnection::startTls()
  -> transport->enableTls(fd, ctx, isServer, hostname)

TcpConnection::connectEstablished()
  -> channel->enableReading()
  -> backpressure->onConnectionEstablished(...)
  -> transport->handshakePending() ? advanceTransportHandshake() : notifyConnected()

TcpConnection::advanceTransportHandshake()
  -> transport->advanceHandshake(channel)
     -> SSL_do_handshake()
     -> WANT_READ / WANT_WRITE 时更新 channel interest
  -> failed    -> handleError()
  -> completed -> notifyConnected()
```

这一条链路说明：

- transport helper 决定 TLS 局部状态
- 但“连接什么时候算 connected 并通知用户”仍由 `TcpConnection` 决定

### 5.2 主链路二：收到数据时，回调模式与协程模式如何分流

```text
EPOLLIN
  -> Channel::handleEvent()
  -> TcpConnection::handleRead()
  -> hasReadWaiter = awaiters->hasReadWaiter()
  -> result = transport->readInto(inputBuffer, channel)

  result == kOk:
    -> awaiters->resumeReadWaiterIfSatisfied(inputBuffer.readableBytes())
    -> callbacks->notifyMessage(connection, &inputBuffer, hasReadWaiter)

  result == kPeerClosed:
    -> handleClose()

  result == kError:
    -> handleError(savedErrno)
```

这一条链路里最重要的点是：

- 数据总是先进入 `inputBuffer`
- 有 read waiter 时优先尝试恢复协程
- callback 模式不会和 awaiter 模式重复消费同一批数据

### 5.3 主链路三：发送数据与背压联动

```text
TcpConnection::send()
  -> 非 owner 线程时 runInLoop(sendInLoop)

TcpConnection::sendInLoop()
  -> transport->writeRaw(...)
  -> 若未写完，剩余数据 append 到 outputBuffer
  -> channel->enableWriting()
  -> callbacks->queueHighWaterMark(...)
  -> backpressure->onBufferedBytesChanged(outputBuffer.readableBytes(), channel)

EPOLLOUT
  -> TcpConnection::handleWrite()
  -> transport->writeFromBuffer(outputBuffer, channel)
  -> backpressure->onBufferedBytesChanged(...)
  -> outputBuffer 空
     -> channel->disableWriting()
     -> finishPendingWrite()
        -> callbacks->queueWriteComplete(...)
        -> awaiters->resumeWriteWaiterIfNeeded()
        -> 若 state == kDisconnecting，再 shutdownInLoop()
```

这一条链路说明：

- 背压控制是跟着 output buffer 走的
- write complete callback 和 write awaiter 都在“真正写空”后触发
- 触发后仍然通过 owner loop 的排队语义来降低重入复杂度

### 5.4 主链路四：协程取消与关闭收敛

```text
co_await connection->asyncReadSome()
  -> ReadAwaitable::await_suspend(handle)
  -> TcpConnection::armReadWaiter(handle, minBytes)
  -> awaiters->armReadWaiter(...)

CancellationToken fired
  -> TcpConnection::cancelReadWaiter(handle, state)
  -> awaiters->cancelReadWaiter(handle)
  -> queueResume(handle)

or connection close
  -> TcpConnection::runCloseSequence()
  -> awaiters->resumeAllOnClose()
  -> queueResume(handle)

resume
  -> ReadAwaitable::await_resume()
  -> 取消时返回 NetError::Cancelled
  -> 关闭时返回 PeerClosed / ConnectionReset
```

这里体现的是：

- registry 只负责恢复句柄
- 语义结果在 awaitable 的 `await_resume()` 里统一解释

## 6. 关键数据流分析

### 6.1 所有权流

```text
TcpConnection
  owns Impl
    owns callbacks/backpressure/awaiters/transport (unique_ptr)
```

这意味着：

- helper 没有共享所有权
- helper 生命周期天然受 `TcpConnection` 约束
- helper 不会脱离连接单独存活

### 6.2 线程流

```text
cross-thread caller
  -> TcpConnection public API
  -> runInLoop / queueInLoop
  -> owner EventLoop thread
  -> helper mutate state
```

这意味着：

- helper 本身不建立第二线程域
- 所有可变状态都回到同一个 owner loop
- 协程恢复也不例外

### 6.3 输入数据流

```text
kernel socket
  -> ConnectionTransport::readInto()
  -> TcpConnection::inputBuffer
  -> ConnectionAwaiterRegistry::resumeReadWaiterIfSatisfied()
  -> or ConnectionCallbackDispatcher::notifyMessage()
```

输入数据的归宿永远先是 `inputBuffer`，不会被 helper 直接交给用户层。

### 6.4 输出数据流

```text
user payload
  -> TcpConnection::sendInLoop()
  -> ConnectionTransport::writeRaw()
  -> unwritten tail -> outputBuffer
  -> ConnectionBackpressureController::apply()
  -> ConnectionTransport::writeFromBuffer()
  -> drain complete -> write callback / write awaiter
```

这里最核心的共享状态是 `outputBuffer`，但它仍由 `TcpConnection` 持有，helper 只是观察或消费它。

### 6.5 错误传播流

```text
transport error
  -> result.savedErrno
  -> TcpConnection::handleError()
  -> closeReason = kError
  -> handleClose()
  -> runCloseSequence()
  -> await_resume() 看到 ConnectionReset
```

这条数据流非常重要，因为它说明 helper 不会发明旁路错误模型。

## 7. 扩展点、修改点与排查点

### 7.1 适合扩展的位置

- 要扩展 TLS/transport 行为：优先看 `ConnectionTransport`
- 要扩展 coroutine waiter 语义：优先看 `ConnectionAwaiterRegistry`
- 要扩展背压策略：优先看 `ConnectionBackpressureController`
- 要扩展 callback 投递与阈值通知：优先看 `ConnectionCallbackDispatcher`

### 7.2 改这里时必须先问的 5 个问题

1. 这个状态到底归 helper，还是归 `TcpConnection` public lifecycle？
2. 这个改动是否仍只在 owner loop 线程上修改状态？
3. close/error 是否仍收敛到 `runCloseSequence()`？
4. 协程恢复是否仍通过 `queueInLoop()`？
5. 对应的 unit/contract/integration 测试在哪里补？

### 7.3 最容易出 bug 的位置

- `ConnectionTransport` 里 TLS `WANT_READ/WANT_WRITE` 与 channel interest 的联动
- `ConnectionAwaiterRegistry` 里 duplicate waiter 与取消恢复的一次性语义
- `ConnectionBackpressureController` 里 high/low water hysteresis
- `ConnectionCallbackDispatcher` 里同步回调与异步排队回调的重入差异

## 8. 测试映射

### 8.1 直接 unit 测试

- `tests/unit/net/test_connection_transport.cpp`
  - 验证 plain transport 的基本读写与 outputBuffer flush
- `tests/unit/net/test_connection_awaiter_registry.cpp`
  - 验证 read waiter 恢复线程、重复 waiter 拒绝、close 恢复、cancel 恢复
- `tests/unit/net/test_connection_backpressure_controller.cpp`
  - 验证阈值校验、暂停读取、恢复读取、禁用策略

### 8.2 间接 contract / integration 测试

- `tests/contract/tcp_connection/test_tcp_connection.cpp`
  - 验证 awaiter 错误语义、背压策略、关闭收敛、取消路径
- `tests/integration/tcp_server/test_tcp_server_backpressure.cpp`
  - 验证 server 场景下高水位回调与背压行为
- `tests/integration/coroutine/test_coroutine_echo_server.cpp`
  - 验证协程恢复发生在正确 worker loop 线程

## 9. 推荐阅读顺序

如果你第一次读这一层，推荐顺序是：

1. `intents/modules/tcp_connection.intent.md`
2. `intents/modules/connection_transport.intent.md`
3. `intents/modules/connection_awaiter_registry.intent.md`
4. `intents/modules/connection_backpressure_controller.intent.md`
5. `mini/net/TcpConnection.h`
6. `mini/net/TcpConnection.cc`
7. `mini/net/detail/ConnectionTransport.h/.cc`
8. `mini/net/detail/ConnectionAwaiterRegistry.h/.cc`
9. `mini/net/detail/ConnectionBackpressureController.h/.cc`
10. `mini/net/detail/ConnectionCallbackDispatcher.h/.cc`
11. `tests/unit/net/test_connection_*.cpp`
12. `tests/contract/tcp_connection/test_tcp_connection.cpp`

如果你只想快速抓主线，请优先看这三段：

- `TcpConnection::connectEstablished()` 到 `advanceTransportHandshake()`
- `TcpConnection::handleRead()` 到 `awaiters->resumeReadWaiterIfSatisfied()`
- `TcpConnection::sendInLoop()` / `handleWrite()` 到 `backpressure->onBufferedBytesChanged()`

## 10. 一句话结论

`mini/net/detail/` 的本质不是“内部杂项”，而是 `TcpConnection` 的内部协作者层：它把 transport、coroutine、backpressure、callback dispatch 这 4 个变化轴从连接生命周期中心里拆出来，但又严格受同一个 owner loop、同一条 close/error 收敛路径和同一套所有权纪律约束。
