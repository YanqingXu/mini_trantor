# mini-trantor 框架理解文档

## 0. 文档摘要

- `mini-trantor` 是一个面向学习、演进和 AI 协作开发的 C++ Reactor 风格网络库。
- 它解决的核心问题不是“功能很多”，而是“结构正确”：线程归属明确、生命周期可推理、回调上下文可预测。
- 项目把 `intent -> rules -> tests -> implementation` 放在代码之前，代码只是设计意图的实现载体。
- 当前真正参与构建和运行的核心代码位于 `mini/`，尤其是 `mini/net/`；`src/` 更像早期原型残留，不是当前主线。
- v1 已落地的核心模块包括 `EventLoop`、`Channel`、`Poller/EPollPoller`、`Buffer`、`Acceptor`、`TcpConnection`、`TcpServer`、`EventLoopThread`、`EventLoopThreadPool`。
- 协程能力已经有最小可运行形态，但它是“贴着 Reactor 语义扩展”的，不是另起一套调度体系。
- 阅读这个项目时，最先要抓住的是三个约束：一个线程一个 `EventLoop`、跨线程操作必须回流到 owner loop、销毁前必须解除注册。
- 与许多“先写代码再补文档”的小项目不同，本项目的 `intents/` 和 `rules/` 不是附属资料，而是理解实现边界的第一入口。
- 当前项目最像“工业风缩小版 trantor 骨架”，而不是完整生产框架：它更强调模块边界、线程模型和生命周期纪律。
- 如果只想快速理解主线，先看 `examples/echo_server/main.cpp`，再回读 `EventLoop`、`Channel`、`EPollPoller`、`TcpConnection`、`TcpServer`。

## 1. 框架整体定位

### 1.1 它是什么

`mini-trantor` 是一个小型、工业风、Reactor 风格的 C++ 网络库。它所在的技术领域是网络运行时基础设施，不是业务框架，也不是协议栈框架。

### 1.2 它负责什么

- 提供单线程 Reactor 核心：`EventLoop`
- 提供 fd 事件绑定抽象：`Channel`
- 提供 I/O 多路复用抽象与 epoll 后端：`Poller` / `EPollPoller`
- 提供 TCP 连接生命周期管理：`TcpConnection`
- 提供 TCP 服务端监听、接入与连接分发：`Acceptor` / `TcpServer`
- 提供 one-loop-per-thread 扩展模型：`EventLoopThread` / `EventLoopThreadPool`
- 提供最小协程适配点：`mini::coroutine::Task` 与 `TcpConnection` 上的 awaitable

### 1.3 它不负责什么

- 不负责 HTTP/WebSocket/RPC 等高层协议
- 不负责业务路由、序列化、鉴权、服务治理
- 不负责复杂的协程取消树或独立协程调度器
- 不负责跨平台后端统一，当前实际后端只有 epoll
- 不负责隐藏线程语义，反而刻意让线程归属暴露且可推理

### 1.4 最核心的抽象是什么

最核心的抽象不是 `TcpServer`，而是 `EventLoop + Channel + Poller` 这组三件套：

- `EventLoop` 决定“谁拥有调度权”
- `Poller` 决定“谁感知内核事件”
- `Channel` 决定“哪个 fd 对应哪些语义回调”

上层的 `Acceptor`、`TcpConnection`、`TcpServer` 都是在这组三件套上叠加出来的 TCP 生命周期模型。

### 1.5 它的运行模型是什么

运行模型是标准但约束很强的 Reactor：

1. 一个线程拥有一个 `EventLoop`
2. `EventLoop` 持有一个 `Poller`
3. `Poller` 等待内核 I/O 事件
4. 活跃事件回填成 `Channel*`
5. `EventLoop` 分发 `Channel` 回调
6. 回调进一步驱动 `Acceptor` / `TcpConnection`
7. 跨线程请求通过 `runInLoop` / `queueInLoop` 回流到 owner loop
8. `eventfd` 用作 wakeup 中断机制

### 1.6 这个框架处于什么层级

它是基础设施层、偏通用能力，不是业务承载层。使用者通常通过：

- 直接创建 `EventLoop`
- 创建 `TcpServer`
- 注册连接回调和消息回调
- 调用 `start()` + `loop()`

来接入它。

## 2. 目录结构总览

### 2.1 顶层目录树摘要

```text
.
├── AGENTS.md
├── CMakeLists.txt
├── README.md
├── docs/
├── examples/
├── intents/
├── mini/
│   ├── base/
│   ├── coroutine/
│   └── net/
├── rules/
├── src/
└── tests/
```

### 2.2 各目录职责

#### `mini/`

当前核心运行时代码目录，也是实际构建进入 `mini_trantor` 库的源码位置。

- `mini/base/`：基础小工具层，放极轻量且稳定的通用构件
- `mini/coroutine/`：协程承载层，目前只有一个最小 `Task`
- `mini/net/`：真正的网络库主线，实现 Reactor、TCP 接入与线程池扩展

这个目录存在的原因是把“当前有效实现”从历史实验目录中分离出来，形成更清晰的运行时主线。

#### `intents/`

设计意图层，是这个项目区别于普通代码库的关键目录。

- `architecture/`：系统级 intent，如线程模型、生命周期规则、整体架构
- `modules/`：模块级 intent，如 `EventLoop`、`Channel`、`TcpConnection`
- `usecases/`：用例级 intent，如 echo server

它回答的是“为什么要这样设计”，不是“代码怎么写”。

#### `rules/`

项目级约束层。这里定义线程亲和、所有权、测试、代码风格、框架理解文档等规则。  
它存在的原因是把 AI 和人类开发者都约束在同一套工程纪律里。

#### `tests/`

契约验证层。当前测试不是海量覆盖，而是围绕公共契约、线程行为、生命周期行为、主路径集成来布置。

#### `examples/`

示例层，用最小可运行程序验证框架是否真的“活起来”。

- `echo_server/`：普通回调式 echo server
- `coroutine_echo_server/`：协程式 echo server

#### `docs/`

工程理解文档层，存放生命周期说明、整改清单，以及本次生成的框架理解文档。

#### `src/`

历史原型/过渡目录。这里有非常早期的 `EventLoop` / `Channel` / `Poller` 草图，但它们未进入当前库构建。  
阅读时不要把 `src/` 误认为当前实现，否则会对项目结构产生错误认知。

#### `include/`

当前为空，说明项目暂时没有把“公共头文件发布布局”和“内部源码布局”分离。

### 2.3 目录边界总结

- `intents/` 和 `rules/` 是设计与约束层
- `mini/` 是当前运行时实现层
- `tests/` 是契约验证层
- `examples/` 是接入与演示层
- `docs/` 是解释与沉淀层
- `src/` 是历史痕迹，不是当前主线

## 3. 核心模块地图

### 3.1 模块一：Intent / Rules 设计约束层

- 模块职责：定义系统意图、线程模型、生命周期纪律、评审顺序和测试要求
- 为什么需要：这个项目强调“先解释系统，再写实现”，没有这一层，代码会退化成普通练习仓库
- 依赖谁：几乎不依赖运行时代码
- 谁依赖它：所有实现、测试、文档和后续演进
- 暴露什么能力：设计边界、评审基准、AI 工作流约束
- 隐藏什么复杂性：把大量隐性工程约束显式化
- 系统地位：核心支撑层
- 典型入口：`intents/architecture/system_overview.intent.md`
- 易错点：把它当成“参考资料”而不是“硬约束”
- 阅读建议：先读 architecture，再读 modules，再读 rules

