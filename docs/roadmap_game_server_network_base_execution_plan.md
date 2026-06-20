# Game Server Networking Base Execution Plan

> Context: Current baseline is `mini-trantor` v6-alpha（客户端生态阶段，目标是把网络库演进为通用游戏服务器底座）。

## 0. 执行目标与边界

### 0.1 目标
把现有 TCP/TLS/Reactor 基础上扩展到一个可用于游戏服务器的网络底座，兼顾：
- 传输层多样性（TCP/TLS/UDP/KCP可组合）
- 大规模跨线程广播（低拷贝、低线程切换）
- 标准化序列化（Protobuf/FlatBuffers）
- 会话重建与断线重连
- 逻辑层 fixed-step 运行模型协作
- 可观测性增强（面向低延迟高并发）

### 0.2 约束（严格遵循 AGENTS）
- 先读 intent/rules（已完成）
- 所有核心模块 PR 均需回答：
  1. Loop / Thread 归属
  2. 所有权
  3. 回调重入点
  4. 跨线程操作的 marshal 方式
  5. 测试文件映射
- 所有权与生命周期优先于“快速实现”

### 0.3 参与文件
- [`docs/roadmap_game_server_network_base_execution_plan.md`](/home/xyq/mini-trantor/docs/roadmap_game_server_network_base_execution_plan.md)（本文件）
- [`mini/net/*`](/home/xyq/mini-trantor/mini/net/)
- [`mini/http/*`](/home/xyq/mini-trantor/mini/http/)
- [`mini/rpc/*`](/home/xyq/mini-trantor/mini/rpc/)
- [`tests/unit/**/*`](/home/xyq/mini-trantor/tests/unit/)
- [`tests/contract/**/*`](/home/xyq/mini-trantor/tests/contract/)
- [`tests/integration/**/*`](/home/xyq/mini-trantor/tests/integration/)

---

## 1. 任务总览（12 项）

优先级：
- **P0**：必须先做（不做无法安全承载游戏主站流）
- **P1**：增强性，P0 之后即可落地
- **P2**：扩展性与高阶优化，P1 后可迭代

1. **Task-01（P0）** 统一传输抽象：引入 `ITransportSession` / `ITransportChannel`，解耦 `TcpConnection` 与上层协议/会话
2. **Task-02（P0）** 先行接入 UDP 基线：新增 `UdpServer`/`UdpSocket` 与事件入口（不引入 KCP 时）
3. **Task-03（P2）** KCP 集成：按独立传输实例挂到 loop（非替代 EventLoop）
4. **Task-04（P0）** base loop 侧广播路由器：减少全局 map 遍历与不必要的跨 loop queue
5. **Task-05（P0）** ioLoop 分桶批量发送：`BroadcastDispatcher` 一次性入队并批量出队发送
6. **Task-06（P0）** 零拷贝与内存池化：`SharedPayload + Arena` 贯通广播链路
7. **Task-07（P0）** 通用 Packet Framing 模块：统一粘包/半包，支持可变长度消息协议
8. **Task-08（P1）** 标准化序列化桥接：Protobuf/FlatBuffers 适配器（与 Task-07 对齐）
9. **Task-09（P0）** 引入 `PlayerSession` + `SessionManager`
10. **Task-10（P1）** 断线重连（Reconnection/Sticky）与短时状态保留
11. **Task-11（P0）** fixed-step 逻辑线程化：`LogicLoop` + 逻辑命令队列，避免阻塞 I/O loop
12. **Task-12（P1）** 指标升级：扩充 `MetricsHook` 与 `TcpServerOptions` 以覆盖广播、队列、抖动与延迟指标

---

## 2. 任务明细（文件级改动）

下面每项均遵循：
- 线程归属：该模块/对象在哪个 loop 线程创建、驱动、销毁
- 所有权：谁创建、谁持有、谁释放、何时注销
- 重入点：哪些 callback 可能在执行中再次触发业务回调或重排队列
- 跨线程：允许哪些操作跨线程，以及通过 `EventLoop::runInLoop/queueInLoop` 的 marshal 方式
- 测试映射：至少包含一条 contract + 一条 integration（有条件可先 unit）

### Task-01（P0）统一传输抽象

1) 文件级变更
- 新增  
  - [`mini/net/transport/TransportTypes.h`](/home/xyq/mini-trantor/mini/net/transport/TransportTypes.h)  
  - [`mini/net/transport/ITransport.h`](/home/xyq/mini-trantor/mini/net/transport/ITransport.h)  
  - [`mini/net/transport/TransportEndpoint.h`](/home/xyq/mini-trantor/mini/net/transport/TransportEndpoint.h)  
  - [`mini/net/transport/TransportManager.h`](/home/xyq/mini-trantor/mini/net/transport/TransportManager.h)  
