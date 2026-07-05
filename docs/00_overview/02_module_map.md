# 模块地图

## 模块总览

```
mini/
├── base/           ← 基础工具（Timestamp, noncopyable）
├── net/            ← Reactor 核心 + 网络层 + 高级组件
│   ├── udp/        ← UDP datagram 基线与 PMTU signal preview
│   ├── kcp/        ← KCP-style reliable UDP preview
│   ├── broadcast/  ← 游戏广播路由与批量发送
│   ├── transport/  ← transport 抽象与 PathMtuCache preview
│   └── framing/    ← 游戏/协议通用 packet framing
├── game/           ← 游戏网络接入、session、logic handoff
├── codec/          ← Protobuf/FlatBuffers 等 codec adapter
├── coroutine/      ← 协程桥接层
├── http/           ← HTTP/1.1 协议层
├── ws/             ← WebSocket 协议层
└── rpc/            ← RPC 协议层
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
| `CancellationToken` | `mini/coroutine/CancellationToken.h` | 协作式取消原语 | 核心 |
| `SleepAwaitable` | `mini/coroutine/SleepAwaitable.h` | 定时器等待桥接 | 桥接 |
| `Timeout` | `mini/coroutine/Timeout.h` | 统一 timeout 语义包装 | 桥接 |
| `WhenAll` | `mini/coroutine/WhenAll.h` | 并发等待全部完成 | 组合 |
| `WhenAny` | `mini/coroutine/WhenAny.h` | 竞争等待首个完成 | 组合 |
| `ResolveAwaitable` | `mini/coroutine/ResolveAwaitable.h` | DNS 解析协程桥接 | 桥接 |

**依赖关系**：`SleepAwaitable` → `EventLoop::runAfter`；`Timeout` → `WhenAny` + `SleepAwaitable`；`WhenAll`/`WhenAny` → `Task<T>`

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

### 8. RPC —— 远程调用协议层

| 类 | 文件 | 职责 | 地位 |
|----|------|------|------|
| `RpcCodec` | `mini/rpc/RpcCodec.h/cc` | 长度前缀二进制帧编解码 | 核心 |
| `RpcChannel` | `mini/rpc/RpcChannel.h/cc` | per-connection 请求/响应关联 + timeout 管理 | 核心 |
| `RpcServer` | `mini/rpc/RpcServer.h/cc` | TcpServer 协议适配器 + method 分发 | 核心 |
| `RpcClient` | `mini/rpc/RpcClient.h/cc` | TcpClient 包装 + callback/coroutine 双模式调用 | 核心 |

### 9. Game Foundation —— 游戏网络底座接入层

| 类 | 文件 | 职责 | 地位 |
|----|------|------|------|
| `PlayerSession` | `mini/game/PlayerSession.h/cc` | 玩家网络 session 状态机 | 游戏网络底座 |
| `SessionManager` | `mini/game/SessionManager.h/cc` | 认证、在线、断线、重连窗口管理 | 游戏网络底座 |
| `GameServerPipeline` | `mini/game/GameServerPipeline.h/cc` | 网络输入到 session/logic 的接入管线 | 游戏网络底座 |
| `GameCommandQueue` | `mini/game/logic/GameCommandQueue.h/cc` | 网络线程到逻辑线程的命令队列 | 游戏网络底座 |
| `LogicLoop` | `mini/game/logic/LogicLoop.h/cc` | fixed-step 逻辑循环桥接 | 游戏网络底座 |
| `GameBackpressurePolicy` | `mini/game/GameBackpressurePolicy.h` | 输入、输出、广播的基础背压策略 | 游戏网络底座 |
| `GameGatewaySecurityPolicy` | `mini/game/GameGatewaySecurityPolicy.h` | 最小网关 admission/security skeleton | 游戏网络底座 |

这一层只负责网络入口、session 生命周期和逻辑 handoff，不拥有账号系统、房间状态、AOI、风控审计或部署拓扑。

### 10. Transport Preview —— KCP/PMTU 预览层

| 类 | 文件 | 职责 | 地位 |
|----|------|------|------|
| `UdpSocket` / `UdpServer` | `mini/net/udp/*` | UDP datagram 基线、read budget、PMTU callback 入口 | 传输底座 |
| `KcpTransport` / `KcpSession` | `mini/net/kcp/*` | KCP-style reliable UDP preview | 预览 |
| `PathMtuSignalAdapter` | `mini/net/udp/PathMtuSignalAdapter.*` | 平台 PMTU signal adapter preview | 预览 |
| `IcmpPathMtuListener` | `mini/net/udp/IcmpPathMtuListener.*` | raw ICMP/ICMPv6 PMTU listener preview | 预览 |
| `PathMtuCache` | `mini/net/transport/PathMtuCache.*` | 进程内共享 PMTU hint cache preview | 预览 |

这一层必须按 `docs/game_server_network_base_scope_boundary.md` 处理：高级 KCP、PMTU、冗余、XOR parity 和 congestion-window 能力是 transport preview，不代表生产级协议栈。

### 11. Codec / Metrics —— 协议桥与轻量观测

| 类 | 文件 | 职责 | 地位 |
|----|------|------|------|
| `PacketFramer` | `mini/net/framing/PacketFramer.h/cc` | 通用 length-prefix/game frame 解析 | 传输底座 |
| `CodecAdapter` | `mini/codec/CodecAdapter.h` | codec 统一错误返回与 encode/decode 接口 | 协议桥 |
| `ProtobufAdapter` | `mini/codec/ProtobufAdapter.*` | Protobuf 风格适配 | 协议桥 |
| `FlatBuffersAdapter` | `mini/codec/FlatBuffersAdapter.*` | FlatBuffers 风格适配 | 协议桥 |
| `MetricsHook` / `MetricsExporter` | `mini/base/*Metrics*` | 轻量 metrics hook、内存聚合和文本 snapshot | 支撑 |

Metrics exporter 只提供无依赖 snapshot 和标签化能力，不负责 scrape server、push gateway、告警或观测平台。

### 12. Advanced —— 高级组件

| 类 | 文件 | 职责 | 地位 |
|----|------|------|------|
| `TlsContext` | `mini/net/TlsContext.h/cc` | RAII SSL_CTX 封装 | 增强 |
| `DnsResolver` | `mini/net/DnsResolver.h/cc` | 异步 DNS 解析 + TTL 缓存 | 增强 |

## 模块依赖全景图

```
              ┌──────────┐
              │ Protocol │  (HTTP, WebSocket, RPC)
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
  → RpcCodec → RpcChannel → RpcServer / RpcClient
  → PacketFramer → TransportManager
  → PlayerSession → SessionManager
  → GameCommandQueue → LogicLoop → GameServerPipeline
  → docs/game_server_network_base_scope_boundary.md
```