### 3.2 模块二：Reactor 核心调度层

- 模块职责：事件等待、事件分发、跨线程任务回流
- 组成：`EventLoop`、`Channel`、`Poller`、`EPollPoller`
- 为什么需要：这是整个库的运行时心脏，没有它就没有可解释的 I/O 驱动模型
- 依赖谁：基础时间戳、epoll/eventfd、少量标准库
- 谁依赖它：`Acceptor`、`TcpConnection`、线程池、协程恢复点
- 暴露什么能力：fd 注册、事件回调、跨线程回流、wake up
- 隐藏什么复杂性：内核 epoll 细节、活跃事件回填、任务队列唤醒机制
- 系统地位：绝对核心层
- 典型入口类：`EventLoop`
- 关键数据结构：`activeChannels_`、`pendingFunctors_`、`Channel::events_ / revents_`
- 易错点：把 `Channel` 理解成 socket 或把 `Poller` 理解成回调执行者

### 3.3 模块三：Socket / 地址 / 缓冲区支撑层

- 模块职责：包装底层 socket、地址、字节缓存和系统调用边界
- 组成：`Socket`、`SocketsOps`、`InetAddress`、`Buffer`
- 为什么需要：把内核 API 与上层 Reactor/TCP 生命周期解耦
- 依赖谁：POSIX socket API
- 谁依赖它：`Acceptor`、`TcpConnection`
- 暴露什么能力：创建监听 fd、accept、shutdown、读写缓冲、地址转换
- 隐藏什么复杂性：`accept4`、`readv`、`setsockopt`、地址转换
- 系统地位：支撑层
- 关键流程：`Buffer::readFd/writeFd`、`Socket::accept`、`SocketsOps::*`
- 易错点：以为 `Buffer` 带线程安全，或者以为 `Socket` 管理完整连接语义

### 3.4 模块四：TCP 连接与服务端生命周期层

- 模块职责：监听、建连、消息分发、关闭、销毁、连接映射维护
- 组成：`Acceptor`、`TcpConnection`、`TcpServer`
- 为什么需要：把“fd 事件”提升为“连接生命周期事件”
- 依赖谁：Reactor 核心层 + 支撑层
- 谁依赖它：examples、未来业务层、协程示例
- 暴露什么能力：搭建 TCP server、注册连接/消息/关闭回调、连接分发到 worker loop
- 隐藏什么复杂性：close/error 收敛、连接从 base loop 到 io loop 的交接
- 系统地位：核心业务承载前的网络会话层
- 典型入口类：`TcpServer`、`TcpConnection`
- 易错点：混淆 base loop 与 io loop；错误理解 `TcpServer` 是否拥有连接对象的所有销毁时机

### 3.5 模块五：线程扩展层

- 模块职责：把单 loop 扩展为 one-loop-per-thread
- 组成：`EventLoopThread`、`EventLoopThreadPool`
- 为什么需要：让框架从“单线程教学版”过渡到“多 loop 扩展版”
- 依赖谁：`EventLoop`
- 谁依赖它：`TcpServer`
- 暴露什么能力：后台线程创建 loop、worker loop 发布、轮转分配连接
- 隐藏什么复杂性：线程启动同步、loop 指针发布、quit + join
- 系统地位：扩展层
- 易错点：误以为线程池会共享连接状态；实际上它只分发 loop，不共享连接可变状态

### 3.6 模块六：协程集成层

- 模块职责：在不破坏 Reactor 线程纪律的前提下，提供最小 co_await 能力
- 组成：`mini/coroutine/Task.h`、`TcpConnection::ReadAwaitable/WriteAwaitable/CloseAwaitable`
- 为什么需要：验证“协程可以叠加在 EventLoop 之上，而不是取代它”
- 依赖谁：`TcpConnection`、C++20 coroutine
- 谁依赖它：`examples/coroutine_echo_server`
- 暴露什么能力：最小任务对象、异步读、异步写、等待关闭
- 隐藏什么复杂性：恢复点仍然通过 `EventLoop::queueInLoop` 回到 owner loop
- 系统地位：实验性扩展层
- 易错点：把它误解为独立协程调度器；实际上它只是 EventLoop 语义上的 await 包装

### 3.7 模块七：示例与测试层

- 模块职责：验证主链路、暴露用法、固定契约
- 为什么需要：这个项目强调 contract，示例和测试就是结构正确性的外部证据
- 组成：`examples/*`、`tests/*`
- 系统地位：验证层
- 阅读建议：用它们反推框架的使用方式和最关键承诺

## 4. 启动流程 / 生命周期分析

### 4.1 程序入口在哪里

当前最典型的程序入口在：

- `examples/echo_server/main.cpp`
- `examples/coroutine_echo_server/main.cpp`

库本身没有“框架全局启动器”，而是要求使用者自己创建 `EventLoop` 并驱动它。

### 4.2 普通 echo server 启动顺序

1. 创建主线程 `EventLoop`
2. 创建 `TcpServer`
3. `TcpServer` 构造时创建：
   - `Acceptor`
   - `EventLoopThreadPool`
   - 生命周期令牌 `lifetimeToken_`
4. 用户设置：
   - `ConnectionCallback`
   - `MessageCallback`
   - 可选线程数和线程初始化回调
5. 调用 `server.start()`
6. `TcpServer::start()`：
   - 原子地确保只启动一次
   - 启动 `EventLoopThreadPool`
   - 通过 `loop_->runInLoop` 把 `acceptor_->listen()` 安排到 base loop 线程
7. 调用 `loop.loop()`
8. `EventLoop` 进入稳定运行态：`poll -> dispatch -> doPendingFunctors -> repeat`

### 4.3 连接进入系统的过程

1. 客户端连接到 listen socket
2. epoll 返回 listen fd 可读
3. `EventLoop` 分发到 `Acceptor::handleRead`
4. `Acceptor` 循环 `accept`
5. 每个新 fd 通过 `newConnectionCallback_` 交给 `TcpServer::newConnection`
6. `TcpServer` 选择一个 io loop
7. 构造 `TcpConnection`
8. 把连接存入 base loop 的 `connections_`
9. 在 io loop 上执行 `connectEstablished()`
10. `TcpConnection` tie 自己、开启读事件、触发连接建立回调

### 4.4 稳定运行态发生什么

稳定运行态的控制权主要在 `EventLoop` 手里：

- I/O 事件来自 `Poller`
- 回调分发由 `Channel`
- 连接状态转移由 `TcpConnection`
- 新连接接入由 `Acceptor`
- 连接表维护由 `TcpServer`
- 跨线程任务靠 `queueInLoop/runInLoop + eventfd wakeup`

### 4.5 关闭与销毁流程

#### 单个连接关闭

1. `handleRead()` 读到 0，或读写错误进入 `handleError()`
2. 错误路径收敛到 `handleClose()`
3. `TcpConnection`：
   - 状态设为 `kDisconnected`
   - `channel_->disableAll()`
   - 恢复所有 waiters
   - 触发 connection callback
   - 触发 close callback
4. `TcpServer::removeConnection()` 把移除操作回流到 base loop
5. base loop 从 `connections_` 擦除该连接
6. io loop 上 `queueInLoop(connectDestroyed())`
7. `connectDestroyed()` 最终 `channel_->remove()`

#### 服务端销毁