- 修改  
  - [`mini/net/ProtocolConnection.h`](/home/xyq/mini-trantor/mini/net/ProtocolConnection.h)  
  - [`mini/net/ProtocolConnectionAdapter.h`](/home/xyq/mini-trantor/mini/net/ProtocolConnectionAdapter.h)  
  - [`mini/net/Callbacks.h`](/home/xyq/mini-trantor/mini/net/Callbacks.h)  

2) 目标行为
- `TcpConnection`、未来 UDP/KCP/自定义传输都按统一 `transport endpoint` 接口暴露 send/close/context 接口。
- 下层 `TransportEndpoint` 负责本地 fd/状态机，上层 Session/协议不再感知底层类型分支。

3) 线程归属
- Endpoint 对象：其 owner loop = 创建它的 `EventLoop`
- `TransportManager`：逻辑线程安全控制层，由调用方线程持有，但所有 `register/deregister` 操作都通过 owner loop 回流

4) 所有权
- `EventLoop` 拥有 Poller/Channel/fd 资源
- `TransportManager`（base loop）拥有 endpoint 生命周期映射（`shared_ptr`）
- 单连接 session 层只持有 `weak_ptr` 到 transport，防止循环引用

5) 回调重入点
- `onRead/onWrite/onError/onClose` 回调可能在 transport io-loop 处理链路中被连续触发；回调内部不得直接同步触发跨 loop 重调度，需入队。
- `ProtocolConnection` 侧若直接回调 user handler，允许由 transport 驱动多次同一 tick 回调，需保证幂等。

6) 跨线程规则
- 仅允许从任意线程调用 `TransportManager::close/send` 等入口，内部统一 `ownerLoop->queueInLoop`。

7) 测试映射（新增）
- Unit：`tests/unit/transport/test_transport_abstraction.cpp`
- Contract：`tests/contract/transport/test_transport_contract.cpp`
- Integration：`tests/integration/transport/test_transport_adapter_loopback.cpp`

---

### Task-02（P0）先行接入 UDP 基线

1) 文件级变更
- 新增  
  - [`mini/net/udp/UdpSocket.h`](/home/xyq/mini-trantor/mini/net/udp/UdpSocket.h)  
  - [`mini/net/udp/UdpSocket.cc`](/home/xyq/mini-trantor/mini/net/udp/UdpSocket.cc)  
  - [`mini/net/udp/UdpServer.h`](/home/xyq/mini-trantor/mini/net/udp/UdpServer.h)  
  - [`mini/net/udp/UdpServer.cc`](/home/xyq/mini-trantor/mini/net/udp/UdpServer.cc)  
- 修改  
  - [`mini/net/Channel.h`](/home/xyq/mini-trantor/mini/net/Channel.h)  
  - [`mini/net/Channel.cc`](/home/xyq/mini-trantor/mini/net/Channel.cc)（仅完善事件语义与错误处理注释/钩子）  

2) 目标行为
- 建立 UDP 事件路径，不依赖 `TcpServer::newConnection` 的 stream 接口。
- 通过 `UdpServer` 实现 `onMessage(sessionId, packet, addr)`，与 TCP 并行运行。

3) 线程归属
- `UdpServer` 与 `UdpSocket` 仍受某个 `EventLoop` 管理（推荐与 base loop 同 loop）。
- `recv`/`send` 回调在该 loop 执行，不跨线程直接改 fd 监听状态。

4) 所有权
- `TcpServer` 不持有 UDP socket。
- `UdpServer` 内部创建的 `UdpSocket` 由 `unique_ptr` 持有；回调与会话 map 在其 io-loop 线程管理。

5) 回调重入点
- UDP `onPacket` 回调可能在单次 epoll cycle 内高频触发；回调必须限制堆积并快速返回。
- 若回调触发 session 广播，必须进入 `BroadcastDispatcher`。

6) 跨线程规则
- 发送广播/写会话缓存可跨线程请求，内部 `runInLoop` 投递到 UDP 所属 loop。

7) 测试映射（新增）
- Unit：`tests/unit/udp/test_udp_socket.cpp`
- Contract：`tests/contract/udp/test_udp_server_contract.cpp`
- Integration：`tests/integration/udp/test_udp_loopback.cpp`

---

### Task-03（P2）KCP 集成（可选增强）

1) 文件级变动
- 新增  
  - [`mini/net/kcp/KcpTransport.h`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.h)  
  - [`mini/net/kcp/KcpTransport.cc`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.cc)  
  - [`mini/net/kcp/KcpSession.h`](/home/xyq/mini-trantor/mini/net/kcp/KcpSession.h)  
  - [`mini/net/kcp/KcpSession.cc`](/home/xyq/mini-trantor/mini/net/kcp/KcpSession.cc)  
