# 模块地图

## 模块总览

```
mini/
├── base/           ← 基础工具（Timestamp, noncopyable）
├── net/            ← Reactor 核心 + 网络层 + 高级组件
├── coroutine/      ← 协程桥接层
├── http/           ← HTTP/1.1 协议层
└── ws/             ← WebSocket 协议层
```

## 模块详情

### 1. Reactor Core —— 事件循环引擎

| 类 | 文件 | 职责 | 地位 |
|----|------|------|------|
| `EventLoop` | `mini/net/EventLoop.h/cc` | poll + 事件分发 + 跨线程任务投递 | **心脏** |
| `Channel` | `mini/net/Channel.h/cc` | fd 事件订阅与回调分发 | 核心 |
| `Poller` | `mini/net/Poller.h` | I/O 多路复用抽象基类 | 核心 |
| `EPollPoller` | `mini/net/EPollPoller.h/cc` | Poller 的 epoll 后端实现 | 核心 |
| `TimerQueue` | `mini/net/TimerQueue.h/cc` | timerfd 驱动的定时任务 | 核心 |

**依赖关系**：`EventLoop` → `Poller` + `TimerQueue` + `Channel`(wakeup)

**为什么需要这个模块**：Reactor 模式的本质 —— 单线程内用"等待事件-分发处理"循环驱动所有 I/O 操作，无锁、高效。

### 2. Thread Model —— 线程扩展

| 类 | 文件 | 职责 | 地位 |
|----|------|------|------|
| `EventLoopThread` | `mini/net/EventLoopThread.h/cc` | 在独立线程中运行一个 EventLoop | 支撑 |
| `EventLoopThreadPool` | `mini/net/EventLoopThreadPool.h/cc` | 管理 N 个 worker 线程 + round-robin | 支撑 |

**依赖关系**：`EventLoopThreadPool` → `EventLoopThread` → `EventLoop`

**为什么需要这个模块**：单线程 Reactor 有吞吐瓶颈。多线程模型让 base loop 负责 accept，worker loops 负责 I/O。

### 3. Net —— TCP 连接管理

| 类 | 文件 | 职责 | 地位 |
|----|------|------|------|
| `TcpServer` | `mini/net/TcpServer.h/cc` | 服务端：Acceptor + 线程池 + 连接映射 | 核心 |
| `TcpConnection` | `mini/net/TcpConnection.h/cc` | 单个连接的状态机 + 缓冲区 + 回调 | **核心中的核心** |
| `Acceptor` | `mini/net/Acceptor.h/cc` | 监听 socket 的 accept 路径适配 | 支撑 |
| `TcpClient` | `mini/net/TcpClient.h/cc` | 客户端：Connector + 连接管理 + 重连 | 核心 |
| `Connector` | `mini/net/Connector.h/cc` | 主动连接适配器 + 指数退避重连 | 支撑 |
| `Buffer` | `mini/net/Buffer.h/cc` | 连接读写路径上的字节容器 | 核心 |

**依赖关系**：`TcpServer` → `Acceptor` + `EventLoopThreadPool` + `TcpConnection`；`TcpConnection` → `Channel` + `Socket` + `Buffer`

**为什么需要这个模块**：将底层 fd 操作抽象为高层的"连接"概念，管理连接的完整生命周期（建立→读写→关闭→销毁）。

### 4. Utils —— 工具类

| 类 | 文件 | 职责 | 地位 |
|----|------|------|------|
| `InetAddress` | `mini/net/InetAddress.h/cc` | IPv4 地址 + 端口封装 | 工具 |
| `Socket` | `mini/net/Socket.h/cc` | fd 级 socket 操作的 RAII 封装 | 工具 |
| `SocketsOps` | `mini/net/SocketsOps.h/cc` | 底层系统调用的薄包装 | 工具 |
| `Callbacks` | `mini/net/Callbacks.h` | 统一的回调类型定义 | 工具 |
| `TimerId` | `mini/net/TimerId.h` | Timer 标识符值对象 | 工具 |

### 5. Coroutine —— 协程桥接