1. `TcpServer` 析构必须在 base loop 线程
2. 重置 `lifetimeToken_`，避免晚到的回调再解引用已销毁的 server
3. 清空 `Acceptor` 的新连接回调
4. 对所有连接在各自 io loop 上安排：
   - 先清空 close callback
   - 再执行 `connectDestroyed()`

#### EventLoop 销毁

`EventLoop` 析构要求：

- 必须在 owner thread
- 不能在 `loop()` 仍运行时析构
- 先移除 wakeup channel，再关闭 eventfd

这说明该项目非常强调“销毁也是调度语义的一部分”。

## 5. 主调用链分析

### 5.1 主链路一：普通 echo server 请求链路

#### 阶段 A：监听建立

1. `examples/echo_server/main.cpp` 创建 `TcpServer`
2. `TcpServer::start()` 安排 `Acceptor::listen()`
3. `Acceptor::listen()` 调用 `Socket::listen()` 并让 `acceptChannel_` 订阅读事件

#### 阶段 B：新连接进入

1. listen fd 可读
2. `EventLoop::loop()` 从 `Poller::poll()` 得到活跃 `Channel`
3. `Channel::handleEvent()` 调用 `Acceptor::handleRead()`
4. `Acceptor` accept 出 `connfd`
5. `TcpServer::newConnection()`：
   - 选 io loop
   - 构造 `TcpConnection`
   - 存入 `connections_`
   - 设置各种回调
   - 在 io loop 上 `connectEstablished()`

#### 阶段 C：读消息

1. conn fd 可读
2. 对应 `TcpConnection` 的 `channel_` 收到可读事件
3. `TcpConnection::handleRead()` 调用 `inputBuffer_.readFd()`
4. 如果读到字节，执行 `messageCallback_`
5. echo 示例中的消息回调把 `buffer->retrieveAllAsString()` 原样 `send()` 回去

#### 阶段 D：写回

1. `TcpConnection::send()` 判断当前线程
2. 若在 owner loop，则直接 `sendInLoop()`
3. 优先尝试直接 `write`
4. 若没写完，则剩余数据进入 `outputBuffer_`
5. 开启 `channel_->enableWriting()`
6. 下次可写事件到来时由 `handleWrite()` 继续发送
7. 缓冲清空后关闭写关注，并触发 `writeCompleteCallback_`

#### 阶段 E：关闭

1. 对端关闭或错误触发 `handleClose()/handleError()`
2. `closeCallback_` 回到 `TcpServer`
3. base loop 擦除连接
4. io loop 完成 `connectDestroyed()`

### 5.2 主链路二：跨线程任务回流链路

这是理解线程模型的关键链路。

1. 非 owner 线程调用 `EventLoop::runInLoop(fn)`
2. 因为不在 loop 线程，内部转为 `queueInLoop(fn)`
3. functor 进入 `pendingFunctors_`
4. `EventLoop::wakeup()` 往 `eventfd` 写入 1
5. 正阻塞在 epoll 的 loop 被唤醒
6. wakeup fd 可读，`handleRead()` 把计数读走
7. 当前轮事件分发结束后，`doPendingFunctors()` 执行积压任务
8. 任务最终在 owner loop 线程中运行

这条链路说明：跨线程不是直接改状态，而是“提交任务 + 打断 poll + 回 owner 线程执行”。

### 5.3 主链路三：协程 echo 链路

1. `coroutine_echo_server` 在连接建立回调里 `echoSession(connection).detach()`
2. 协程体中 `co_await connection->asyncReadSome()`
3. 若当前缓冲不满足读取条件，`ReadAwaitable::await_suspend` 调用 `armReadWaiter`
4. waiter 被记录在 `TcpConnection::readWaiter_`
5. 数据到来时 `handleRead()` 读取到 `inputBuffer_`
6. `resumeReadWaiterIfNeeded()` 通过 `queueResume()` 安排协程恢复
7. 恢复动作仍通过 `loop_->queueInLoop` 回到 owner loop
8. 协程拿到消息后 `co_await asyncWrite(...)`
9. 写完成后 `resumeWriteWaiterIfNeeded()` 恢复协程
10. 如果连接关闭，`resumeAllWaitersOnClose()` 统一唤醒等待者

这条链路最重要的点是：协程没有绕开 EventLoop，而是把“恢复”也纳入 EventLoop 调度。

## 6. 关键数据流分析

### 6.1 连接 fd 如何进入系统

- `Acceptor` 从 listen socket accept 出 `connfd`
- `TcpServer::newConnection()` 接住这个 fd
- `TcpConnection` 用它创建：
  - `Socket`
  - `Channel`
- 从这一刻起，fd 的 I/O 生命周期由 `TcpConnection` 驱动

### 6.2 输入数据如何流动

1. fd 可读
2. `TcpConnection::handleRead()`
3. `Buffer::readFd()` 读入 `inputBuffer_`
4. 回调式路径：`messageCallback_(conn, &inputBuffer_)`
5. 协程式路径：`consumeReadableBytes()` 把数据作为字符串取出

数据的实际归属在 `TcpConnection` 内部，而不是直接散落到用户代码。

### 6.3 输出数据如何流动

1. 用户调用 `send()`
2. 若能直接写完，数据直接离开用户态
3. 若写不完，剩余字节进入 `outputBuffer_`
4. `channel_` 开启写关注
5. `handleWrite()` 再次把缓冲写向 fd
6. 清空后关闭写关注，触发写完成回调

### 6.4 运行时状态如何保存

- `EventLoop` 保存：
  - 当前线程 id
  - `Poller`
  - wakeup fd/channel
  - 活跃 channel 列表
  - pending functors
- `Channel` 保存：
  - `fd`
  - `events_`
  - `revents_`
  - tie 关系
  - 各类回调
- `TcpConnection` 保存：
  - 连接状态机
  - `Socket`
  - `Channel`
  - 输入/输出缓冲
  - 用户回调
  - 协程等待状态
- `TcpServer` 保存：
  - 连接表
  - 线程池
  - 生命周期令牌

### 6.5 所有权与观察关系

- `EventLoop` 拥有 `Poller`
- `Poller` 不拥有 `Channel`，只保存 fd 到 `Channel*` 的登记
- `Channel` 不拥有 `EventLoop`，也不默认拥有 fd
- `TcpConnection` 拥有自身 `Socket` 和 `Channel`
- `TcpServer` 通过 `shared_ptr<TcpConnection>` 拥有连接对象
- `Channel::tie()` 通过 `weak_ptr` 观察上层 owner 是否仍活着

### 6.6 是否存在共享状态和线程间传递

项目刻意避免共享可变状态。线程间传递主要是：

- functor 进入 `EventLoop::pendingFunctors_`
- 连接从 base loop 被分发到某个 io loop
- 关闭路径通过 base loop 和 io loop 之间的显式回流协调

### 6.7 错误如何传播

- 低层系统调用错误通过返回值和 `errno` 上浮
- `TcpConnection` 在严重读写错误时显式打印错误并统一走 `handleClose()`
- 该项目当前不追求复杂错误对象建模，而是优先强调状态收敛与安全 teardown

## 7. 文件级职责拆解（重点）

### 文件：`CMakeLists.txt`

#### 1）这个文件的核心职责
定义当前项目的真实构建入口，决定哪些源码会被编进 `mini_trantor`。

#### 2）为什么这个文件必须存在
它是识别“当前主线实现”最可靠的证据。没有它，很容易误把 `src/` 当成核心代码。

#### 3）它在系统中的位置
启动层 / 构建层。