- 修改  
  - [`mini/net/transport/TransportManager.h`](/home/xyq/mini-trantor/mini/net/transport/TransportManager.h)  

2) 目标行为
- 在 UDP 之上建立可靠传输层，不改变 EventLoop 语义（所有 timer 归于 owner loop）。
- 保留 KCP 作为可选 backend，不与 TCP 路径共享会话状态逻辑。

3) 线程归属
- KCP 会话对象与对应 `EventLoop` 1:1 绑定
- `I/O` 与重传定时任务都挂在该 loop 的 `runAfter/runEvery`

4) 所有权
- `TransportManager` 按连接名持有 `shared_ptr<KcpSession>`；会话内部持有 UDP 句柄 `weak_ptr`，避免环。

5) 回调重入点
- KCP 定时 flush 可能触发 send 回调；发送完成回调与接收回调可同 tick 执行。

6) 跨线程
- 所有外部 API 经过 `queueInLoop`，禁止外部线程直接改输入/输出缓冲状态。

7) 测试映射（新增）
- Unit：`tests/unit/kcp/test_kcp_codec.cpp`
- Integration：`tests/integration/kcp/test_kcp_reliable_flow.cpp`

---

### Task-04（P0）base loop 广播路由器（减少连接图遍历）

1) 文件级变更
- 新增  
  - [`mini/net/broadcast/BroadcastRouter.h`](/home/xyq/mini-trantor/mini/net/broadcast/BroadcastRouter.h)  
  - [`mini/net/broadcast/BroadcastRouter.cc`](/home/xyq/mini-trantor/mini/net/broadcast/BroadcastRouter.cc)  
- 修改  
  - [`mini/net/TcpServer.h`](/home/xyq/mini-trantor/mini/net/TcpServer.h)  
  - [`mini/net/TcpServer.cc`](/home/xyq/mini-trantor/mini/net/TcpServer.cc)  

2) 目标行为
- 连接创建时更新“按 ioLoop 的 session 索引”；广播按目标逻辑树拆分到多个 loop buckets。
- base loop 不再直接逐个跨循环发送。

3) 线程归属
- `BroadcastRouter` 只在 base loop 线程维护全局索引表，做路由决策。
- 实际发送提交在目标 ioLoop 完成。

4) 所有权
- `TcpServer` 持有 `shared_ptr` Router；Router 持有轻量 weak index（session id->connection/session 映射）。

5) 回调重入点
- `removeConnection` 与广播注册/注销可能在同 tick 重入；操作必须是幂等且能接受“重复 remove/重复注册”。

6) 跨线程
- `broadcast()` 入口只要是逻辑/网络任意线程可调用，内部分发到 base loop 后再分发到各 ioLoop。

7) 测试映射
- Contract：`tests/contract/net/test_tcp_server_broadcast_router_contract.cpp`
- Integration：`tests/integration/tcp_server/test_tcp_server_broadcast_threaded.cpp`
- 复用：`tests/integration/tcp_server/test_tcp_server_threaded.cpp`（新增广播子场景）

---

### Task-05（P0）ioLoop 分桶批量发送（BroadcastDispatcher）

1) 文件级变更
- 新增  
  - [`mini/net/broadcast/BroadcastDispatcher.h`](/home/xyq/mini-trantor/mini/net/broadcast/BroadcastDispatcher.h)  
  - [`mini/net/broadcast/BroadcastDispatcher.cc`](/home/xyq/mini-trantor/mini/net/broadcast/BroadcastDispatcher.cc)  
- 修改  
  - [`mini/net/TcpConnection.h`](/home/xyq/mini-trantor/mini/net/TcpConnection.h)  
  - [`mini/net/TcpConnection.cc`](/home/xyq/mini-trantor/mini/net/TcpConnection.cc)  

2) 目标行为
- 同一 ioLoop 的广播消息批量入列，在一个 `queueInLoop` 中完成一次 flush，减少 wakeup 次数。

3) 线程归属
- 分发器构造/持有在 `TcpServer` 侧；执行 flush 在目标 ioLoop 线程。

4) 所有权
- `BroadcastDispatcher` 对 batch 命令持有 `shared_ptr<BroadcastBatch>`；发送结束回收。

5) 回调重入点
- 批量 flush 回调完成后会触发写完成回调；不可在回调里直接触发下一批 flush，建议重新入队以保证可裁剪。

6) 跨线程
- 批量项由逻辑线程/应用线程产生并投递；任何直接触发发送行为都必须走 owner loop。

7) 测试映射
- Unit：`tests/unit/broadcast/test_dispatcher_batch.cpp`
- Contract：`tests/contract/net/test_broadcast_batch_contract.cpp`
- Integration：`tests/integration/net/test_broadcast_fanout.cpp`

