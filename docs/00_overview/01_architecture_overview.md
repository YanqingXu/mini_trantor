# 架构总览

## 整体架构图

```
┌─────────────────────────────────────────────────────────────────┐
│                        Application Layer                         │
│  (echo_server, coroutine_echo_server, HttpServer, WsServer,     │
│   RpcServer/RpcClient-based apps)                               │
├─────────────────────────────────────────────────────────────────┤
│                       Protocol Layer                             │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐         │
│  │ HTTP/1.1     │  │ WebSocket    │  │ RPC          │         │
│  │ HttpServer   │  │ WsServer     │  │ RpcServer    │         │
│  │ HttpContext  │  │ WsCodec      │  │ RpcClient    │         │
│  │ HttpRequest  │  │ WsHandshake  │  │ RpcChannel   │         │
│  │ HttpResponse │  │ WsConnection │  │ RpcCodec     │         │
│  └──────────────┘  └──────────────┘  └──────────────┘         │
├─────────────────────────────────────────────────────────────────┤
│                     Coroutine Bridge Layer                        │
│  ┌────────────┐  ┌───────────────────┐  ┌────────────────┐     │
│  │ Task<T>    │  │ CancellationToken │  │ SleepAwaitable │     │
│  └────────────┘  └───────────────────┘  └────────────────┘     │
│  ┌────────────┐  ┌───────────┐  ┌──────────────┐              │
│  │ WhenAll    │  │ WhenAny   │  │ Timeout      │              │
│  └────────────┘  └───────────┘  └──────────────┘              │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │ TcpConnection::ReadAwaitable / WriteAwaitable / Close... │  │
│  └──────────────────────────────────────────────────────────┘  │
├─────────────────────────────────────────────────────────────────┤
│                         Net Layer                                │
│  ┌──────────┐  ┌───────────────┐  ┌──────────┐  ┌──────────┐ │
│  │TcpServer │  │TcpConnection  │  │TcpClient │  │Connector │ │
│  │          │◄─┤(shared_ptr)   │  │          │─►│          │ │
│  │Acceptor  │  │Buffer(in/out) │  │          │  │          │ │
│  └──────────┘  │Channel        │  └──────────┘  └──────────┘ │
│                │Socket         │                               │
│                └───────────────┘                               │
├─────────────────────────────────────────────────────────────────┤
│                    Reactor Core Layer                             │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │                      EventLoop                            │  │
│  │  ┌────────┐  ┌──────────┐  ┌───────────┐  ┌──────────┐ │  │
│  │  │Poller  │  │TimerQueue│  │wakeupFd   │  │pending   │ │  │
│  │  │(epoll) │  │(timerfd) │  │(eventfd)  │  │Functors  │ │  │
│  │  └────────┘  └──────────┘  └───────────┘  └──────────┘ │  │
│  └──────────────────────────────────────────────────────────┘  │
├─────────────────────────────────────────────────────────────────┤
│                     Thread Layer                                 │
│  ┌──────────────────┐  ┌──────────────────────────────────┐   │
│  │ EventLoopThread  │  │ EventLoopThreadPool              │   │
│  │ (one loop/thread)│  │ (round-robin load balance)       │   │
│  └──────────────────┘  └──────────────────────────────────┘   │
├─────────────────────────────────────────────────────────────────┤
│                     Advanced Layer                               │
│  ┌──────────────┐  ┌──────────────┐                            │
│  │ TlsContext   │  │ DnsResolver  │                            │
│  │ (SSL_CTX)    │  │ (thread pool │                            │
│  │              │  │  + cache)    │                            │
│  └──────────────┘  └──────────────┘                            │
├─────────────────────────────────────────────────────────────────┤
│                       Base Layer                                 │
│  ┌────────────┐  ┌──────────────┐  ┌──────────┐               │
│  │ Timestamp  │  │ noncopyable  │  │ InetAddr │               │
│  └────────────┘  └──────────────┘  └──────────┘               │
└─────────────────────────────────────────────────────────────────┘
              │                     │
              ▼                     ▼
        Linux Kernel           OpenSSL
        (epoll/timerfd/         (SSL_CTX/
         eventfd/socket)         SSL)
```

## 分层说明

### 1. Base Layer（基础工具层）