#### 4）它依赖谁
依赖项目目录布局和 CMake。

#### 5）谁依赖它
开发者、CI、本地构建过程。

#### 6）文件中的关键内容
- `add_library(mini_trantor ...)`：说明 `mini/net/*.cc` 才是当前运行时实现
- `add_executable(...)`：示例和测试入口
- `target_link_libraries(... pthread)`：线程模型依赖 pthread

#### 7）阅读这个文件时应该重点看什么
先看 `add_library` 的源码列表，再看测试和 examples 列表。

#### 8）这个文件最容易让人误解的地方
README 中还写着 `src/`，但构建系统已经切换到 `mini/`。

#### 9）如果我要修改/扩展这个文件，应该注意什么
新增模块时必须同步更新构建入口和测试入口，否则 intent 和实现会再次脱节。

### 文件：`mini/net/EventLoop.h` / `mini/net/EventLoop.cc`

#### 1）这个文件的核心职责
实现单线程 Reactor 核心，管理 poll、事件分发、跨线程任务回流和 wakeup。

#### 2）为什么这个文件必须存在
没有它，项目只剩一堆零散 socket 操作，不再是可调度、可推理的事件驱动运行时。

#### 3）它在系统中的位置
核心调度层。

#### 4）它依赖谁
`Poller`、`Channel`、`Timestamp`、`eventfd`、互斥锁和线程库。

#### 5）谁依赖它
几乎所有运行时模块：`Channel`、`Acceptor`、`TcpConnection`、线程池、协程恢复逻辑。

#### 6）文件中的关键类 / 函数
- `EventLoop::loop()`：主循环
- `runInLoop/queueInLoop()`：跨线程回流 API
- `wakeup()`：中断 poll
- `doPendingFunctors()`：在 owner thread 冲刷任务队列
- 构造/析构：建立并清理 wakeup fd 与 wakeup channel

#### 7）阅读这个文件时应该重点看什么
先看成员变量，再看构造函数中 wakeup channel 的建立，最后看 `loop()`、`queueInLoop()` 和析构流程。

#### 8）这个文件最容易让人误解的地方
它看起来像“事件循环容器”，实际上它才是整个线程归属与执行上下文的边界。

#### 9）如果我要修改/扩展这个文件，应该注意什么
任何 timer、metrics、coroutine 恢复、优先级任务扩展都不能破坏当前的单线程 owner-loop 纪律。

### 文件：`mini/net/Channel.h` / `mini/net/Channel.cc`

#### 1）这个文件的核心职责
把一个 fd 的事件兴趣集、返回事件和回调分发绑定在一起。

#### 2）为什么这个文件必须存在
没有它，上层只能直接操纵 epoll 事件和回调映射，业务层与后端细节会纠缠在一起。

#### 3）它在系统中的位置
核心调度层与上层对象之间的桥接层。

#### 4）它依赖谁
`EventLoop`、`Timestamp`、epoll 事件常量。

#### 5）谁依赖它
`EventLoop`、`Acceptor`、`TcpConnection`。

#### 6）文件中的关键类 / 函数
- `enableReading/enableWriting/disableAll()`：维护兴趣集
- `update()`：把注册变化交给 `EventLoop`
- `remove()`：强制要求先 `disableAll()` 再移除
- `handleEvent()`：tie 守卫入口
- `handleEventWithGuard()`：真正的分发逻辑

#### 7）阅读这个文件时应该重点看什么
重点区分 `events_` 与 `revents_`，再看 `tie()` 和析构期的 remove-before-destroy 保护。

#### 8）这个文件最容易让人误解的地方
它不是 socket 封装，也不是 Poller；它本质上是 fd 事件语义对象。

#### 9）如果我要修改/扩展这个文件，应该注意什么
注意不要让后端细节继续上浮。当前它直接包含 `<sys/epoll.h>`，已经有一点 backend leak 风险。

### 文件：`mini/net/Poller.h` / `mini/net/Poller.cc`

#### 1）这个文件的核心职责
定义 I/O 多路复用抽象接口，并维护 fd 到 `Channel*` 的登记表。

#### 2）为什么这个文件必须存在
没有这层抽象，`EventLoop` 就会直接依赖 epoll 细节，未来无法替换后端，也不利于教学解释。

#### 3）它在系统中的位置
核心调度层中的后端抽象接口层。

#### 4）它依赖谁
`Channel`、`EventLoop`、标准容器。

#### 5）谁依赖它
`EventLoop` 和具体后端 `EPollPoller`。

#### 6）文件中的关键类 / 函数
- `poll(...)`
- `updateChannel(...)`
- `removeChannel(...)`
- `hasChannel(...)`
- `newDefaultPoller(...)`

#### 7）阅读这个文件时应该重点看什么
看接口语义，而不是实现细节；尤其是“它不拥有 Channel”这一点。

#### 8）这个文件最容易让人误解的地方
它不是调度器，只是事件来源与注册关系管理器。

#### 9）如果我要修改/扩展这个文件，应该注意什么
新的后端必须保持 `EventLoop` 看到的语义不变，而不是把后端差异继续向上泄漏。

### 文件：`mini/net/EPollPoller.h` / `mini/net/EPollPoller.cc`

#### 1）这个文件的核心职责
实现基于 epoll 的 `Poller` 后端。

#### 2）为什么这个文件必须存在
当前项目要真正跑起来，必须把抽象层落到 Linux epoll。

#### 3）它在系统中的位置
平台适配层 / 后端实现层。

#### 4）它依赖谁
`Poller`、`Channel`、`epoll_*` 系统调用。

#### 5）谁依赖它
`Poller::newDefaultPoller()` 和 `EventLoop`。

#### 6）文件中的关键类 / 函数
- `poll()`：调用 `epoll_wait`
- `updateChannel()`：根据 `Channel::index()` 决定 add/mod/del
- `removeChannel()`：删除登记并清理 index
- `fillActiveChannels()`：把内核事件翻译回 `Channel*`

#### 7）阅读这个文件时应该重点看什么
先看 `kNew/kAdded/kDeleted` 状态，再看 `updateChannel()` 如何与 `Channel::index_` 协作。

#### 8）这个文件最容易让人误解的地方
它看起来不大，但它决定了注册关系和实际内核状态是否一致，是运行时正确性的关键文件。

#### 9）如果我要修改/扩展这个文件，应该注意什么
最危险的是让 `channels_` 与内核 epoll 状态分叉，或者让已删除 channel 仍可能回流为活跃事件。

### 文件：`mini/net/Acceptor.h` / `mini/net/Acceptor.cc`

#### 1）这个文件的核心职责
管理监听 socket 与 accept 路径，把新连接 fd 向上交给 `TcpServer`。

#### 2）为什么这个文件必须存在
它把“监听接入”从“连接生命周期”中拆出来，避免 `TcpServer` 同时承担太多底层 fd 细节。

#### 3）它在系统中的位置
TCP 接入层 / 胶水层。

#### 4）它依赖谁
`EventLoop`、`Socket`、`Channel`、`InetAddress`、`SocketsOps`。

#### 5）谁依赖它
`TcpServer`。

#### 6）文件中的关键类 / 函数
- 构造函数：创建 listen fd、绑定地址、设置读回调
- `listen()`：真正开始监听并注册读事件
- `handleRead()`：循环 accept 并交付或关闭 fd
- 析构：确保 owner thread 上解除监听注册