---

### Task-06（P0）零拷贝 + payload 生命周期

1) 文件级变更
- 新增  
  - [`mini/net/buffer/Payload.h`](/home/xyq/mini-trantor/mini/net/buffer/Payload.h)  
  - [`mini/net/buffer/Payload.cc`](/home/xyq/mini-trantor/mini/net/buffer/Payload.cc)  
  - [`mini/net/buffer/PayloadPool.h`](/home/xyq/mini-trantor/mini/net/buffer/PayloadPool.h)  
- 修改  
  - [`mini/net/broadcast/BroadcastDispatcher.h`](/home/xyq/mini-trantor/mini/net/broadcast/BroadcastDispatcher.h)  
  - [`mini/net/broadcast/BroadcastDispatcher.cc`](/home/xyq/mini-trantor/mini/net/broadcast/BroadcastDispatcher.cc)  

2) 目标行为
- 广播 payload 仅序列化一次，跨会话共享 `shared_ptr<Payload>`，发送时仅做引用计数+切片写出。

3) 线程归属
- `PayloadPool` 可在应用线程创建并安全复用，但生命周期回收在 owning loop 通过最终引用释放，不在 callback 内释放内部 mutable 状态。

4) 所有权
- 发布者持有 `PayloadPtr`，广播器短暂持有 `shared_ptr`，发送完成后归还池或由池回收。

5) 回调重入点
- 同一 payload 在 flush 前后可能被多次引用；不得在 `onWriteComplete` 中 mutate 不可共享内容。

6) 跨线程
- 跨线程仅可提交 payload 句柄；不应共享裸指针或可变 `Buffer` 引用。

7) 测试映射
- Unit：`tests/unit/buffer/test_payload_pool.cpp`
- Contract：`tests/contract/broadcast/test_payload_sharing_contract.cpp`
- Benchmark（可选）：`tests/integration/broadcast/test_broadcast_copy_count.cpp`

---

### Task-07（P0）通用 Packet Framing（粘包/半包）

1) 文件级变更
- 新增  
  - [`mini/net/framing/PacketFramer.h`](/home/xyq/mini-trantor/mini/net/framing/PacketFramer.h)  
  - [`mini/net/framing/PacketFramer.cc`](/home/xyq/mini-trantor/mini/net/framing/PacketFramer.cc)  
  - [`mini/net/framing/FrameType.h`](/home/xyq/mini-trantor/mini/net/framing/FrameType.h)  
- 修改  
  - [`mini/rpc/RpcCodec.h`](/home/xyq/mini-trantor/mini/rpc/RpcCodec.h)（复用 framer 的 length prefix strategy）  
  - 新增：`mini/http/HttpResponseContext.h/.cc`（若原先未合并，可将响应解析与 framing strategy 统一）  

2) 目标行为
- 所有高频游戏消息走统一帧结构：
  `magic + len + msg_id + flags + seq + payload`
- 统一处理 `incomplete / invalid / overlimit` 三类流控异常。

3) 线程归属
- Framer 为 per-connection/session 状态机，在 owner loop 线程调用。

4) 所有权
- Framer 由 `SessionContext` 持有（值对象或独占成员），随会话关闭销毁。

5) 回调重入点
- `onFrameDecoded` 可能同 tick 产生多个完整帧，应可复用循环并限制单次上限，防阻塞。

6) 跨线程
- 输入字节仅在 owner loop decode。跨线程只可投递原始 packet 引用，不可直接 decode state.

7) 测试映射
- Unit：`tests/unit/framing/test_framer.cpp`
- Contract：`tests/contract/framing/test_framer_contract.cpp`
- Integration：`tests/integration/transport/test_framing_halfpack.cpp`

---

### Task-08（P1）标准化序列化桥接（Protobuf/FlatBuffers）

1) 文件级变更
- 新增  
  - [`mini/codec/CodecAdapter.h`](/home/xyq/mini-trantor/mini/codec/CodecAdapter.h)  
  - [`mini/codec/ProtobufAdapter.h`](/home/xyq/mini-trantor/mini/codec/ProtobufAdapter.h)  
  - [`mini/codec/ProtobufAdapter.cc`](/home/xyq/mini-trantor/mini/codec/ProtobufAdapter.cc)  
  - [`mini/codec/FlatBuffersAdapter.h`](/home/xyq/mini-trantor/mini/codec/FlatBuffersAdapter.h)  
  - [`mini/codec/FlatBuffersAdapter.cc`](/home/xyq/mini-trantor/mini/codec/FlatBuffersAdapter.cc)  