| 类 | 文件 | 职责 | 地位 |
|----|------|------|------|
| `Task<T>` | `mini/coroutine/Task.h` | 协程结果对象 | 核心 |
| `SleepAwaitable` | `mini/coroutine/SleepAwaitable.h` | 定时器等待桥接 | 桥接 |
| `WhenAll` | `mini/coroutine/WhenAll.h` | 并发等待全部完成 | 组合 |
| `WhenAny` | `mini/coroutine/WhenAny.h` | 竞争等待首个完成 | 组合 |
| `ResolveAwaitable` | `mini/coroutine/ResolveAwaitable.h` | DNS 解析协程桥接 | 桥接 |

**依赖关系**：`SleepAwaitable` → `EventLoop::runAfter`；`WhenAll`/`WhenAny` → `Task<T>`

**为什么需要这个模块**：让用户用 `co_await` 线性风格编写异步代码，同时保持 Reactor 语义。

### 6. HTTP —— HTTP/1.1 协议层

| 类 | 文件 | 职责 | 地位 |
|----|------|------|------|
| `HttpServer` | `mini/http/HttpServer.h/cc` | TcpServer 的 HTTP 协议适配器 | 核心 |
| `HttpContext` | `mini/http/HttpContext.h/cc` | per-connection 增量解析状态机 | 核心 |
| `HttpRequest` | `mini/http/HttpRequest.h/cc` | HTTP 请求值对象 | 数据结构 |
| `HttpResponse` | `mini/http/HttpResponse.h/cc` | HTTP 响应构建器 + 序列化 | 数据结构 |

### 7. WebSocket —— WebSocket 协议层

| 类 | 文件 | 职责 | 地位 |
|----|------|------|------|
| `WebSocketServer` | `mini/ws/WebSocketServer.h/cc` | TcpServer 包装 + HTTP→WS 升级 | 核心 |
| `WebSocketCodec` | `mini/ws/WebSocketCodec.h/cc` | RFC 6455 帧编解码 | 核心 |
| `WebSocketHandshake` | `mini/ws/WebSocketHandshake.h/cc` | Upgrade 验证 + Accept 计算 | 支撑 |
| `WebSocketConnection` | `mini/ws/WebSocketConnection.h/cc` | per-connection 状态机 + ping/pong | 核心 |

### 8. Advanced —— 高级组件

| 类 | 文件 | 职责 | 地位 |
|----|------|------|------|
| `TlsContext` | `mini/net/TlsContext.h/cc` | RAII SSL_CTX 封装 | 增强 |
| `DnsResolver` | `mini/net/DnsResolver.h/cc` | 异步 DNS 解析 + TTL 缓存 | 增强 |

## 模块依赖全景图

```
              ┌──────────┐
              │ Protocol │  (HTTP, WebSocket)
              │  Layer   │
              └────┬─────┘
                   │ uses
              ┌────▼─────┐     ┌───────────┐
              │   Net    │◄────│ Coroutine │
              │  Layer   │     │  Bridge   │
              └────┬─────┘     └─────┬─────┘
                   │                 │
              ┌────▼─────┐          │ uses
              │ Reactor  │◄─────────┘
              │  Core    │
              └────┬─────┘
                   │
    ┌──────────────┼──────────────┐
    │              │              │
┌───▼────┐  ┌─────▼─────┐  ┌────▼─────┐
│ Thread │  │  Advanced │  │  Base    │
│ Model  │  │  (TLS/DNS)│  │  Utils  │
└────────┘  └───────────┘  └──────────┘
```

## 推荐阅读顺序

**初学者路径**：

```
Timestamp → noncopyable → InetAddress → Socket
  → Channel → Poller → EPollPoller → EventLoop
  → Buffer → TcpConnection → Acceptor → TcpServer
  → EventLoopThread → EventLoopThreadPool
  → Connector → TcpClient
  → Task<T> → SleepAwaitable
  → TimerQueue → WhenAll/WhenAny
  → TlsContext → DnsResolver
  → HttpContext → HttpServer
  → WebSocketCodec → WebSocketServer
```