#### 7）阅读这个文件时应该重点看什么
先看构造，再看 `listen()`，最后看 `handleRead()` 中“无回调则显式 close fd”的处理。

#### 8）这个文件最容易让人误解的地方
它不是 server 本身，只是 listenfd 适配器。

#### 9）如果我要修改/扩展这个文件，应该注意什么
不要把连接分配策略、业务协议逻辑塞进来；它应该始终是个薄层。

### 文件：`mini/net/TcpConnection.h` / `mini/net/TcpConnection.cc`

#### 1）这个文件的核心职责
表示单个 TCP 连接，统一管理状态机、socket、channel、buffer、用户回调和协程等待状态。

#### 2）为什么这个文件必须存在
没有它，连接层的读、写、关闭、错误、缓冲、回调、销毁会散落在多个类里，生命周期将不可控。

#### 3）它在系统中的位置
核心连接生命周期层。

#### 4）它依赖谁
`EventLoop`、`Socket`、`Channel`、`Buffer`、`InetAddress`、`SocketsOps`、协程库。

#### 5）谁依赖它
`TcpServer`、用户回调、协程示例。

#### 6）文件中的关键类 / 函数
- 状态机：`kConnecting/kConnected/kDisconnecting/kDisconnected`
- `connectEstablished()/connectDestroyed()`：建立与销毁两个关键阶段
- `handleRead()/handleWrite()/handleClose()/handleError()`：连接的主事件入口
- `send()/sendInLoop()`：跨线程安全发送
- `shutdown()/shutdownInLoop()`：半关闭写端
- `ReadAwaitable/WriteAwaitable/CloseAwaitable`：协程接入点

#### 7）阅读这个文件时应该重点看什么
先看成员变量，再看四个事件处理函数，再看 `sendInLoop()`，最后看 awaiter 相关逻辑。

#### 8）这个文件最容易让人误解的地方
它看起来像“一个连接对象”，实际上它是整个项目生命周期最复杂、线程纪律最敏感的类。

#### 9）如果我要修改/扩展这个文件，应该注意什么
最容易出问题的是：
- 跨线程直接读写连接内部状态
- 错误路径与关闭路径分叉
- 协程 waiter 与销毁路径互相踩踏
- channel 未 remove 就进入析构

### 文件：`mini/net/TcpServer.h` / `mini/net/TcpServer.cc`

#### 1）这个文件的核心职责
协调接入、worker loop 选择、连接表维护和跨 loop 移除。

#### 2）为什么这个文件必须存在
没有它，`Acceptor` 只能产生裸 fd，系统缺少把“接入”转成“连接对象管理”的总控边界。

#### 3）它在系统中的位置
服务端会话总控层。

#### 4）它依赖谁
`Acceptor`、`EventLoopThreadPool`、`TcpConnection`、`SocketsOps`。

#### 5）谁依赖它
示例程序和未来业务接入层。

#### 6）文件中的关键类 / 函数
- `start()`：启动监听和线程池
- `newConnection()`：新建连接并分派到 io loop
- `removeConnection()`：跨 loop 回流到 base loop
- `removeConnectionInLoop()`：真正擦除并安排 `connectDestroyed()`
- 析构函数：基于 `lifetimeToken_` 做安全收尾

#### 7）阅读这个文件时应该重点看什么
先看构造函数中 `Acceptor` 回调绑定，再看 `newConnection()` 和两段式 remove 路径。

#### 8）这个文件最容易让人误解的地方
它不直接处理每个连接的 I/O，但它决定连接何时进入、何时退出、在哪个 loop 上生存。

#### 9）如果我要修改/扩展这个文件，应该注意什么
最要警惕的是 base loop 和 io loop 职责混乱，以及关闭回调捕获裸 `this` 导致悬空访问。

### 文件：`mini/net/EventLoopThread.h` / `mini/net/EventLoopThread.cc`

#### 1）这个文件的核心职责
在后台线程中创建、发布并运行一个 `EventLoop`。

#### 2）为什么这个文件必须存在
没有它，`EventLoopThreadPool` 无法形成 one-loop-per-thread 扩展模型。

#### 3）它在系统中的位置
线程扩展层。

#### 4）它依赖谁
`EventLoop`、线程、条件变量。

#### 5）谁依赖它
`EventLoopThreadPool`。

#### 6）文件中的关键类 / 函数
- `startLoop()`：启动线程并等待 loop 发布
- `threadFunc()`：在线程栈上构造 `EventLoop` 并进入 `loop()`
- 析构：调用 `quit()` 并 `join()`

#### 7）阅读这个文件时应该重点看什么
重点看 loop 指针何时发布，以及为什么 `loop_` 指向线程栈上的对象仍然是安全的。

#### 8）这个文件最容易让人误解的地方
它不是线程池，只负责“一条线程 + 一个 EventLoop”。

#### 9）如果我要修改/扩展这个文件，应该注意什么
不要破坏 loop 的发布同步关系；不要让析构在 worker 线程未退出前结束。

### 文件：`mini/net/EventLoopThreadPool.h` / `mini/net/EventLoopThreadPool.cc`

#### 1）这个文件的核心职责
管理多个 `EventLoopThread`，并以轮转策略返回下一个可用 loop。

#### 2）为什么这个文件必须存在
它把“多线程”限制成“多个独立 EventLoop 的可预测分发”，而不是共享状态的线程池。

#### 3）它在系统中的位置
扩展层。

#### 4）它依赖谁
`EventLoop`、`EventLoopThread`。

#### 5）谁依赖它
`TcpServer`。

#### 6）文件中的关键类 / 函数
- `setThreadNum()`：配置 worker 数量
- `start()`：启动所有 worker loop
- `getNextLoop()`：轮转选下一个 io loop
- `getAllLoops()`：返回所有 loop

#### 7）阅读这个文件时应该重点看什么
看 `start()` 的 worker 发布，再看 `getNextLoop()` 的轮转逻辑和零线程回退逻辑。

#### 8）这个文件最容易让人误解的地方
它的“池”并不是任务池，而是 loop 容器。

#### 9）如果我要修改/扩展这个文件，应该注意什么
如果未来加入负载感知或亲和策略，也不能破坏“连接状态只属于某个 loop”这一原则。

### 文件：`mini/net/Buffer.h` / `mini/net/Buffer.cc`

#### 1）这个文件的核心职责
提供连接级字节缓存，维护可读/可写/可预留区域。

#### 2）为什么这个文件必须存在
没有它，读写路径只能直接操作临时数组，无法优雅处理半包、剩余写入和缓冲复用。

#### 3）它在系统中的位置
数据结构层 / 支撑层。

#### 4）它依赖谁
标准容器、`readv/write`。

#### 5）谁依赖它
`TcpConnection` 和消息回调。

#### 6）文件中的关键类 / 函数
- `append()` / `retrieve*()`：缓冲操作
- `ensureWritableBytes()` / `makeSpace()`：空间管理
- `readFd()`：使用双 iovec 进行高效读取
- `writeFd()`：把可读区写入 fd

#### 7）阅读这个文件时应该重点看什么
重点看 `readerIndex_`、`writerIndex_` 与 `makeSpace()` 的搬移逻辑。

#### 8）这个文件最容易让人误解的地方
它不是协议解析器，也不是线程安全容器。

#### 9）如果我要修改/扩展这个文件，应该注意什么
保持索引语义简单可推理，避免为性能引入过早复杂化。