- 修改  
  - [`mini/net/broadcast/BroadcastDispatcher.h`](/home/xyq/mini-trantor/mini/net/broadcast/BroadcastDispatcher.h)（编码阶段对接）  
  - [`mini/rpc/RpcChannel.h`](/home/xyq/mini-trantor/mini/rpc/RpcChannel.h)（可选：统一 payload encoder 接口）  

2) 目标行为
- 提供 `Encoder/Decoder` 可插拔链，游戏协议只关心 `GameMessage` 与 `Payload`。

3) 线程归属
- 编码器可无锁纯函数化（线程安全）或通过无状态单例。

4) 所有权
- `CodecAdapter` 由服务/场景注入，不跨连接持久占用可变状态。

5) 回调重入点
- 解码失败回调要可重入，防止反序列化抛错后影响连接生命周期。

6) 跨线程
- 编码/解码可在逻辑线程执行，但提交网络发送时仅提交 payload handle 到对应 ioLoop。

7) 测试映射
- Unit：`tests/unit/codec/test_protobuf_adapter.cpp`，`tests/unit/codec/test_flatbuffers_adapter.cpp`
- Contract：`tests/contract/codec/test_codec_adapter_contract.cpp`
- Integration：`tests/integration/codec/test_game_message_roundtrip.cpp`

---

### Task-09（P0）PlayerSession / SessionManager

1) 文件级变更
- 新增  
  - [`mini/game/PlayerSession.h`](/home/xyq/mini-trantor/mini/game/PlayerSession.h)  
  - [`mini/game/PlayerSession.cc`](/home/xyq/mini-trantor/mini/game/PlayerSession.cc)  
  - [`mini/game/SessionManager.h`](/home/xyq/mini-trantor/mini/game/SessionManager.h)  
  - [`mini/game/SessionManager.cc`](/home/xyq/mini-trantor/mini/game/SessionManager.cc)  
- 修改  
  - [`mini/http/HttpServer.h`](/home/xyq/mini-trantor/mini/http/HttpServer.h)（如需会话注入入口）  
  - [`mini/rpc/RpcServer.h`](/home/xyq/mini-trantor/mini/rpc/RpcServer.h)（如需 token 鉴权钩子）  

2) 目标行为
- `TcpConnection` 专注传输；`PlayerSession` 负责身份、背包/角色状态引用、权限、心跳状态、离线窗口状态。

3) 线程归属
- 会话管理器初始化在 base 或逻辑主 loop；会话的“读写关键状态”在逻辑 loop，`Transport` 事件线程只持有会话 id/弱引用。

4) 所有权
- `SessionManager` 持有 `shared_ptr<PlayerSession>`；连接侧仅持有 `weak_ptr<PlayerSession>`，防止循环引用。

5) 回调重入点
- `onConnectionClose`、`onAuthTimeout`、`onHeartbeatTimeout` 可能重叠触发，需幂等处理（状态机检查）。

6) 跨线程
- 认证/登录命令可能在逻辑线程完成，连接关闭通知通过 `queueInLoop` 回到 I/O loop。

7) 测试映射
- Unit：`tests/unit/game/test_player_session_fsm.cpp`
- Contract：`tests/contract/game/test_session_manager_contract.cpp`
- Integration：`tests/integration/game/test_connect_auth_replay.cpp`

Task-09 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- `SessionManager` 所属循环：由构造或 `setOwnerLoop()` 注入的 `mini::net::EventLoop*`（通常为逻辑 loop）。  
- `PlayerSession` 状态字段与会话元数据由 `SessionManager` 作为单线程逻辑入口进行管理；业务层可并发查询，但状态变更建议走 `SessionManager` API。

2. Who owns it and who releases it?
- 所有权：`SessionManager` 持有 `std::shared_ptr<PlayerSession>`（`sessions_`）。  
- 引用策略：连接侧与上层仅持有 `std::weak_ptr`（`SessionManager::getSessionWeak`）或短暂 `shared_ptr`。  
- 释放：`removeSession/removeSessionByTransport` 删除 map 和 transport 索引；`cleanupClosedSessions` 清理已关闭会话并触发 `close` 回调；无引用后会话会自然析构。

3. Which callbacks may re-enter?
- 状态回调 `SessionStateCallback` 可能在任意入口被触发（包括状态无变化被尝试的边界输入），因此要按 `old != new` 和状态幂等设计（`PlayerSession` 内部拒绝非法迁移）。  
- `SessionManager::emitState` 会统一在逻辑 loop 发射回调，避免跨线程回调重入；业务侧需保证回调本身可重入安全。

4. Which operations are allowed cross-thread, and how are they marshaled?
- 对外状态类操作允许跨线程调用，但 `SessionManager` 内部通过 `postOnLogicLoop()`（`runInLoop/queueInLoop` 语义）回到逻辑 loop 发射回调。  
- 底层 transport 身份写入保持在 manager 保护区内，采用 mutex + 索引更新；查找/绑定使用原子化流程避免悬挂映射。