提供最底层的通用抽象：时间戳、不可复制基类、地址封装。这一层无依赖，被所有上层引用。

### 2. Reactor Core Layer（Reactor 核心层）

**这是整个框架的心脏。** EventLoop 是单线程事件循环，它驱动一切。

- **EventLoop**：poll + 事件分发 + 跨线程任务回流。所有可变状态只在 owner 线程访问。
- **Poller / EPollPoller**：I/O 多路复用后端，将内核 epoll 事件映射到 Channel。
- **Channel**：fd 的事件订阅与回调分发实体。不拥有 fd，不拥有 EventLoop。
- **TimerQueue**：基于 timerfd 的定时任务系统，支持 one-shot 和 repeating timer。

### 3. Thread Layer（线程模型层）

实现 one-loop-per-thread 扩展模型：

- **EventLoopThread**：在独立线程中运行一个 EventLoop。
- **EventLoopThreadPool**：管理多个 worker 线程，round-robin 分配新连接。

### 4. Net Layer（网络层）

TCP 连接的完整生命周期管理：

- **TcpServer**：服务端 —— 协调 Acceptor、线程池和连接映射。
- **TcpConnection**：单个 TCP 连接的状态机 —— Channel + Buffer + 回调。
- **TcpClient**：客户端 —— 通过 Connector 发起连接，支持自动重连。
- **Acceptor**：监听 socket 适配器，accept 后交付 fd。
- **Connector**：主动连接适配器，处理非阻塞 connect + 指数退避重连。

### 5. Coroutine Bridge Layer（协程桥接层）

将 C++20 协程与 Reactor 调度语义融合：

- **Task\<T\>**：最小可组合的 coroutine 结果对象，支持 `co_await` / `start()` / `detach()`。
- **CancellationToken**：协作式取消原语，贯穿 `SleepAwaitable`、`TcpConnection` awaitable、DNS resolve 与 `WhenAny` loser cancel。
- **SleepAwaitable**：基于 TimerQueue 的协程定时等待，返回显式 `Expected<void, NetError>`。
- **WhenAll / WhenAny**：结构化并发原语 —— 等待全部完成 / 等待首个完成；`WhenAny` 会向败者发出协作式取消请求。
- **Timeout**：将常见的 `operation vs timer` 竞态收敛为统一 `NetError::TimedOut`。
- **ReadAwaitable / WriteAwaitable / CloseAwaitable**：TcpConnection 内置的协程 I/O 挂起点。

### 6. Protocol Layer（协议层）

建立在 Net 层之上的协议解析与协议服务器：

- **HTTP/1.1**：增量解析状态机 + TcpServer 协议适配器。
- **WebSocket**：RFC 6455 帧编解码 + HTTP Upgrade 握手 + 自动 ping/pong。
- **RPC**：长度前缀帧编解码 + 请求关联 + callback/coroutine 双模式调用。

### 7. Advanced Layer（高级组件层）

独立的功能增强组件：

- **TlsContext**：RAII 封装 SSL_CTX，配置后只读，安全跨线程共享。
- **DnsResolver**：异步工作线程池 + TTL 缓存，不阻塞 EventLoop。

## 核心设计原则

### 1. One-Loop-Per-Thread

每个线程最多拥有一个 EventLoop，所有属于该 EventLoop 的对象（Channel、TcpConnection、TimerQueue）都只在该线程上操作。这消除了几乎所有的数据竞争。

### 2. 跨线程投递

当需要从其他线程操作某个 EventLoop 的对象时，通过 `runInLoop()` / `queueInLoop()` 将任务投递到目标线程：

```
Thread A ──► queueInLoop(functor) ──► EventLoop B (wakeup via eventfd) ──► 执行 functor
```

### 3. Ownership 清晰

- `TcpServer` 拥有所有 `TcpConnection`（通过 `shared_ptr`）
- `TcpConnection` 拥有 `Channel`、`Socket`、`Buffer`
- `EventLoop` 拥有 `Poller`、`TimerQueue`、`wakeupChannel`
- 协议层对象通过 `std::any context_` 附着在 `TcpConnection` 上

### 4. 协程不绕过 Reactor

所有 awaitable 的恢复都通过 `EventLoop::queueInLoop()` 投递，确保协程在正确的 loop 线程上恢复执行。协程是调度友好的，不是调度替代品。
