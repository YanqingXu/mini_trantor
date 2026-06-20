# Task-01 统一传输抽象 — 执行清单（Task List）

## 目标对齐
- 任务：引入 `ITransportSession` / `ITransportChannel` 抽象，并以 `TransportEndpoint` 将 `TcpConnection` 的 fd 状态机与上层协议会话解耦。
- 约束来源：`AGENTS.md` + `docs/roadmap_game_server_network_base_execution_plan.md` 0.2（Change Gate 必答项）。
- 影响范围：`transport` 抽象层 + 协议连接接口层。

## 文件级改动清单

### 1) 新增文件
- `mini/net/transport/TransportTypes.h`
  - 新增 `TransportSessionId`、无效/起始 ID 常量和 `TransportKind`。
  - 该文件不持有对象引用，不处理生命周期。

- `mini/net/transport/ITransport.h`
  - 新增 `ITransportChannel`（send/shutdown/forceClose/connected/getLoop/name）。
  - 新增 `ITransportSession`（sessionId / kind / transportContext）。
  - 增加统一回调别名 `TransportReadCallback` / `TransportCloseCallback`。

- `mini/net/transport/TransportEndpoint.h`
  - 新增 `TransportEndpoint`：`ITransportChannel + ITransportSession` 的 TCP 绑定实现。
  - 内部通过 `std::weak_ptr<TcpConnection>` 持有底层通道，避免 ownership 反转。

- `mini/net/transport/TransportManager.h`
  - 新增管理器：按 `sessionId -> TransportEndpoint` 保存会话映射。
  - 提供 `register / send / close / deregister`，内建 `ownerLoop` marshaling。
  - 支持跨线程 API 安全入口（队列回流）。

### 2) 修改文件
- `mini/net/ProtocolConnection.h`
  - `IProtocolConnection` 同步继承 `ITransportChannel + ITransportSession`。
  - 保留 `ProtocolContext` 插槽接口，满足 HTTP/RPC/WebSocket 现有语义。

- `mini/net/ProtocolConnectionAdapter.h/.cc`
  - 适配器改为持有 `weak_ptr<ITransportChannel>` 与 `weak_ptr<ITransportSession>`。
  - `createAndBind` 使用 `TransportEndpoint::create(conn)` 进行上下文绑定。
  - `getProtocolContext` 与 `setProtocolContext` 映射到 transport-context 承载槽（当前实现为同槽映射，保持语义兼容）。

- `mini/net/Callbacks.h`
  - 新增 transport 抽象回调类型 `TransportReadCallback` / `TransportCloseCallback`，减少对 `TcpConnection` 的硬耦合。

- `tests/unit/transport/test_transport_abstraction.cpp`
  - 新增 Task-01 核心契约最小覆盖：端点上下文/会话、Adapter 绑定、manager 生命周期。

- `tests/CMakeLists.txt`
  - 注册 `unit/transport/test_transport_abstraction.cpp`。

## Core Module Change Gate（Task-01 5 问答）

### Loop / Thread 归属
- `TransportEndpoint` 绑定的 loop 由其底层 `TcpConnection::getLoop()` 决定。
- `TransportManager` 属于调用侧 owner loop（建议由 `TcpServer` 的 base loop 或上层网络管理 loop 构造与析构）。
- 所有 `register/send/close/deregister` 操作不直接在调用线程直接触发映射变更：统一通过 `ownerLoop_->isInLoopThread()` / `queueInLoop` 进行回流。

### 所有权（Ownership）
- 生命周期主体：`TransportManager` 内部以 `std::shared_ptr<TransportEndpoint>` 持有连接会话映射。
- 连接生命周期：`TcpConnection` 通过 `TcpConnection::setContext(shared_ptr<ProtocolConnectionAdapter>)` 持有适配器。
- 适配器不持有强引用到 endpoint（`weak_ptr<ITransport*>`）以避免循环引用。
- `TransportEndpoint` 持有 `weak_ptr<TcpConnection>`，避免从 transport 层反向托举 TcpConnection。

### 回调重入点（Re-entry）
- 风险点：`onRead/onClose` 在 ioLoop 内可能在一次 poll/dispatch 中高频触发。
- `TransportEndpoint` 不直接发起上层回调，仅将 I/O 结果透传；
  重入检查应在业务侧 protocol handler 的回调链中完成（例如连接状态变化后重复触发的幂等处理）。
- `ProtocolConnectionAdapter` 的 `send/shutdown/forceClose` 在弱引用失效时直接 no-op，避免悬空回调造成 UAF。

### 跨线程 Marshal 方式
- `TransportManager::registerEndpoint/registerConnection/send/close/deregister` 统一走内部 `post()`。
- `post()` 逻辑：
  - 若当前线程即 owner loop，直接执行。
  - 否则 `ownerLoop_->queueInLoop(std::move(fn))`。
- 这样确保 `TransportManager` 的 `endpoints_` map 与发送/关闭动作只在 owner 线程上并发安全修改。

### 测试映射（Test Mapping）
- 已有新增：`tests/unit/transport/test_transport_abstraction.cpp`
- 计划新增（待 P0 后续补齐）
  - `tests/contract/transport/test_transport_contract.cpp`：验证跨线程入队约束、生命周期边界与 id 分配契约。
  - `tests/integration/transport/test_transport_adapter_loopback.cpp`：循环连接场景下 adapter 与 manager 的协同。

## 最小实现顺序（建议）
1. `TransportTypes.h`：先定义 session id 与 transport kind。
2. `ITransport.h`：先立统一抽象接口。
3. `TransportEndpoint.h`：先把 `TcpConnection` 封装为统一端点。
4. `TransportManager.h`：再补充会话 map 与跨线程 marshal。
5. `ProtocolConnection.h`：把协议接口绑定到新抽象。
6. `ProtocolConnectionAdapter.h/.cc`：改造适配器持久化/解绑逻辑。
7. `Callbacks.h`：增加传输回调统一类型。
8. `tests/unit/transport/test_transport_abstraction.cpp` + `tests/CMakeLists.txt`：补充最小契约。

## 执行建议
- 本任务目标范围先聚焦“解耦与生命周期正确性”，不在此阶段扩展 UDP/KCP。
- 下一阶段（Task-02/03）可复用本 `ITransport*` 与 `TransportManager` 的 session 表达，快速挂接 UDP/KCP endpoint 子类。