5. Which test file verifies this change?
- Thread-affinity：`tests/contract/game/test_session_manager_contract.cpp::testStateCallbackMarshalsToLogicLoop`
- 生命周期/迁移：`tests/contract/game/test_session_manager_contract.cpp::testTransportRebindAndRemoval`、`testTransportRebindEvictsPreviousOwner`
- 失败路径/边界：`tests/contract/game/test_session_manager_contract.cpp::testRemoveFailurePaths`

---

### Task-10（P1）断线重连与 Session Sticky

1) 文件级变更
- 修改  
  - [`mini/game/SessionManager.h`](/home/xyq/mini-trantor/mini/game/SessionManager.h)  
  - [`mini/game/PlayerSession.h`](/home/xyq/mini-trantor/mini/game/PlayerSession.h)  
  - [`mini/game/PlayerSession.cc`](/home/xyq/mini-trantor/mini/game/PlayerSession.cc)

2) 目标行为
- 断线后在短时窗口内保留 session state；重连时可基于 token 恢复 transport 与会话上下文。  
- 窗口过期后会话回收并进入 `Closed`，后续同 token 重连将创建新会话。
- 连接对象可在 close 时释放，SessionManager 维持可恢复窗口态并避免重复绑定回调/状态抖动。

3) 线程归属
- 会话状态在 `SessionManager`（逻辑 loop）推进；网络 close 回调仅提交事件并触发逻辑 loop 的重连窗口计时。

4) 所有权
- `SessionManager` 持有 `shared_ptr<PlayerSession>`；`transport` 生命周期只通过 session 索引跟踪。  
- 重连超时后由 `removeSession` 收敛所有权并清理 transport 绑定；未关闭时 session 仍可被 `ensureSession` 重绑恢复。

5) 回调重入点
- `onReconnect` 与 `onReconnectWindowExpired` 可能并发到达；通过 `reconnectEpoch` + timer id 防重入并避免重复恢复/关闭。

6) 跨线程
- Network close -> 在 manager 通过 `runAfter/cancel` 在逻辑 loop 内调度重连窗口；状态回调统一 `postOnLogicLoop` 到逻辑 loop。
- `SessionManager::bindTransport` 负责更新 transport 索引和状态转换，避免跨线程直接写入 mapping。

7) 测试映射
- Unit：`tests/unit/game/test_player_session_fsm.cpp`
- Contract：`tests/contract/game/test_session_manager_contract.cpp`
- Integration：`tests/integration/game/test_connect_auth_replay.cpp`、`tests/integration/game/test_reconnect_flow.cpp`

Task-10 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- `SessionManager` 的会话生命周期归属逻辑 loop（通过构造或 `setOwnerLoop` 注入的 `mini::net::EventLoop*`）。
- `onConnectionClose`、`setTransportIndexLocked` 在内部同步保护；`stateCallback` 统一投递到 owner loop。

2. Who owns it and who releases it?
- 所有权：`SessionManager` 持有 `sessions_` 的 `shared_ptr<PlayerSession>`；`transportIndex_` 仅做反查。
- 释放：`removeSession/removeSessionByTransport` 和重连窗口过期路径（`onReconnectWindowExpired`）回收 `session`，并清理重连 timer/epoch 映射。

3. Which callbacks may re-enter?
- 状态回调 `SessionStateCallback` 可能被多个路径触发（`markReconnecting`、`markOnline`、`onConnectionClose`、超时关闭），回调入口统一去重入后在逻辑 loop 执行。
- `onConnectionClose` 与重连定时器到期都可能同时触发同一 token 的收敛，需保证幂等。

4. Which operations are allowed cross-thread, and how are they marshaled?
- 允许跨线程调用：`ensureSession`、`onConnectionClose`、`mark*`、`onReconnect`、`removeSession`（通过外部服务路径）。
- 实现做法：所有状态变更回调通过 `postOnLogicLoop` marshal；重连窗口 timer/cancel 在逻辑 loop 统一调度，并使用 mutex 保证 `reconnectTimer_` 与 `reconnectEpoch_` 一致性。

5. Which test file verifies this change?
- 线程 / 回调重入：`tests/contract/game/test_session_manager_contract.cpp::testStateCallbackMarshalsToLogicLoop`
- 生命周期：`tests/integration/game/test_reconnect_flow.cpp`（窗口内复用 / 过期重建）
- 边界行为：`tests/contract/game/test_session_manager_contract.cpp::testRemoveFailurePaths`、
  `tests/contract/game/test_session_manager_contract.cpp::testOnReconnectFailureForMissingSession`、
  `tests/integration/game/test_reconnect_flow.cpp`