### 文件：`mini/net/Socket.h` / `mini/net/Socket.cc`、`mini/net/SocketsOps.h` / `mini/net/SocketsOps.cc`

#### 1）这个文件的核心职责
封装最底层 socket 系统调用边界，并用 RAII 管理 fd。

#### 2）为什么这个文件必须存在
它把 POSIX C 风格 API 与上层 C++ 对象模型隔开，降低上层噪音。

#### 3）它在系统中的位置
平台适配层 / 工具层。

#### 4）它依赖谁
socket API、`InetAddress`。

#### 5）谁依赖它
`Acceptor`、`TcpConnection`。

#### 6）文件中的关键类 / 函数
- `Socket::accept/bindAddress/listen/shutdownWrite`
- `setReuseAddr/Port/KeepAlive/TcpNoDelay`
- `sockets::createNonblockingOrDie`
- `sockets::getLocalAddr/getPeerAddr/getSocketError`

#### 7）阅读这个文件时应该重点看什么
区分 `Socket` 的 RAII 包装职责与 `SocketsOps` 的纯系统调用职责。

#### 8）这个文件最容易让人误解的地方
它们不是网络层逻辑，只是“最底层能力边界”。

#### 9）如果我要修改/扩展这个文件，应该注意什么
保持它们薄而稳定，不要把业务含义或连接状态机塞进系统调用封装层。

### 文件：`mini/net/InetAddress.h` / `mini/net/InetAddress.cc`

#### 1）这个文件的核心职责
提供 IPv4 地址与端口的轻量包装。

#### 2）为什么这个文件必须存在
没有它，上层会频繁直接处理 `sockaddr_in`，可读性和边界清晰度都会下降。

#### 3）它在系统中的位置
工具层。

#### 4）它依赖谁
`arpa/inet.h` 和 `sockaddr_in`。

#### 5）谁依赖它
`Socket`、`Acceptor`、`TcpConnection`、`TcpServer`、examples。

#### 6）文件中的关键类 / 函数
- 构造函数：支持端口和文本 IP
- `toIp()` / `toIpPort()`
- `getSockAddrInet()` / `setSockAddrInet()`

#### 7）阅读这个文件时应该重点看什么
重点看它的“轻量”属性，而不是把它想成复杂地址抽象。

#### 8）这个文件最容易让人误解的地方
当前只支持 IPv4，不是通用网络地址层。

#### 9）如果我要修改/扩展这个文件，应该注意什么
未来加 IPv6 时最好新增明确抽象，而不是把当前轻量类型拉成巨型兼容层。

### 文件：`mini/coroutine/Task.h`

#### 1）这个文件的核心职责
提供最小可组合的 coroutine 结果对象。

#### 2）为什么这个文件必须存在
没有它，协程示例就缺少统一承载对象，也难以表达 detach / co_await 语义。

#### 3）它在系统中的位置
协程承载层。

#### 4）它依赖谁
标准协程库、异常处理、可选值。

#### 5）谁依赖它
协程示例和未来 await 风格 API。

#### 6）文件中的关键类 / 函数
- `TaskPromiseBase`：异常、continuation、detached 状态
- `FinalAwaiter`：结束时恢复 continuation 或自毁
- `Task::start()` / `detach()` / `operator co_await()`

#### 7）阅读这个文件时应该重点看什么
先看 `initial_suspend/final_suspend`，再看 continuation 与 detach 的关系。

#### 8）这个文件最容易让人误解的地方
它不是调度器，只是 coroutine 结果对象。

#### 9）如果我要修改/扩展这个文件，应该注意什么
不要让它擅自定义线程切换语义，恢复线程仍应该由上层 EventLoop 相关逻辑决定。

### 文件：`examples/echo_server/main.cpp`、`examples/coroutine_echo_server/main.cpp`

#### 1）这个文件的核心职责
展示普通回调式和协程式两种接入方式。

#### 2）为什么这个文件必须存在
它们是理解“框架到底怎么用”的最快入口。

#### 3）它在系统中的位置
示例层 / 启动层。

#### 4）它依赖谁
`EventLoop`、`TcpServer`、`TcpConnection`、`Buffer`、`Task`

#### 5）谁依赖它
第一次接触项目的阅读者。

#### 6）文件中的关键内容
- 创建 loop 和 server
- 设置连接/消息回调
- 调用 `start()` 和 `loop()`
- 协程版本展示 `asyncReadSome/asyncWrite`

#### 7）阅读这个文件时应该重点看什么
看最短接入路径，而不是追求功能复杂度。

#### 8）这个文件最容易让人误解的地方
示例非常短，但它已经覆盖了项目最关键的主调用链。

#### 9）如果我要修改/扩展这个文件，应该注意什么
示例最好继续承担“教学入口”角色，不要把它膨胀成复杂业务 demo。

### 文件：`tests/test_event_loop.cpp`、`tests/test_poller_contract.cpp`、`tests/test_tcp_connection.cpp`、`tests/test_tcp_server.cpp`

#### 1）这个文件的核心职责
固定当前最重要的线程、注册、连接、服务端主路径契约。

#### 2）为什么这个文件必须存在
项目强调“tests are contract enforcement”，这些测试就是公共语义的可执行说明。

#### 3）它在系统中的位置
测试层 / 契约层。

#### 4）它依赖谁
核心运行时模块。

#### 5）谁依赖它
开发者、评审者、未来重构者。

#### 6）文件中的关键内容
- `test_event_loop.cpp`：验证 same-thread immediate 与 cross-thread queue 回流
- `test_poller_contract.cpp`：验证注册、触发、remove 语义
- `test_tcp_connection.cpp`：验证 socketpair 下的连接建立、消息读取、关闭
- `test_tcp_server.cpp`：验证 echo server 主链路

#### 7）阅读这个文件时应该重点看什么
关注“测试在保护什么契约”，而不是只看断言数量。

#### 8）这个文件最容易让人误解的地方
测试数量不多，但覆盖的是最关键的结构性风险点。

#### 9）如果我要修改/扩展这个文件，应该注意什么
一旦更改线程顺序、生命周期约束或回调语义，相关 contract test 应同步更新。

### 文件：`src/`

#### 1）这个文件的核心职责
它不是当前核心职责实现，而是早期原型痕迹。

#### 2）为什么这个文件必须存在
从仓库演化角度看，它说明项目曾经历过更粗糙的最小版本。

#### 3）它在系统中的位置
历史/演进痕迹层。

#### 4）它依赖谁
非常少，且未形成当前可运行主线。

#### 5）谁依赖它
当前构建不依赖。

#### 6）文件中的关键内容
极简 `EventLoop/Channel/Poller` 草图。

#### 7）阅读这个文件时应该重点看什么
只把它当“演进参考”，不要当现状。

#### 8）这个文件最容易让人误解的地方
目录名叫 `src/`，但它并不是当前源码主线。

#### 9）如果我要修改/扩展这个文件，应该注意什么
更合理的方向是继续弱化它的存在感，或在未来明确标注 legacy。

## 8. 类级职责拆解（针对关键类）

### 类：`EventLoop`

#### 1）类的定位
单线程调度器，也是线程归属边界持有者。

#### 2）核心职责
驱动 poll、分发活跃事件、执行 pending functors。

#### 3）关键成员变量的意义
- `threadId_`：owner thread 身份
- `poller_`：I/O 事件来源
- `wakeupFd_` / `wakeupChannel_`：跨线程唤醒通道
- `pendingFunctors_`：跨线程或延迟执行任务队列
- `activeChannels_`：当前 poll 返回的活跃事件集合