---

### Task-11（P0）LogicLoop 固定步长与命令队列

1) 文件级变更
- 新增  
  - [`mini/game/logic/LogicLoop.h`](/home/xyq/mini-trantor/mini/game/logic/LogicLoop.h)  
  - [`mini/game/logic/LogicLoop.cc`](/home/xyq/mini-trantor/mini/game/logic/LogicLoop.cc)  
  - [`mini/game/logic/GameCommandQueue.h`](/home/xyq/mini-trantor/mini/game/logic/GameCommandQueue.h)  
  - [`mini/game/logic/GameCommandQueue.cc`](/home/xyq/mini-trantor/mini/game/logic/GameCommandQueue.cc)  
- 修改  
  - [`mini/net/TcpServer.h`](/home/xyq/mini-trantor/mini/net/TcpServer.h)（新增 logic callback 注入）  
  - [`mini/net/TcpServer.cc`](/home/xyq/mini-trantor/mini/net/TcpServer.cc)（消息路径挂接逻辑命令入队）  

2) 目标行为
- I/O loop 仅产生 `GameCommand`（解析+排队），不直接执行业务计算。
- `LogicLoop` 按 fixed-step（例如 16ms）消费并产出输出命令，回传给 I/O loop 发送。

3) 线程归属
- `LogicLoop` 独占一个线程（或线程池 worker），与 I/O loop 解耦。
- 网络发送必须回到各 connection 的 owner loop。

4) 所有权
- 命令对象由逻辑入口持有并通过 `shared_ptr<Command>` 在队列流动，消费后回收。

5) 回调重入点
- 高优先级命令可能在同 tick 连续入队；`LogicLoop` 需裁切（max commands per tick）并防止饥饿。

6) 跨线程
- 逻辑线程与网络线程之间只通过无锁队列 + owner-loop 封装发送，避免直接访问 `TcpConnection` 的成员。

7) 测试映射
- Unit：`tests/unit/logic/test_game_command_queue.cpp`
- Contract：`tests/contract/logic/test_logic_loop_timing_contract.cpp`
- Integration：`tests/integration/logic/test_network_to_logic_roundtrip.cpp`

Task-11 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- `LogicLoop` 的计算与 `fixed-step` tick 消费运行在独占 `EventLoopThread` 对应的逻辑 loop 线程。
- `GameCommandQueue` 本身无独立 loop，队列入队/出队在多线程上加锁保护；命令在逻辑 loop tick 中 drain。

2. Who owns it and who releases it?
- 所有权：`LogicLoop` 对 `EventLoopThread`、`GameCommandQueue` 和 `CommandProcessor`/`OutputDispatcher` 持有成员级所有权。
- 释放：`LogicLoop` stop 时取消 tick timer、清空 queue、并退出逻辑 loop；`GameCommand` 由 `shared_ptr` 持有，在 `tick`/dispatch 阶段转移并自动析构。

3. Which callbacks may re-enter?
- `CommandProcessor` 在单次 tick 中被逻辑 loop 调用，允许在业务内产生命令输出并复用 `TcpConnection` 的发送回调。
- `OutputDispatcher` 在逻辑 loop 线程执行，内部仅通过 `connection->getLoop()->queueInLoop` 回写 IO loop，避免跨线程重入 `send`。
- `TcpServer::setLogicMessageCallback` 在 IO loop 的 message 入口触发；此回调只做命令建模与入队，不应做耗时计算。

4. Which operations are allowed cross-thread, and how are they marshaled?
- `TcpServer` message callback 可从 IO loop 线程并发调用 `LogicLoop::submit`；当逻辑循环处于 running 状态时，`submit` 主要做加锁入队。
- `LogicLoop` 的逻辑处理通过 `EventLoop::runEvery(fixedStep)` 在逻辑 loop 内驱动。
- 逻辑产出的响应通过默认 `OutputDispatcher` 回到 `connection->getLoop()->queueInLoop`，所有 `send` 仍在 connection owner loop 执行。

5. Which test file verifies this change?
- 线程归属与回调位置：`tests/contract/logic/test_logic_loop_timing_contract.cpp`
- 生命周期/并发关闭：`tests/contract/logic/test_logic_loop_timing_contract.cpp`
- 边界行为（backlog/max-commands-lane/停止后不再处理）：`tests/contract/logic/test_logic_loop_timing_contract.cpp`
- 队列契约：`tests/unit/logic/test_game_command_queue.cpp`
- 真实闭环网络->逻辑->回写：`tests/integration/logic/test_network_to_logic_roundtrip.cpp`

---

### Task-12（P1）监控与可观测性升级

1) 文件级变更
- 修改  
  - [`mini/base/MetricsHook.h`](/home/xyq/mini-trantor/mini/base/MetricsHook.h)  
  - [`mini/net/TcpServerOptions.h`](/home/xyq/mini-trantor/mini/net/TcpServerOptions.h)（新增网络行为指标开关）  
  - [`mini/net/TcpServer.h`](/home/xyq/mini-trantor/mini/net/TcpServer.h)  
  - [`mini/net/TcpServer.cc`](/home/xyq/mini-trantor/mini/net/TcpServer.cc)  
  - [`mini/net/TcpConnection.h`](/home/xyq/mini-trantor/mini/net/TcpConnection.h)
  - [`mini/net/TcpConnection.cc`](/home/xyq/mini-trantor/mini/net/TcpConnection.cc)

2) 目标行为
- 新增指标：
  - 广播 fanout 延迟、跨循环入队延迟、payload 大小分布
  - session 级重连成功率、重连耗时
  - fixed-step backlog 与 lag（逻辑落后毫秒）
  - 每-loop pending functor 峰值与 wakeup 次数

3) 线程归属
- hook 回调保持在发生事件的 owner loop 回调线程执行，`MetricsHook` 保持“只读记录者”职责。

4) 所有权
- `TcpServer` 与 `TcpConnection` 只持有 hook functor；生命周期与 stop/destruct 同步清理，防止 dangling callback。

5) 回调重入点
- metrics 回调可能和状态日志并发频发，需设计轻量（避免在 callback 内阻塞）并防止 re-enter 递归计数错误。

6) 跨线程
- 跨线程仅允许 `snapshot` 报告（原子计数/聚合对象）读取，不允许直接 mutate io-loop 内部指标缓冲。

7) 测试映射
- Unit：`tests/unit/metrics/test_metrics_hook_ext.cpp`
- Contract：`tests/contract/net/test_game_metrics_contract.cpp`
- Integration：`tests/integration/benchmark/test_fps_like_broadcast_latency.cpp`（轻量压测）

---

## 3. 文件级改动门禁（所有 P0/P1/P2 任务共用）

每个核心文件的 PR 都应包含“核心模块改动闸门”标准段落（可复用 `docs/core_module_change_gate.md` 要求）：
- Loop/Thread
- Ownership / Release
- Re-entrant callbacks
- Cross-thread Operations
- Test Mapping（精确文件路径）

建议新增模板（落地到每个 PR/提交）：
- `Core Module Reference`：指向 `intent` + `rules`
- `Change Gate`：5 个问题逐条回答
- `Exit Criteria`：任务级可验证条件（含测试名）

---

## 4. 最小实现顺序（推荐）

### 先决顺序（M1–M14）
1. **M1**：Task-01（统一传输抽象）  
2. **M2**：Task-04（base loop 广播路由）  
3. **M3**：Task-05（ioLoop 分桶批量发送）  
4. **M4**：Task-06（零拷贝 payload）  
5. **M5**：Task-07（通用 framing）  
6. **M6**：Task-09（PlayerSession）  
7. **M7**：Task-11（LogicLoop + 命令队列）  
8. **M8**：Task-12（指标升级）  
9. **M9**：Task-08（序列化适配）  
10. **M10**：Task-10（Reconnection）  
11. **M11**：Task-02（UDP 基线）  
12. **M12**：Task-03（KCP）  

### 并行窗口
- Task-06 可在 Task-11 完成前并行进行，但需要先有 session 接口。
- Task-08 与 Task-07 可并行（但序列化输出对象建议等 Task-07 的 frame schema 确认后再 merge）。
- Task-12 可与 Task-09/10 并行进行，避免等待全部数据模型落地。

### 每完成一个任务必须完成
- 对应新/改文件的最小回归测试（至少 1 条）
- 关键旧测试不回归（如 `tests/contract/tcp_server/test_tcp_server.cpp`）
- 文档与 `intent` 的阶段对应关系更新（v6 之后可能新增子阶段文档）

---

## 5. 交付建议（两周制）

### Sprint A（第 1~2 周）
- 完成 Task-01/04/05/06/07

### Sprint B（第 3~4 周）
- 完成 Task-09/11/12（可配合协议最小接入）

### Sprint C（第 5~6 周）
- 完成 Task-08/10/02

### Sprint D（可选）
- Task-03（KCP）与 AOI/多分片高级优化（在 Task-02 基座稳定后补）

---

## 6. 直接行动清单（本地执行）

1. 先在 docs 中新增 `intents/architecture/v7_game_server_base.intent.md`（可选）：记录新阶段边界。
2. 冻结本执行清单对应的每个任务的接口草图，再批量起 PR。
3. 每个 PR 结束时补齐：
   - `Core Module Change Gate`
   - 新增/修改测试文件列表
   - “未完待续”项（非阻塞问题）声明。