#### 4）关键方法职责
- `loop()`：主循环
- `runInLoop()`：同线程立即执行，否则回流
- `queueInLoop()`：总是排队
- `wakeup()`：打断阻塞 poll
- `updateChannel/removeChannel()`：统一对接 `Poller`

#### 5）对象生命周期
通常由栈对象或 `EventLoopThread` 线程栈内对象创建，不可跨线程随意销毁。

#### 6）协作关系
它驱动 `Poller`，分发 `Channel`，被 `TcpServer` / `TcpConnection` / 线程池依赖。

#### 7）设计意图
用“owner-thread discipline”替代广泛加锁。

#### 8）扩展点 / 重构点
最佳扩展点是 timer、metrics、scheduler hook。  
风险最大的地方是事件顺序与销毁路径。

### 类：`Channel`

#### 1）类的定位
fd 事件语义对象，不是资源所有者。

#### 2）核心职责
保存兴趣事件和返回事件，并把它们翻译成回调。

#### 3）关键成员变量的意义
- `events_`：想关注什么
- `revents_`：内核实际返回了什么
- `index_`：在 `EPollPoller` 中的注册状态
- `tie_`：上层 owner 的弱引用守卫

#### 4）关键方法职责
- `enableReading/enableWriting/...`：维护兴趣集
- `handleEvent()`：带 tie 守卫的统一分发入口
- `remove()`：强制 remove-before-destroy

#### 5）对象生命周期
通常由 `Acceptor` 或 `TcpConnection` 内部持有，必须在所属 loop 线程解除注册后销毁。

#### 6）协作关系
被 `EventLoop` 注册到 `Poller`，被 `TcpConnection` 等上层对象配置回调。

#### 7）设计意图
把“事件语义”从“连接语义”和“后端语义”中分离出来。

#### 8）扩展点 / 重构点
可扩展 backend-neutral 事件定义。  
风险最大的是 tie、remove 与析构期关系。

### 类：`TcpConnection`

#### 1）类的定位
连接生命周期总管，也是协程与回调的交汇点。

#### 2）核心职责
统一管理读写、缓冲、状态机、关闭、错误和 awaiters。

#### 3）关键成员变量的意义
- `state_`：连接状态
- `socket_`：fd RAII 包装
- `channel_`：事件入口
- `inputBuffer_` / `outputBuffer_`：读写缓存
- `readWaiter_` / `writeWaiter_` / `closeWaiter_`：协程等待状态

#### 4）关键方法职责
- `connectEstablished()`：注册读事件并触发“已连接”
- `handleRead()/handleWrite()/handleClose()/handleError()`：主事件处理
- `connectDestroyed()`：最终解除注册
- `send()/shutdown()`：对外 API

#### 5）对象生命周期
由 `TcpServer` 的 `shared_ptr` 连接表持有，在关闭路径中经 base loop 与 io loop 协调后销毁。

#### 6）协作关系
与 `EventLoop`、`Channel`、`Buffer`、`Socket`、`TcpServer` 强协作。

#### 7）设计意图
把所有连接级可变状态集中在 owner loop，避免回调和协程把状态撕裂到多线程。

#### 8）扩展点 / 重构点
可扩展高水位线、背压、更多 awaiters。  
风险最大的部分是关闭路径、waiter 恢复、跨线程 send。

### 类：`TcpServer`

#### 1）类的定位
服务端连接总控器。

#### 2）核心职责
接收新连接、选择 io loop、维护连接表、安排销毁。

#### 3）关键成员变量的意义
- `acceptor_`：监听入口
- `threadPool_`：worker loops
- `connections_`：base loop 维护的连接表
- `lifetimeToken_`：防止晚到回调触碰已销毁 server

#### 4）关键方法职责
- `start()`：启动系统
- `newConnection()`：创建并分发连接
- `removeConnection()`：跨 loop 回流
- `removeConnectionInLoop()`：真正从连接表删除

#### 5）对象生命周期
通常由主程序栈对象持有，析构要求在 base loop 线程。

#### 6）协作关系
向下协作 `Acceptor`、线程池、`TcpConnection`；向上暴露 callback 接口。

#### 7）设计意图
把“监听管理”和“连接管理”集中到一个对外 API 入口，但仍保持底层薄层分工。

#### 8）扩展点 / 重构点
可扩展 naming、连接统计、hook、监控。  
风险最大的是 callback 生命周期与跨 loop 连接移除。

### 类：`EventLoopThreadPool`

#### 1）类的定位
loop 选择器，不是任务线程池。

#### 2）核心职责
启动 worker loops 并按轮转分配连接。

#### 3）关键成员变量的意义
- `baseLoop_`：零线程时的回退 loop
- `threads_`：worker 线程容器
- `loops_`：已发布的 worker loop 列表
- `next_`：轮转游标

#### 4）关键方法职责
- `start()`：创建 workers
- `getNextLoop()`：连接分配
- `getAllLoops()`：调试和测试辅助

#### 5）对象生命周期
由 `TcpServer` 持有，通常与服务端同寿命。

#### 6）协作关系
上接 `TcpServer`，下接 `EventLoopThread`。

#### 7）设计意图
以最简单的 one-loop-per-thread 模型支持扩展，不引入共享连接状态。

#### 8）扩展点 / 重构点
未来可加入负载均衡策略。  
当前风险较低，主要风险是误用为共享任务池。

## 9. 依赖关系与分层说明

### 9.1 当前分层

1. 设计约束层：`AGENTS.md`、`intents/`、`rules/`
2. 基础层：`mini/base/`
3. 平台/工具层：`InetAddress`、`Socket`、`SocketsOps`
4. 数据结构层：`Buffer`
5. 运行时核心层：`EventLoop`、`Channel`、`Poller`、`EPollPoller`
6. 网络会话层：`Acceptor`、`TcpConnection`、`TcpServer`
7. 扩展层：`EventLoopThread`、`EventLoopThreadPool`、`Task`
8. 示例与测试层：`examples/`、`tests/`

### 9.2 依赖方向

- 高层可以依赖低层
- 运行时核心层不应反向依赖 `TcpServer` 这种上层会话对象
- 平台层不应知道业务语义
- 测试和示例可以依赖所有运行时代码

### 9.3 当前值得注意的依赖特征

- `Channel` 直接依赖 epoll 常量，说明后端细节还没有完全被 `Poller` 层隔离
- `TcpConnection` 同时承载回调接口和协程等待逻辑，属于变化集中区
- `src/` 未参与构建，但名字容易造成理解分层误导

### 9.4 是否存在循环依赖

从代码结构上看，没有明显的编译层循环依赖，但存在运行时互相协作：

- `EventLoop` 驱动 `Channel`
- `Channel` 把 update/remove 反向委托回 `EventLoop`

这类双向协作是 Reactor 框架常见形态，只要所有权和调用边界清晰，就不是坏的循环。

### 9.5 稳定抽象与变化集中区

较稳定：

- `EventLoop`
- `Poller` 抽象
- `Buffer`
- `Socket/SocketsOps`

变化集中：

- `TcpConnection`
- `TcpServer`
- 协程 awaitable 逻辑

## 10. 扩展点地图

### 10.1 新增一个网络后端

从 `mini/net/Poller.h` 入手，实现新的具体 `Poller`，再在 `newDefaultPoller()` 切换或配置选择。

### 10.2 新增 TimerQueue

最佳切入点是 `EventLoop`。  
原因：timer 的调度语义本质上属于 owner loop，而不应独立于 EventLoop 线程模型存在。

### 10.3 新增协议层或业务 server

不要改 `EventLoop`。  
从 `TcpServer` 的回调接入开始，或者在其上再包一层更高阶 server，把协议解析放在消息回调或连接包装对象里。

### 10.4 新增协程能力

优先从 `TcpConnection` awaitable 模式扩展，如：

- 定长读取
- 行读取
- 等待可写
- 等待关闭超时

但恢复点依然应通过 `EventLoop`。

### 10.5 加日志、监控、埋点

适合插入的位置：

- `EventLoop::loop()`：每轮调度耗时、活跃事件数
- `EPollPoller::poll()`：poll 返回事件数、异常统计
- `TcpServer::newConnection/removeConnectionInLoop()`：连接数变化
- `TcpConnection::handleRead/handleWrite/handleClose/handleError()`：连接级 I/O 与故障指标

### 10.6 改造线程分配策略

从 `EventLoopThreadPool::getNextLoop()` 入手。  
当前是 round-robin，未来可替换为负载感知，但不能破坏“一个连接只归一个 loop”。

### 10.7 做热更新/插件化/脚本化

当前项目没有现成插件机制。  
如果要改造，最合适的位置不是 Reactor 核心，而是 `TcpServer` 之上的协议/业务层封装。

## 11. 调试与排错入口

### 11.1 启动失败先看哪里

- `examples/*/main.cpp`
- `TcpServer::start()`
- `Acceptor` 构造与 `listen()`
- `SocketsOps::bindOrDie/listenOrDie`

### 11.2 新连接进不来先看哪里

- `Acceptor::handleRead()`
- listen fd 是否已 `enableReading()`
- `EPollPoller::fillActiveChannels()`

### 11.3 消息没回调先看哪里

- `TcpConnection::handleRead()`
- `messageCallback_` 是否设置
- `Buffer::readFd()` 返回值
- `Channel` 的读回调是否正确注册

### 11.4 数据没写出去先看哪里

- `TcpConnection::sendInLoop()`
- `outputBuffer_` 是否积压
- `channel_->enableWriting()` 是否触发
- `handleWrite()` 是否被调用

### 11.5 关闭路径异常先看哪里

- `handleClose()` 是否被调用
- `handleError()` 是否正确收敛到关闭
- `TcpServer::removeConnection()` / `removeConnectionInLoop()`
- `connectDestroyed()` 是否最终 `channel_->remove()`

### 11.6 跨线程行为异常先看哪里

- `EventLoop::runInLoop/queueInLoop`
- `EventLoop::wakeup`
- `EventLoop::doPendingFunctors`
- 是否误在非 owner thread 直接调用 loop-owned API

### 11.7 崩溃先看哪里

当前代码有不少主动 `abort()` / throw 路径，崩溃时首先检查是否违反了以下契约：

- 非 owner thread 析构 `EventLoop` 或 `Acceptor`
- `Channel` 未 remove 就析构
- 一个线程里创建了多个 `EventLoop`
- 重复安装多个同类 waiter

### 11.8 内存/生命周期问题先看哪里

- `Channel::tie()`
- `TcpServer::lifetimeToken_`
- 关闭回调是否捕获裸 `this`
- `shared_ptr<TcpConnection>` 的流转路径

## 12. 推荐阅读顺序

### 第一轮：建立全局认知

1. `AGENTS.md`
2. `README.md`
3. `intents/architecture/system_overview.intent.md`
4. `intents/architecture/threading_model.intent.md`
5. `intents/architecture/lifetime_rules.intent.md`

### 第二轮：看最短可运行路径

1. `examples/echo_server/main.cpp`
2. `mini/net/TcpServer.h/.cc`
3. `mini/net/Acceptor.h/.cc`
4. `mini/net/TcpConnection.h/.cc`

### 第三轮：理解 Reactor 核心

1. `mini/net/EventLoop.h/.cc`
2. `mini/net/Channel.h/.cc`
3. `mini/net/Poller.h/.cc`
4. `mini/net/EPollPoller.h/.cc`

### 第四轮：理解支撑层

1. `mini/net/Buffer.h/.cc`
2. `mini/net/Socket.h/.cc`
3. `mini/net/SocketsOps.h/.cc`
4. `mini/net/InetAddress.h/.cc`

### 第五轮：理解扩展能力

1. `mini/net/EventLoopThread.h/.cc`
2. `mini/net/EventLoopThreadPool.h/.cc`
3. `mini/coroutine/Task.h`
4. `examples/coroutine_echo_server/main.cpp`

### 第六轮：回到测试验证认知

1. `tests/test_event_loop.cpp`
2. `tests/test_poller_contract.cpp`
3. `tests/test_tcp_connection.cpp`
4. `tests/test_tcp_server.cpp`
5. `tests/test_event_loop_thread_pool.cpp`

## 13. 框架设计优点 / 风险点

### 13.1 优点

- 分层总体清晰，尤其是设计意图层与运行时层分开
- `EventLoop`、`Channel`、`Poller` 的职责边界比较干净
- 线程模型明确，owner-loop 纪律强，便于推理
- 生命周期意识很强，`remove-before-destroy`、`tie`、`lifetimeToken_` 都在主动防坑
- 测试覆盖虽不多，但围绕关键契约布置，信号密度高
- 协程集成方式克制，没有绕开 EventLoop 重新发明调度器

### 13.2 风险点

- 文档和目录演进尚未完全同步：README 还提 `src/`，实际构建在 `mini/`
- `Channel` 对 epoll 常量的直接依赖意味着后端抽象仍有泄漏
- `TcpConnection` 当前职责较多，是未来最容易膨胀的类
- 当前错误处理多为 `abort()` / stderr 打印，适合学习但不够生产化
- `include/` 为空，说明对外 API 组织尚未稳定
- `TimerQueue` 等 intent 中的规划模块还未落地，未来扩展时需要谨慎保持现有调度顺序

## 14. 最终总结

### 14.1 这个框架最该抓住的 6 个核心点

1. 这是一个 Intent-first 的 Reactor 网络库，不是单纯代码集合。
2. 真正的运行时主线在 `mini/net`，不是 `src/`。
3. `EventLoop` 是线程归属边界，所有 loop-owned 状态都必须回到 owner thread。
4. `Channel` 是 fd 事件语义对象，`Poller` 是事件来源，`TcpConnection` 是连接生命周期总管。
5. 关闭路径和销毁路径是这个项目最重要的正确性来源之一。
6. 协程只是叠加在 EventLoop 之上的恢复机制，不是并行宇宙里的第二套调度器。

### 14.2 如果想快速上手，下一步该做什么

- 先本地跑 `echo_server`
- 再读 `test_tcp_server.cpp`
- 然后尝试给 `TcpConnection` 增加一个小能力，比如高水位回调或更明确的状态日志

### 14.3 如果想快速改几个文件练手

- `mini/net/TcpServer.cc`
- `mini/net/TcpConnection.cc`
- `mini/net/EventLoop.cc`
- `tests/test_tcp_server.cpp`

### 14.4 如果想真正掌握这个项目

下一步最值得深挖的是：

- `EventLoop` 的事件顺序与 wakeup 语义
- `TcpConnection` 的关闭/销毁收敛路径
- 协程 waiter 如何与连接生命周期安全共存
- `TimerQueue` 将来应该如何接入而不破坏当前线程纪律
