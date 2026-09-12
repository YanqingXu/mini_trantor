# Game Server Networking Base Execution Plan

> Context: Current baseline is `mini-trantor` v6-alpha（客户端生态阶段，目标是把网络库演进为通用游戏服务器底座）。
>
> Scope closure: M1-M32 freeze the current state as `game-network foundation + transport preview`.
> The next milestone is M33 scope hardening, not another KCP production feature.

## 0. 执行目标与边界

### 0.1 目标
把现有 TCP/TLS/Reactor 基础上扩展到一个可用于游戏服务器的网络底座，兼顾：
- 传输层多样性（TCP/TLS/UDP/KCP可组合）
- 大规模跨线程广播（低拷贝、低线程切换）
- 标准化序列化（Protobuf/FlatBuffers）
- 会话重建与断线重连
- 逻辑层 fixed-step 运行模型协作
- 可观测性增强（面向低延迟高并发）
- 网关入口安全骨架（auth validator、防重放、限频、关闭原因）
- 游戏层背压优先级与自适应 soft threshold
- 指标标签化与无依赖文本导出适配
- KCP 固定 MTU 分片与 large payload contract
- KCP 高丢包长时 contract 与 retry/RTO 参数化
- KCP selective ACK 预览
- KCP dynamic MTU probe/backoff 预览
- KCP congestion-window 预览
- KCP redundant-copy 预览
- KCP PMTU blackhole cooldown 预览
- KCP transport-local MTU path cache 预览
- KCP XOR parity recovery 预览
- KCP path MTU failure / ICMP signal 预览
- KCP Linux UDP error queue PMTU signal 接入
- KCP IPv4 raw ICMP PMTU signal listener 预览
- KCP ICMPv6 raw PMTU signal listener 预览
- KCP/UDP cross-platform PMTU signal adapter 预览
- KCP cross-transport shared PathMtuCache 预览
- KCP userspace raw ICMP PMTU signal authentication 预览
- UDP platform PMTU source capability/query 预览

### 0.1.1 收口定位

当前路线图不再把后续生产级 KCP、FEC、AOI、安全平台或观测平台实现作为 core 主线。

M1-M32 的交付物被定位为：

- `core`：Reactor、TCP、UDP 基线、transport abstraction、framing、metrics hook。
- `game-foundation`：SessionManager、GameServerPipeline、LogicLoop、Broadcast、基础安全和基础背压。
- `transport-preview`：KCP/PMTU/raw ICMP/redundant-copy/XOR parity/congestion-window preview。
- `out-of-core adapters`：真实 auth provider、安全审计、AOI、多进程网关、生产 FEC、生产 congestion control、可部署观测端点。

后续新增能力必须先通过 `docs/game_server_network_base_scope_boundary.md` 和
`intents/architecture/game_network_base_scope.intent.md` 的 scope gate。

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

## 1. M1-M32 已冻结任务总览（32 项）

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
13. **Task-13（P1）** 游戏网关安全骨架：auth token validator、connect-auth replay window、per-session rate limit、异常关闭原因指标
14. **Task-14（P1）** 游戏层背压优先级：output / broadcast soft-zone shedding 与自适应 soft threshold
15. **Task-15（P1）** Metrics exporter 标签化与文本导出：static labels、Prometheus text snapshot、tagged metrics smoke
16. **Task-16（P1）** KCP 固定 MTU 分片：large payload split/reassembly、16-bit payload boundary、损伤网络分片 contract
17. **Task-17（P1）** KCP 高丢包长时：retry/RTO options、周期性高损伤长消息流 contract
18. **Task-18（P1）** KCP selective ACK 预览：ACK payload SACK entries、乱序缺口重传抑制 contract
19. **Task-19（P1）** KCP dynamic MTU probe/backoff 预览：per-session datagram size、probe ACK、成功升 MTU 与失败回退 contract
20. **Task-20（P1）** KCP congestion-window 预览：per-session sendQueue、in-flight window cap、ACK drain contract
21. **Task-21（P1）** KCP redundant-copy 预览：bounded same-seq data duplicates、首发丢包副本覆盖 contract
22. **Task-22（P1）** KCP PMTU blackhole cooldown 预览：probe retry exhaustion 后冷却抑制、safe-size data delivery、后续 re-probe contract
23. **Task-23（P1）** KCP transport-local MTU path cache 预览：同 peer reopen 复用 confirmed size / blackhole cooldown contract
24. **Task-24（P1）** KCP XOR parity recovery 预览：bounded parity group、每组一个 data 丢包恢复、无需 RTO contract
25. **Task-25（P1）** KCP path MTU failure / ICMP signal 预览：显式路径 MTU 失败通知、safe-size downgrade、cache cooldown reuse contract
26. **Task-26（P1）** KCP Linux UDP error queue PMTU signal 接入：`IP_RECVERR` / `IPV6_RECVERR`、local `EMSGSIZE`、owner-loop PMTU signal callback contract
27. **Task-27（P1）** KCP IPv4 raw ICMP PMTU signal listener 预览：Linux raw ICMP Packet Too Big parser/listener、quoted UDP filter、owner-loop callback contract
28. **Task-28（P1）** KCP ICMPv6 raw PMTU signal listener 预览：Linux raw ICMPv6 Packet Too Big parser/listener、quoted UDP filter、same owner-loop PMTU callback contract
29. **Task-29（P1）** KCP/UDP cross-platform PMTU signal adapter 预览：抽离 `PathMtuSignalAdapter`，Linux `MSG_ERRQUEUE` 实现 + portable no-op fallback、owner-loop adapter contract
30. **Task-30（P1）** KCP cross-transport shared PathMtuCache 预览：显式注入线程安全 in-process cache，transport restart/rebind 后复用 confirmed size / cooldown contract
31. **Task-31（P1）** KCP userspace raw ICMP PMTU signal authentication 预览：UDP 保留 bounded quoted payload evidence，KCP 可选按 quoted KCP magic/version/session id 认证 raw ICMP PMTU signal
32. **Task-32（P1）** UDP platform PMTU source capability/query 预览：adapter 暴露 platform PMTU capability facts、generic configure/drain alias 和 connected-socket MTU query hook，Linux error queue 路径保持运行态 source

### M33（P0）Scope Boundary Hardening

目标不是新增传输能力，而是阻止底座继续无边界膨胀：

- 新增 `intents/architecture/game_network_base_scope.intent.md`。
- 新增 `docs/game_server_network_base_scope_boundary.md`。
- 将 KCP/PMTU/FEC 类能力统一标记为 `transport-preview` / `transport-experimental`。
- 将高级安全、AOI、多进程网关、观测平台和生产传输研发移出 core roadmap。
- 后续 PR/change description 必须回答 scope gate：属于 core、game-foundation、transport-preview、adapter 还是 example。

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
- `UdpSocket` 每次 read handler 至多处理配置的 datagram budget；budget 用尽后把控制权还给 `EventLoop`，剩余 datagram 由后续 level-triggered readiness 继续驱动。
- UDP read-batch metric 记录 datagram 数、字节数、读批次耗时和 budget 是否用尽，供突发包公平性调优使用。

3) 线程归属
- `UdpServer` 与 `UdpSocket` 仍受某个 `EventLoop` 管理（推荐与 base loop 同 loop）。
- `recv`/`send` 回调在该 loop 执行，不跨线程直接改 fd 监听状态。

4) 所有权
- `TcpServer` 不持有 UDP socket。
- `UdpServer` 内部创建的 `UdpSocket` 由 `unique_ptr` 持有；回调与会话 map 在其 io-loop 线程管理。

5) 回调重入点
- UDP `onPacket` 回调可能在单次 epoll cycle 内高频触发；回调必须限制堆积并快速返回。
- 若回调触发 session 广播，必须进入 `BroadcastDispatcher`。
- metric callback 与 packet callback 均在 owner loop 同步执行，必须轻量，不得阻塞读批次返回。

6) 跨线程规则
- 发送广播/写会话缓存可跨线程请求，内部 `runInLoop` 投递到 UDP 所属 loop。

7) 测试映射（新增）
- Unit：`tests/unit/udp/test_udp_socket.cpp`
- Contract：`tests/contract/udp/test_udp_server_contract.cpp`
- Integration：`tests/integration/udp/test_udp_loopback.cpp`、`tests/integration/udp/test_udp_sendto_stop_lifecycle.cpp`

---

### Task-03（P2）KCP 集成（可选增强）

1) 文件级变动
- 新增  
  - [`intents/modules/kcp_transport.intent.md`](/home/xyq/mini-trantor/intents/modules/kcp_transport.intent.md)
  - [`mini/net/kcp/KcpTransport.h`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.h)  
  - [`mini/net/kcp/KcpTransport.cc`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.cc)  
  - [`mini/net/kcp/KcpSession.h`](/home/xyq/mini-trantor/mini/net/kcp/KcpSession.h)  
  - [`mini/net/kcp/KcpSession.cc`](/home/xyq/mini-trantor/mini/net/kcp/KcpSession.cc)  
- 修改  
  - [`mini/net/transport/TransportManager.h`](/home/xyq/mini-trantor/mini/net/transport/TransportManager.h)  

2) 目标行为
- 在 UDP 之上建立可靠传输层，不改变 EventLoop 语义（所有 timer 归于 owner loop）。
- 保留 KCP 作为可选 backend，不与 TCP 路径共享会话状态逻辑。
- 停止后的 KCP session/raw peer 发送请求必须在 owner loop 闸门处丢弃，不得继续向 UDP fd 发包。
- preview 压力 contract 覆盖丢包、乱序、重复包、延迟抖动后的按序一次性交付，以及 stop/close/send 并发清理。

3) 线程归属
- KCP 会话对象与对应 `EventLoop` 1:1 绑定
- `I/O` 与重传定时任务都挂在该 loop 的 `runAfter/runEvery`

4) 所有权
- `TransportManager` 按连接名持有 `shared_ptr<KcpSession>`；会话内部持有 UDP 句柄 `weak_ptr`，避免环。

5) 回调重入点
- KCP 定时 flush 可能触发 send 回调；发送完成回调与接收回调可同 tick 执行。

6) 跨线程
- 所有外部 API 经过 `queueInLoop`，禁止外部线程直接改输入/输出缓冲状态；`openSession()` 跨线程调用同步 marshal 到 owner loop 后返回 session 结果。

7) 测试映射（新增）
- Unit：`tests/unit/kcp/test_kcp_codec.cpp`
- Contract：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`
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
- 底层序列化函数或注入回调抛出的异常必须被适配器转为显式错误返回，不外泄到连接生命周期路径。

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
- 旧连接 close callback 晚于新连接同 token 绑定到达时，不得注销新连接已经恢复的广播 session / group / AOI 路由。

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
- Contract：`tests/contract/game/test_session_manager_contract.cpp`、`tests/contract/net/test_tcp_server_broadcast_router_contract.cpp`
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
  - [`mini/base/MetricsExporter.h`](/home/xyq/mini-trantor/mini/base/MetricsExporter.h)
  - [`mini/base/MetricsExporter.cc`](/home/xyq/mini-trantor/mini/base/MetricsExporter.cc)
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
- 新增 exporter：
  - `MetricsExporter` 提供可替换 counter / histogram 写入接口
  - `InMemoryMetricsExporter` 提供 CI / benchmark 可断言的内存聚合
  - `MetricsHookRecorder` 将 typed hook callback 转成 exporter 写入，不改变 hook owner-loop 触发语义

3) 线程归属
- hook 回调保持在发生事件的 owner loop 回调线程执行，`MetricsHook` 保持“只读记录者”职责。
- exporter 不拥有 loop；in-memory 聚合器内部 mutex 保护跨 owner loop 记录。

4) 所有权
- `TcpServer` 与 `TcpConnection` 只持有 hook functor；生命周期与 stop/destruct 同步清理，防止 dangling callback。
- `MetricsHookRecorder` 生成的 callback 只捕获 exporter shared ownership，不捕获 reactor/game 对象。

5) 回调重入点
- metrics 回调可能和状态日志并发频发，需设计轻量（避免在 callback 内阻塞）并防止 re-enter 递归计数错误。

6) 跨线程
- 跨线程仅允许 `snapshot` 报告（原子计数/聚合对象）读取，不允许直接 mutate io-loop 内部指标缓冲。

7) 测试映射
- Unit：`tests/unit/metrics/test_metrics_hook_ext.cpp`
- Unit：`tests/unit/metrics/test_metrics_exporter.cpp`
- Contract：`tests/contract/net/test_metrics_exporter_contract.cpp`
- Contract：`tests/contract/net/test_game_metrics_contract.cpp`
- Integration：`tests/integration/benchmark/test_fps_like_broadcast_latency.cpp`（广播轻量压测）
- Integration：`tests/integration/benchmark/test_game_server_metrics_smoke.cpp`（游戏服指标闭环烟测）

---

### Task-13（P1）游戏网关安全骨架

1) 文件级变更
- 新增
  - [`intents/modules/game_gateway_security.intent.md`](/home/xyq/mini-trantor/intents/modules/game_gateway_security.intent.md)
  - [`mini/game/GameGatewaySecurityPolicy.h`](/home/xyq/mini-trantor/mini/game/GameGatewaySecurityPolicy.h)
  - [`tests/unit/game/test_game_gateway_security_policy.cpp`](/home/xyq/mini-trantor/tests/unit/game/test_game_gateway_security_policy.cpp)
  - [`tests/contract/game/test_game_gateway_security_contract.cpp`](/home/xyq/mini-trantor/tests/contract/game/test_game_gateway_security_contract.cpp)
  - [`tests/integration/game/test_game_gateway_security.cpp`](/home/xyq/mini-trantor/tests/integration/game/test_game_gateway_security.cpp)
- 修改
  - [`mini/base/MetricsHook.h`](/home/xyq/mini-trantor/mini/base/MetricsHook.h)
  - [`mini/base/MetricsExporter.h`](/home/xyq/mini-trantor/mini/base/MetricsExporter.h)
  - [`mini/base/MetricsExporter.cc`](/home/xyq/mini-trantor/mini/base/MetricsExporter.cc)
  - [`mini/game/GameServerPipeline.h`](/home/xyq/mini-trantor/mini/game/GameServerPipeline.h)
  - [`mini/game/GameServerPipeline.cc`](/home/xyq/mini-trantor/mini/game/GameServerPipeline.cc)
  - [`tests/unit/metrics/test_metrics_hook_ext.cpp`](/home/xyq/mini-trantor/tests/unit/metrics/test_metrics_hook_ext.cpp)
  - [`tests/unit/metrics/test_metrics_exporter.cpp`](/home/xyq/mini-trantor/tests/unit/metrics/test_metrics_exporter.cpp)

2) 目标行为
- `GameSecurityOptions` 作为值语义配置接入 `GameServerPipeline::Options::validate()`。
- 默认配置关闭安全策略，不改变旧 auth 行为；auth payload 仍整体作为 session token。
- 启用 replay window 后，auth payload 可按 `sessionToken|nonce` 解析，同 `(sessionToken, nonce)` 在窗口内重放会被拒绝。
- 同 session token 使用 fresh nonce 仍允许 sticky reconnect，不把 replay policy 错误等同于“同 token 不可重连”。
- 支持注入 `AuthTokenValidator`，在进入 `SessionManager` 前执行最小 admission。
- 已认证 session 的非 auth frame 经过 per-session rate window；越限后关闭连接。
- `GameSecurityMetricSample` 区分 AuthAccepted、AuthRejected、RateLimited、AbnormalClose，并记录 reason/current/limit/payload bytes。

3) 线程归属
- auth parsing、validator、replay check、rate-limit check 均在连接 owner I/O loop 的 pipeline message path 上执行。
- replay/rate cache 归 `GameServerPipeline` 持有；同一 pipeline 可能被多个 I/O loop 回调，因此通过 `securityMutex_` 显式同步。
- security metric callback 在触发事件的 owner loop 上同步执行。

4) 所有权
- `GameServerPipeline` 借用 `TcpServer`、`TransportManager`、`SessionManager`、`LogicLoop`，不拥有它们。
- `GameServerPipeline` 拥有 security options、validator functor、replay cache、rate cache 和 metric callback。
- `SessionManager` 仍是 `PlayerSession` owner；security admission 不直接推进 session 状态。
- `TcpConnection` 仍拥有 close 状态机；security 只请求 `forceClose()`。

5) 回调重入点
- `AuthTokenValidator` 与 security metric callback 在连接 owner loop 同步触发；它们必须轻量、可重入，不应阻塞账号系统或调用长耗时业务逻辑。
- auth reject、rate reject 和 invalid frame close 都可能与 close callback、broadcast unbind、session close event 在同 tick 交错，路径需保持幂等。

6) 跨线程
- `setAuthTokenValidator()` 和 `setSecurityMetricCallback()` 可在安装前设置；如果运行时更新，也只通过 setter 锁替换 functor。
- replay/rate 共享状态不直接暴露外部跨线程 API。
- 关闭统一走 `TcpConnection::forceClose()`，由 connection owner loop 收敛生命周期。

7) 测试映射
- Unit：`tests/unit/game/test_game_gateway_security_policy.cpp`
- Unit：`tests/unit/metrics/test_metrics_hook_ext.cpp`
- Unit：`tests/unit/metrics/test_metrics_exporter.cpp`
- Contract：`tests/contract/game/test_game_gateway_security_contract.cpp`
- Integration：`tests/integration/game/test_game_gateway_security.cpp`

Task-13 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- security admission 由连接 owner I/O loop 驱动；共享 replay/rate cache 由 `GameServerPipeline` mutex 保护。

2. Who owns it and who releases it?
- `GameServerPipeline` 拥有 security cache 和 functor；pipeline 析构时随成员释放。`SessionManager` 与 `TcpConnection` 的所有权边界不变。

3. Which callbacks may re-enter?
- auth validator、security metric callback、connection close callback、session close event 可能同 tick 交错；validator/metric 只能观察或返回 admission 结果。

4. Which operations are allowed cross-thread, and how are they marshaled?
- setter 通过锁替换 functor；连接关闭通过 `forceClose()` 回到 connection owner loop；session 状态仍通过 `SessionManager` API marshal。

5. Which test file verifies this change?
- 默认兼容与 fresh nonce reconnect：`tests/contract/game/test_game_gateway_security_contract.cpp`
- validator/replay/rate-limit 真实 TCP 路径：`tests/integration/game/test_game_gateway_security.cpp`
- options 与 metrics：`tests/unit/game/test_game_gateway_security_policy.cpp`、`tests/unit/metrics/test_metrics_hook_ext.cpp`、`tests/unit/metrics/test_metrics_exporter.cpp`

---

### Task-14（P1）游戏层背压优先级与自适应 soft threshold

1) 文件级变更
- 修改
  - [`intents/modules/game_backpressure_policy.intent.md`](/home/xyq/mini-trantor/intents/modules/game_backpressure_policy.intent.md)
  - [`mini/game/GameBackpressurePolicy.h`](/home/xyq/mini-trantor/mini/game/GameBackpressurePolicy.h)
  - [`mini/game/logic/GameCommandQueue.h`](/home/xyq/mini-trantor/mini/game/logic/GameCommandQueue.h)
  - [`mini/game/logic/LogicLoop.h`](/home/xyq/mini-trantor/mini/game/logic/LogicLoop.h)
  - [`mini/game/logic/LogicLoop.cc`](/home/xyq/mini-trantor/mini/game/logic/LogicLoop.cc)
  - [`mini/game/GameServerPipeline.cc`](/home/xyq/mini-trantor/mini/game/GameServerPipeline.cc)
  - [`mini/net/TcpServer.h`](/home/xyq/mini-trantor/mini/net/TcpServer.h)
  - [`mini/net/TcpServer.cc`](/home/xyq/mini-trantor/mini/net/TcpServer.cc)
  - [`mini/net/broadcast/BroadcastDispatcher.h`](/home/xyq/mini-trantor/mini/net/broadcast/BroadcastDispatcher.h)
  - [`mini/net/broadcast/BroadcastDispatcher.cc`](/home/xyq/mini-trantor/mini/net/broadcast/BroadcastDispatcher.cc)
  - [`mini/base/MetricsHook.h`](/home/xyq/mini-trantor/mini/base/MetricsHook.h)
  - [`mini/base/MetricsExporter.cc`](/home/xyq/mini-trantor/mini/base/MetricsExporter.cc)
  - [`tests/unit/game/test_game_backpressure_policy.cpp`](/home/xyq/mini-trantor/tests/unit/game/test_game_backpressure_policy.cpp)
  - [`tests/contract/logic/test_logic_loop_timing_contract.cpp`](/home/xyq/mini-trantor/tests/contract/logic/test_logic_loop_timing_contract.cpp)
  - [`tests/integration/game/test_game_backpressure_policy.cpp`](/home/xyq/mini-trantor/tests/integration/game/test_game_backpressure_policy.cpp)
  - [`tests/unit/metrics/test_metrics_exporter.cpp`](/home/xyq/mini-trantor/tests/unit/metrics/test_metrics_exporter.cpp)

2) 目标行为
- `GameBackpressureOptions::PriorityShedding` 为 output send 和 broadcast fanout 提供值语义策略配置。
- 没有显式 priority flags 的旧 frame 继续按 normal priority 处理；packet flags 的低两位只表示 Low/Normal/High/Critical。
- output soft/adaptive overload zone 只丢弃低于要求优先级的消息，并通过 `OutputDropped` + `DropLowPriority` 上报。
- broadcast fanout/payload soft/adaptive overload zone 只拒绝低于要求优先级的 fanout，并在 dispatch 前通过 `BroadcastRejected` + `DropLowPriority` 上报。
- adaptive soft threshold 在配置 hard limit 且启用 adaptive 时推导 soft zone；压力越接近 hard limit，要求的 minimum priority 可提升一级。
- hard limit 语义不变：超过 hard limit 仍拒绝/丢弃，不因高优先级绕过资源保护。
- exporter 记录 broadcast/game backpressure priority 直方图，保证丢弃决策可以按优先级回溯。

3) 线程归属
- output payload soft/adaptive 检查发生在 `LogicLoop` owner loop，保护默认输出队列进入目标 owner loop 前的资源。
- output latency soft/adaptive 检查发生在目标 connection/endpoint owner loop，保护 queue-to-send 延迟。
- broadcast route 与 soft/adaptive admission 发生在 `TcpServer` base loop；只有 admission 接受后才进入 `BroadcastDispatcher` 的 per-io-loop dispatch。

4) 所有权
- priority shedding 策略是 `GameBackpressureOptions` 的值语义配置，不拥有 loop、connection、session 或 dispatcher。
- priority 作为命令/广播请求元数据传递，不改变 payload ownership；广播 payload 仍由 shared payload/batch 持有。
- metrics callback 只观察 decision sample，不拥有或修改 reactor/game 对象。

5) 回调重入点
- output drop metric 在 logic loop 或目标 owner loop 同步触发；callback 不得再次直接调用 send。
- broadcast admission callback 在 base loop route 后同步触发；callback 不得 mutate router 或递归调用 broadcast。
- pipeline message callback 解析 frame flags 后可能提交 logic 或 broadcast，仍必须快速返回并保持 close/unbind 幂等。

6) 跨线程
- `LogicLoop::submit()` / `submitWithResult()` 可跨 I/O loop 调用；priority 随 `GameCommand` 入队，在队列锁边界内与命令一起保存。
- `TcpServer::broadcast*()` 可跨线程调用；priority 随 broadcast request marshal 到 base loop，再进入 admission 和 dispatch。
- 所有 send 仍通过 connection/endpoint owner loop，不因高优先级消息绕过 EventLoop 调度语义。

7) 测试映射
- Unit：`tests/unit/game/test_game_backpressure_policy.cpp`
- Unit：`tests/unit/metrics/test_metrics_exporter.cpp`
- Contract：`tests/contract/logic/test_logic_loop_timing_contract.cpp`
- Integration：`tests/integration/game/test_game_backpressure_policy.cpp`

Task-14 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- output priority shedding 由 `LogicLoop` owner loop 和目标 endpoint owner loop 分段执行；broadcast priority shedding 由 `TcpServer` base loop 执行。

2. Who owns it and who releases it?
- `GameBackpressureOptions` 由 pipeline/logic/server 以值语义持有；priority 只是命令/广播元数据，随对应对象释放。

3. Which callbacks may re-enter?
- backpressure metric callback、broadcast admission callback、pipeline message callback、logic output callback 可能同 tick 交错；它们必须保持观察者职责和幂等关闭语义。

4. Which operations are allowed cross-thread, and how are they marshaled?
- logic submit 和 public broadcast API 可跨线程调用；priority 随请求对象一起进入 queue/route/admission，真实发送仍 marshal 到目标 owner loop。

5. Which test file verifies this change?
- priority policy validation and mapping：`tests/unit/game/test_game_backpressure_policy.cpp`
- output low-priority soft drop：`tests/contract/logic/test_logic_loop_timing_contract.cpp`
- broadcast low-priority soft reject and high-priority pass：`tests/integration/game/test_game_backpressure_policy.cpp`
- exporter priority histograms：`tests/unit/metrics/test_metrics_exporter.cpp`

---

### Task-15（P1）Metrics exporter 标签化与文本导出适配

1) 文件级变更
- 修改
  - [`intents/modules/metrics_exporter.intent.md`](/home/xyq/mini-trantor/intents/modules/metrics_exporter.intent.md)
  - [`mini/base/MetricsExporter.h`](/home/xyq/mini-trantor/mini/base/MetricsExporter.h)
  - [`mini/base/MetricsExporter.cc`](/home/xyq/mini-trantor/mini/base/MetricsExporter.cc)
  - [`tests/unit/metrics/test_metrics_exporter.cpp`](/home/xyq/mini-trantor/tests/unit/metrics/test_metrics_exporter.cpp)
  - [`tests/contract/net/test_metrics_exporter_contract.cpp`](/home/xyq/mini-trantor/tests/contract/net/test_metrics_exporter_contract.cpp)
  - [`tests/integration/benchmark/test_game_server_metrics_smoke.cpp`](/home/xyq/mini-trantor/tests/integration/benchmark/test_game_server_metrics_smoke.cpp)

2) 目标行为
- `TaggedMetricsExporter` 作为 exporter wrapper，给所有 counter / histogram name 增加静态 deployment labels。
- label key 必须可验证、无重复；label value 需要转义 quote、backslash 和 newline，保证文本导出稳定。
- `MetricsHookRecorder` 可直接接收 tagged exporter，hook 触发线程和 recorder ownership 语义不变。
- `renderPrometheusText()` 只从 `MetricsSnapshot` 值拷贝渲染 text exposition，不引入 Prometheus 依赖、后台线程或网络端点。
- 渲染输出对 snapshot name 排序，避免 CI 断言受 unordered map 遍历顺序影响。
- histogram snapshot 以 summary count/sum 加 min/max gauge 的方式导出，保留当前聚合语义。

3) 线程归属
- exporter 不拥有 `EventLoop`；tagged wrapper 在原 hook callback 线程同步编码 metric name 并转发给 sink exporter。
- `InMemoryMetricsExporter` 仍用内部 mutex 保护跨 owner loop 聚合。
- Prometheus text rendering 应从 snapshot/reporting 线程调用，不应在 owner-loop hot callback 中执行。

4) 所有权
- 调用方拥有 sink exporter；`TaggedMetricsExporter` 只共享 sink exporter ownership 并持有静态 label 值。
- `MetricsHookRecorder` 只捕获 exporter shared ownership，不捕获 loop、connection、session、pipeline 或 server。
- `MetricsSnapshot` 是 value copy，渲染函数不观察 exporter 内部状态。

5) 回调重入点
- recorder callback、tagged wrapper 和 sink exporter 可能在多个 owner loop 同步触发；它们不得调用 reactor API 或基于指标值修改策略。
- text rendering 不是 hook callback，不应被放入连接读写热路径。

6) 跨线程
- 多个 owner loop 可并发记录到同一个 tagged exporter；同步边界仍由 sink exporter 提供。
- snapshot 读取和 text rendering 可在报告线程执行；它们不直接访问 reactor/game mutable state。

7) 测试映射
- Unit：`tests/unit/metrics/test_metrics_exporter.cpp`
- Contract：`tests/contract/net/test_metrics_exporter_contract.cpp`
- Integration：`tests/integration/benchmark/test_game_server_metrics_smoke.cpp`

Task-15 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- exporter/tagged exporter 不拥有 loop；hook callback 在原 owner loop 上执行，text rendering 从 snapshot/reporting 线程执行。

2. Who owns it and who releases it?
- 调用方通过 `shared_ptr<MetricsExporter>` 拥有 exporter；tagged exporter 共享 sink exporter 并持有静态 label 值，随 shared ownership 释放。

3. Which callbacks may re-enter?
- recorder callback 可由不同 owner loop 同步触发；tagged wrapper 只做 name 编码和转发，不调用 reactor。

4. Which operations are allowed cross-thread, and how are they marshaled?
- cross-thread recording 允许并发调用 exporter；同步由 sink exporter 内部 mutex 提供。没有 EventLoop marshal，因为 exporter 不拥有 reactor 状态。

5. Which test file verifies this change?
- label validation / text rendering：`tests/unit/metrics/test_metrics_exporter.cpp`
- concurrent tagged recording / owner-loop tagged recorder：`tests/contract/net/test_metrics_exporter_contract.cpp`
- end-to-end tagged game metrics smoke：`tests/integration/benchmark/test_game_server_metrics_smoke.cpp`

---

### Task-16（P1）KCP 固定 MTU 分片与 large payload contract

1) 文件级变更
- 修改
  - [`intents/modules/kcp_transport.intent.md`](/home/xyq/mini-trantor/intents/modules/kcp_transport.intent.md)
  - [`mini/net/kcp/KcpCodec.h`](/home/xyq/mini-trantor/mini/net/kcp/KcpCodec.h)
  - [`mini/net/kcp/KcpTransport.h`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.h)
  - [`mini/net/kcp/KcpTransport.cc`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.cc)
  - [`tests/unit/kcp/test_kcp_codec.cpp`](/home/xyq/mini-trantor/tests/unit/kcp/test_kcp_codec.cpp)
  - [`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`](/home/xyq/mini-trantor/tests/contract/kcp/test_kcp_transport_stress_contract.cpp)
  - [`tests/integration/kcp/test_kcp_reliable_flow.cpp`](/home/xyq/mini-trantor/tests/integration/kcp/test_kcp_reliable_flow.cpp)

2) 目标行为
- 修正 KCP frame payload length 边界：`payloadLen` 是 16-bit，单 frame payload 上限是 `65535`，不能允许 `64 * 1024` 溢出。
- `KcpTransport` 使用固定 1200-byte 安全 UDP payload 目标；超过单 frame 安全载荷的应用 payload 自动拆成 reliable fragments。
- 每个 fragment 仍使用独立 KCP seq，并进入同一 in-flight / ACK / retransmission 状态机。
- 接收端 fragment assembly 属于 `SessionFlowState`，只有完整应用 payload 重组后才触发一次 message callback。
- 超过 `kMaxApplicationPayloadSize` 的应用 payload 直接丢弃，不创建无限 fragment 状态。
- 该阶段不做动态 MTU 探测、拥塞窗口、FEC 或生产 KCP 参数调优。

3) 线程归属
- split/reassembly、pending packet、fragment assembly、in-flight 和 ACK 状态都属于 `KcpTransport` owner loop。
- public `send()` / `sendTo()` 仍可跨线程调用，但 payload split 必须 marshal 到 owner loop 后执行。
- UDP packet callback 进入 `onPacket()` 后在 owner loop 解码、ACK、重组和上报 message callback。

4) 所有权
- `KcpTransport` owns session map、flow state、fragment assembly 和 flush timer。
- `KcpSession` 只观察 owner transport，不拥有 fragment 或 socket 状态。
- fragment payload 按值存储在 `SessionFlowState` 中；stop/close 清理 session state 时一并释放。

5) 回调重入点
- message callback 在完整 payload 重组后触发，可能再次调用 send/close；这些入口仍回到 owner loop。
- flush tick 可能重传 fragment frame；retry budget 耗尽仍通过正常 close session 路径释放状态。

6) 跨线程
- `KcpSession::send()` 可以由任意线程调用；实际 split/in-flight mutation 通过 transport owner loop `post()` 执行。
- `stop()` off-loop 同步 marshal 并等待 owner loop 清理 session/fragment/in-flight 状态。

7) 测试映射
- Unit：`tests/unit/kcp/test_kcp_codec.cpp`
- Contract：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`
- Integration：`tests/integration/kcp/test_kcp_reliable_flow.cpp`

Task-16 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- KCP socket、session map、in-flight packet、pending packet 和 fragment assembly 全部归 `KcpTransport` owner loop。

2. Who owns it and who releases it?
- `KcpTransport` owns fragment assembly and flow state；session close/stop 通过 `removeSessionStateLocked()` 或 `sessionStates_.clear()` 释放。

3. Which callbacks may re-enter?
- complete-message callback 和 flush retry close 可能 re-enter send/close；这些入口仍 marshal 到 owner loop 并保持 session removal 幂等。

4. Which operations are allowed cross-thread, and how are they marshaled?
- `send()` / `closeSession()` / `stop()` / `openSession()` 可跨线程；状态 mutation 都在 owner loop 的 `post()` / `runInLoop()` / `queueInLoop()` 中执行。

5. Which test file verifies this change?
- payload length boundary：`tests/unit/kcp/test_kcp_codec.cpp`
- fragmented large payload under loss/reorder/duplicate/jitter：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`
- fragmented large payload roundtrip：`tests/integration/kcp/test_kcp_reliable_flow.cpp`

---

### Task-17（P1）KCP 高丢包长时与 retry/RTO 参数化

1) 文件级变更
- 修改
  - [`intents/modules/kcp_transport.intent.md`](/home/xyq/mini-trantor/intents/modules/kcp_transport.intent.md)
  - [`mini/net/kcp/KcpTransport.h`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.h)
  - [`mini/net/kcp/KcpTransport.cc`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.cc)
  - [`tests/unit/kcp/test_kcp_codec.cpp`](/home/xyq/mini-trantor/tests/unit/kcp/test_kcp_codec.cpp)
  - [`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`](/home/xyq/mini-trantor/tests/contract/kcp/test_kcp_transport_stress_contract.cpp)
  - [`docs/game_server_network_base_lifecycle_hardening.md`](/home/xyq/mini-trantor/docs/game_server_network_base_lifecycle_hardening.md)
  - [`docs/game_server_network_base_phase_closure_audit.md`](/home/xyq/mini-trantor/docs/game_server_network_base_phase_closure_audit.md)

2) 目标行为
- `KcpTransportOptions` 显式配置 initial RTO、max RTO、max retransmissions、safe datagram payload 和 max application payload。
- options 在 transport 构造期归一化；构造后作为不可变策略读取，不引入跨线程动态调参共享状态。
- flush tick 使用配置后的 retry/RTO budget；默认值保持 Task-16 前后兼容。
- 高丢包长时 contract 使用确定性 UDP proxy 注入周期性多次 data 丢包、ACK 丢失、重复和延迟，验证 tuned retry/RTO policy 下长消息流仍按序一次性交付。
- 该阶段仍不实现动态 MTU 探测、拥塞窗口或 FEC。

3) 线程归属
- socket、session map、pending packet、in-flight packet、fragment assembly 和 flush timer 都归 `KcpTransport` owner loop。
- retry/RTO options 是 transport 构造期策略；owner loop flush tick 只读取，不在 packet callback 中跨线程修改。

4) 所有权
- `KcpTransport` owns options value、session map、flow state 和 flush timer。
- `KcpSession` 只观察 transport，不持有 options 或 socket。
- session close/stop 清理 in-flight/pending/fragment state；options 随 transport 生命周期释放。

5) 回调重入点
- message callback 可 re-enter send/close；send 仍通过 owner loop post 后读取 options 并更新 in-flight。
- flush tick retry budget 耗尽可触发 close session；close 路径保持幂等。

6) 跨线程
- `send()` / `closeSession()` / `stop()` / `openSession()` 可跨线程调用；状态 mutation 都在 owner loop 执行。
- options 只在构造函数传入，不提供跨线程运行时 setter。

7) 测试映射
- Unit：`tests/unit/kcp/test_kcp_codec.cpp`
- Contract：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

Task-17 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- KCP socket、session map、in-flight packet、pending packet、fragment assembly 和 flush timer 全部归 `KcpTransport` owner loop；retry/RTO options 是 transport 构造期不可变策略。

2. Who owns it and who releases it?
- `KcpTransport` owns options value and flow state；session close/stop 释放 flow state，transport 析构释放 options。

3. Which callbacks may re-enter?
- complete-message callback 和 flush retry close 可能 re-enter send/close；这些入口仍 marshal 到 owner loop。

4. Which operations are allowed cross-thread, and how are they marshaled?
- `send()` / `closeSession()` / `stop()` / `openSession()` 可跨线程；实际 mutation 通过 `post()` / `runInLoop()` / `queueInLoop()` 回到 owner loop。

5. Which test file verifies this change?
- option normalization：`tests/unit/kcp/test_kcp_codec.cpp`
- periodic high-loss long-run stream：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

---

### Task-18（P1）KCP selective ACK 预览

1) 文件级变更
- 修改
  - [`intents/modules/kcp_transport.intent.md`](/home/xyq/mini-trantor/intents/modules/kcp_transport.intent.md)
  - [`mini/net/kcp/KcpCodec.h`](/home/xyq/mini-trantor/mini/net/kcp/KcpCodec.h)
  - [`mini/net/kcp/KcpTransport.h`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.h)
  - [`mini/net/kcp/KcpTransport.cc`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.cc)
  - [`tests/unit/kcp/test_kcp_codec.cpp`](/home/xyq/mini-trantor/tests/unit/kcp/test_kcp_codec.cpp)
  - [`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`](/home/xyq/mini-trantor/tests/contract/kcp/test_kcp_transport_stress_contract.cpp)
  - [`docs/game_server_network_base_lifecycle_hardening.md`](/home/xyq/mini-trantor/docs/game_server_network_base_lifecycle_hardening.md)
  - [`docs/game_server_network_base_phase_closure_audit.md`](/home/xyq/mini-trantor/docs/game_server_network_base_phase_closure_audit.md)

2) 目标行为
- ACK frame 支持 `kKcpFrameFlagSelectiveAck`，payload 使用 transport 私有 `SAK1` 格式承载乱序已到达 seq。
- cumulative ACK 仍通过 frame `ack` 字段表达，selective ACK 只额外确认 `pendingPackets` 中已经到达但尚未连续交付的 seq。
- 发送端收到 selective ACK 后只回收对应 in-flight packet，不改变接收端按 `nextRecvSeq` 连续交付的语义。
- malformed SACK payload 被忽略，自动退回 cumulative ACK 行为。
- 该阶段仍不实现动态 MTU 探测、拥塞窗口或 FEC。

3) 线程归属
- selective ACK payload 在 `KcpTransport` owner loop 内由 `SessionFlowState::pendingPackets` 生成。
- selective ACK 应用也在 owner loop 内修改 in-flight map。

4) 所有权
- `KcpTransport` owns pending packet、in-flight packet 和 SACK 编解码 helper。
- ACK payload 按值随 frame 发送；`KcpSession` 不拥有 ACK/SACK 状态。

5) 回调重入点
- message callback 仍只在完整顺序 payload 后触发，可能 re-enter send/close。
- ACK/SACK 处理不触发业务回调，只影响 owner-loop in-flight 回收。

6) 跨线程
- public send/close/open/stop 仍可跨线程；selective ACK 的生成和应用只发生在 owner loop packet callback 内。
- 不提供跨线程 SACK setter 或外部状态注入。

7) 测试映射
- Unit：`tests/unit/kcp/test_kcp_codec.cpp`
- Contract：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

Task-18 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- KCP socket、session map、pending packet、in-flight packet、fragment assembly、flush timer 和 selective ACK 生成/应用全部归 `KcpTransport` owner loop。

2. Who owns it and who releases it?
- `KcpTransport` owns flow state and ACK/SACK helper logic；session close/stop 清理 pending/in-flight/fragment state，ACK payload 按值随 frame 生命周期释放。

3. Which callbacks may re-enter?
- complete-message callback 和 flush retry close 可能 re-enter send/close；ACK/SACK 处理本身不触发业务回调。

4. Which operations are allowed cross-thread, and how are they marshaled?
- `send()` / `closeSession()` / `stop()` / `openSession()` 可跨线程；actual mutation 通过 `post()` / `runInLoop()` / `queueInLoop()` 回 owner loop。

5. Which test file verifies this change?
- selective ACK flag/payload preservation：`tests/unit/kcp/test_kcp_codec.cpp`
- out-of-order gap retransmission suppression：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

---

### Task-19（P1）KCP dynamic MTU probe/backoff 预览

1) 文件级变更
- 修改
  - [`intents/modules/kcp_transport.intent.md`](/home/xyq/mini-trantor/intents/modules/kcp_transport.intent.md)
  - [`mini/net/kcp/KcpCodec.h`](/home/xyq/mini-trantor/mini/net/kcp/KcpCodec.h)
  - [`mini/net/kcp/KcpTransport.h`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.h)
  - [`mini/net/kcp/KcpTransport.cc`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.cc)
  - [`tests/unit/kcp/test_kcp_codec.cpp`](/home/xyq/mini-trantor/tests/unit/kcp/test_kcp_codec.cpp)
  - [`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`](/home/xyq/mini-trantor/tests/contract/kcp/test_kcp_transport_stress_contract.cpp)
  - [`docs/game_server_network_base_lifecycle_hardening.md`](/home/xyq/mini-trantor/docs/game_server_network_base_lifecycle_hardening.md)
  - [`docs/game_server_network_base_phase_closure_audit.md`](/home/xyq/mini-trantor/docs/game_server_network_base_phase_closure_audit.md)

2) 目标行为
- KCP frame 支持 `kKcpFrameFlagMtuProbe` 控制帧，probe request 使用私有 `MTP1` payload，probe ACK 使用私有 `MTA1` payload。
- MTU probe 是 per-session preview：`SessionFlowState` 维护当前 confirmed datagram payload size、in-flight probe、probe retry count 和 probe fallback 状态。
- 开启 `enableMtuProbing` 后，session 从 `minDatagramPayloadSize` 起步；probe 成功后提升到目标 datagram size，后续 send path 使用新的 single-frame / fragment payload limit。
- probe request/ACK 不占用 data seq，不进入 in-flight data map，不触发 message callback；它只影响 owner-loop flow state。
- probe retry 用 flush tick 驱动；超过 `mtuProbeMaxRetries` 后保持最后确认的安全 datagram size，并交给后续 blackhole cooldown 策略抑制立即重复探测。
- 默认 `enableMtuProbing=false`，旧固定 safe datagram payload 行为保持兼容。
- 该阶段仍不实现完整 PMTU discovery、ICMP blackhole detection、拥塞窗口或 FEC。

3) 线程归属
- socket、session map、in-flight packet、pending packet、fragment assembly、flush timer 和 MTU probe state 全部归 `KcpTransport` owner loop。
- MTU probe 的发起、ACK 应用、重试和 fallback 都在 owner-loop packet callback 或 flush tick 内完成。

4) 所有权
- `KcpTransport` owns options value、session flow state、probe wire packet 和 probe 编解码 helper。
- `KcpSession` 只观察 transport，不拥有 MTU probe 状态。
- session close/stop 清理 flow state 时一并释放 probe state 和 cached probe packet。

5) 回调重入点
- probe control frame 处理不触发业务 callback；complete-message callback 仍只由 data path 触发，可能 re-enter send/close。
- flush tick 可能同时重传 data frame 与发送/重发 MTU probe；retry budget close 仍走正常 close session 路径。

6) 跨线程
- `send()` / `closeSession()` / `stop()` / `openSession()` 可跨线程；actual mutation 通过 `post()` / `runInLoop()` / `queueInLoop()` 回 owner loop。
- MTU probe options 只在构造函数传入并归一化，不提供跨线程运行时 setter。

7) 测试映射
- Unit：`tests/unit/kcp/test_kcp_codec.cpp`
- Contract：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

Task-19 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- KCP socket、session map、pending packet、in-flight packet、fragment assembly、flush timer 和 MTU probe/backoff state 全部归 `KcpTransport` owner loop。

2. Who owns it and who releases it?
- `KcpTransport` owns flow state and probe helper logic；session close/stop 清理 per-session probe target、retry count、cached probe wire packet 和 fallback/cooldown 状态。

3. Which callbacks may re-enter?
- complete-message callback 和 flush retry close 可能 re-enter send/close；MTU probe request/ACK 处理本身不触发业务回调。

4. Which operations are allowed cross-thread, and how are they marshaled?
- `send()` / `closeSession()` / `stop()` / `openSession()` 可跨线程；actual mutation 通过 `post()` / `runInLoop()` / `queueInLoop()` 回 owner loop。MTU probe 没有跨线程 setter。

5. Which test file verifies this change?
- MTU probe flag/payload preservation and option normalization：`tests/unit/kcp/test_kcp_codec.cpp`
- MTU probe success/failure datagram sizing contract：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

---

### Task-20（P1）KCP congestion-window 预览

1) 文件级变更
- 修改
  - [`intents/modules/kcp_transport.intent.md`](/home/xyq/mini-trantor/intents/modules/kcp_transport.intent.md)
  - [`mini/net/kcp/KcpTransport.h`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.h)
  - [`mini/net/kcp/KcpTransport.cc`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.cc)
  - [`tests/unit/kcp/test_kcp_codec.cpp`](/home/xyq/mini-trantor/tests/unit/kcp/test_kcp_codec.cpp)
  - [`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`](/home/xyq/mini-trantor/tests/contract/kcp/test_kcp_transport_stress_contract.cpp)
  - [`docs/game_server_network_base_lifecycle_hardening.md`](/home/xyq/mini-trantor/docs/game_server_network_base_lifecycle_hardening.md)
  - [`docs/game_server_network_base_phase_closure_audit.md`](/home/xyq/mini-trantor/docs/game_server_network_base_phase_closure_audit.md)

2) 目标行为
- 新增可选 `enableCongestionWindow`，默认关闭以保持旧即时发送行为。
- 开启后，reliable data frame 只有在 `inFlight.size() < congestionWindow` 时才会发送；超出窗口的 frame 留在 per-session owner-loop `sendQueue`。
- ACK 实际回收 in-flight packet 后，窗口按 preview AIMD 规则增长，并按原 seq 顺序 drain `sendQueue`。
- retransmission timeout 会把 session congestion window 回退到 configured minimum，并保留未发送队列。
- queued frame 在进入 `inFlight` 前不参与重传；进入 `inFlight` 时才设置 first-send time / RTO / retry count。
- 该阶段不实现生产级 BBR/CUBIC/KCP window tuning，也不做带宽估计或跨 session 全局公平调度。

3) 线程归属
- socket、session map、in-flight packet、sendQueue、pending packet、fragment assembly、flush timer 和 congestion-window state 全部归 `KcpTransport` owner loop。
- ACK 应用、窗口增长、timeout 回退和 queued frame drain 均在 owner-loop packet callback 或 flush tick 中完成。

4) 所有权
- `KcpTransport` owns options value、session flow state、sendQueue 和 congestion-window helper logic。
- `KcpSession` 只观察 transport，不拥有窗口、队列或 socket 状态。
- session close/stop 清理 flow state 时一并释放 queued outbound packets。

5) 回调重入点
- complete-message callback 可能 re-enter send/close；send 仍回到 owner-loop enqueue/drain 规则。
- ACK/window 处理本身不触发业务回调；flush retry close 仍走正常 close session 路径。

6) 跨线程
- `send()` / `closeSession()` / `stop()` / `openSession()` 可跨线程；actual mutation 通过 `post()` / `runInLoop()` / `queueInLoop()` 回 owner loop。
- congestion-window options 只在构造函数传入并归一化，不提供跨线程运行时 setter。

7) 测试映射
- Unit：`tests/unit/kcp/test_kcp_codec.cpp`
- Contract：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

Task-20 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- KCP socket、session map、pending packet、in-flight packet、sendQueue、fragment assembly、flush timer 和 congestion-window state 全部归 `KcpTransport` owner loop。

2. Who owns it and who releases it?
- `KcpTransport` owns flow state and queued outbound packets；session close/stop 释放 sendQueue、inFlight、pending 和 fragment state。

3. Which callbacks may re-enter?
- complete-message callback 和 flush retry close 可能 re-enter send/close；ACK/window 处理本身不触发业务回调。

4. Which operations are allowed cross-thread, and how are they marshaled?
- `send()` / `closeSession()` / `stop()` / `openSession()` 可跨线程；actual mutation 通过 `post()` / `runInLoop()` / `queueInLoop()` 回 owner loop。窗口配置没有运行时跨线程 setter。

5. Which test file verifies this change?
- congestion-window option normalization：`tests/unit/kcp/test_kcp_codec.cpp`
- initial burst cap and ACK drain contract：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

---

### Task-21（P1）KCP redundant-copy 预览

1) 文件级变更
- 修改
  - [`intents/modules/kcp_transport.intent.md`](/home/xyq/mini-trantor/intents/modules/kcp_transport.intent.md)
  - [`mini/net/kcp/KcpTransport.h`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.h)
  - [`mini/net/kcp/KcpTransport.cc`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.cc)
  - [`tests/unit/kcp/test_kcp_codec.cpp`](/home/xyq/mini-trantor/tests/unit/kcp/test_kcp_codec.cpp)
  - [`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`](/home/xyq/mini-trantor/tests/contract/kcp/test_kcp_transport_stress_contract.cpp)
  - [`docs/game_server_network_base_lifecycle_hardening.md`](/home/xyq/mini-trantor/docs/game_server_network_base_lifecycle_hardening.md)
  - [`docs/game_server_network_base_phase_closure_audit.md`](/home/xyq/mini-trantor/docs/game_server_network_base_phase_closure_audit.md)

2) 目标行为
- 新增可选 `enableRedundantCopies`，默认关闭以保持旧发送行为。
- 开启后，newly sent reliable data frame 可发送 bounded same-seq wire duplicates；副本不创建新的 data seq，不进入额外 in-flight 记录。
- queued frame 只有在 congestion-window drain 后首次进入 `inFlight` 时才发送副本；RTO retransmission 不额外放大副本。
- 接收端继续使用已有 duplicate/ACK 语义，应用层仍按序且一次性交付。
- 冗余副本全部丢失时回到现有 ACK/RTO retransmission 路径。
- 该阶段不实现 Reed-Solomon、XOR parity group、adaptive redundancy controller 或生产 FEC codec。

3) 线程归属
- redundant-copy emission 在 `KcpTransport` owner loop send/drain path 内完成。
- socket、session map、in-flight packet、sendQueue、pending packet、fragment assembly、flush timer 和 redundancy options 读取都归 owner loop。

4) 所有权
- `KcpTransport` owns options value and outbound wire duplicate policy。
- `KcpSession` 只观察 transport，不拥有 redundancy state。
- redundant wire copy 按值进入本轮 `toSend`，发送后不保存在独立生命周期中。

5) 回调重入点
- redundant-copy emission 不触发业务 callback；receiver duplicate path 仍可能发送 ACK。
- complete-message callback 仍只由 data path 的顺序推进触发，可能 re-enter send/close。

6) 跨线程
- `send()` / `closeSession()` / `stop()` / `openSession()` 可跨线程；actual mutation 通过 `post()` / `runInLoop()` / `queueInLoop()` 回 owner loop。
- redundancy options 只在构造函数传入并归一化，不提供跨线程运行时 setter。

7) 测试映射
- Unit：`tests/unit/kcp/test_kcp_codec.cpp`
- Contract：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

Task-21 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- KCP socket、session map、pending packet、in-flight packet、sendQueue、fragment assembly、flush timer 和 redundant-copy emission 全部归 `KcpTransport` owner loop。

2. Who owns it and who releases it?
- `KcpTransport` owns redundancy options and outbound emission helper；redundant wire copies 按值发送，不拥有独立跨 tick 生命周期。

3. Which callbacks may re-enter?
- complete-message callback 和 flush retry close 可能 re-enter send/close；redundant-copy emission 本身不触发业务回调。

4. Which operations are allowed cross-thread, and how are they marshaled?
- `send()` / `closeSession()` / `stop()` / `openSession()` 可跨线程；actual mutation 通过 `post()` / `runInLoop()` / `queueInLoop()` 回 owner loop。冗余配置没有运行时跨线程 setter。

5. Which test file verifies this change?
- redundant-copy option normalization：`tests/unit/kcp/test_kcp_codec.cpp`
- first data loss covered by redundant copy without RTO：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

---

### Task-22（P1）KCP PMTU blackhole cooldown 预览

1) 文件级变更
- 修改
  - [`intents/modules/kcp_transport.intent.md`](/home/xyq/mini-trantor/intents/modules/kcp_transport.intent.md)
  - [`mini/net/kcp/KcpTransport.h`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.h)
  - [`mini/net/kcp/KcpTransport.cc`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.cc)
  - [`tests/unit/kcp/test_kcp_codec.cpp`](/home/xyq/mini-trantor/tests/unit/kcp/test_kcp_codec.cpp)
  - [`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`](/home/xyq/mini-trantor/tests/contract/kcp/test_kcp_transport_stress_contract.cpp)
  - [`docs/game_server_network_base_lifecycle_hardening.md`](/home/xyq/mini-trantor/docs/game_server_network_base_lifecycle_hardening.md)
  - [`docs/game_server_network_base_phase_closure_audit.md`](/home/xyq/mini-trantor/docs/game_server_network_base_phase_closure_audit.md)

2) 目标行为
- 新增 `KcpTransportOptions::mtuProbeBlackholeCooldown`，默认 1000ms，构造期归一化。
- MTU probe retry exhaustion 后不再永久禁用该 session 探测，而是进入 per-session cooldown。
- cooldown 期间不发送新的 probe control frame，current datagram payload size 保持最后 confirmed safe size。
- cooldown 期间 data path 仍按安全尺寸分片和重传，不因探测黑洞阻塞可靠数据交付。
- cooldown 到期后允许重新探测，以覆盖路径 MTU 后续恢复或路由变化。
- 该阶段不实现完整 PMTU discovery、ICMP blackhole signal、路径级 MTU cache 或已发送 oversized data frame 的重分片恢复。

3) 线程归属
- `mtuProbeCooldownUntil`、`mtuProbeBlackholeCount`、in-flight probe 和 current datagram payload size 全部归 `KcpTransport` owner loop 的 `SessionFlowState`。
- cooldown 进入、抑制和到期后的 re-probe 均由 owner-loop flush tick 推进。

4) 所有权
- `KcpTransport` owns options value、session flow state 和 MTU probe cooldown policy。
- `KcpSession` 只观察 transport，不拥有或暴露 cooldown state。
- session close/stop 清理 flow state 时释放 cooldown/probe 状态。

5) 回调重入点
- MTU probe request/ACK 和 cooldown 状态推进不触发业务 callback。
- data message callback 仍可能 re-enter send/close；这些操作继续 marshal 到 owner loop。

6) 跨线程
- `send()` / `closeSession()` / `stop()` / `openSession()` 可跨线程；actual mutation 通过 `post()` / `runInLoop()` / `queueInLoop()` 回 owner loop。
- blackhole cooldown option 只在构造函数传入并归一化，不提供跨线程运行时 setter。

7) 测试映射
- Unit：`tests/unit/kcp/test_kcp_codec.cpp`
- Contract：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

Task-22 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- KCP socket、session map、pending packet、in-flight packet、fragment assembly、flush timer、MTU probe state 和 blackhole cooldown state 全部归 `KcpTransport` owner loop。

2. Who owns it and who releases it?
- `KcpTransport` owns cooldown options and per-session flow state；session close/stop 释放 cooldown/probe state。

3. Which callbacks may re-enter?
- complete-message callback 和 flush retry close 可能 re-enter send/close；MTU probe cooldown 处理本身不触发业务回调。

4. Which operations are allowed cross-thread, and how are they marshaled?
- `send()` / `closeSession()` / `stop()` / `openSession()` 可跨线程；actual mutation 通过 `post()` / `runInLoop()` / `queueInLoop()` 回 owner loop。cooldown 配置没有运行时跨线程 setter。

5. Which test file verifies this change?
- `mtuProbeBlackholeCooldown` option normalization：`tests/unit/kcp/test_kcp_codec.cpp`
- blackhole cooldown suppresses immediate reprobe and preserves safe data delivery：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

---

### Task-23（P1）KCP transport-local MTU path cache 预览

1) 文件级变更
- 修改
  - [`intents/modules/kcp_transport.intent.md`](/home/xyq/mini-trantor/intents/modules/kcp_transport.intent.md)
  - [`mini/net/kcp/KcpTransport.h`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.h)
  - [`mini/net/kcp/KcpTransport.cc`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.cc)
  - [`tests/unit/kcp/test_kcp_codec.cpp`](/home/xyq/mini-trantor/tests/unit/kcp/test_kcp_codec.cpp)
  - [`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`](/home/xyq/mini-trantor/tests/contract/kcp/test_kcp_transport_stress_contract.cpp)
  - [`docs/game_server_network_base_lifecycle_hardening.md`](/home/xyq/mini-trantor/docs/game_server_network_base_lifecycle_hardening.md)
  - [`docs/game_server_network_base_phase_closure_audit.md`](/home/xyq/mini-trantor/docs/game_server_network_base_phase_closure_audit.md)

2) 目标行为
- 新增可选 `KcpTransportOptions::enableMtuPathCache`，默认关闭以保持旧行为。
- 开启后，`KcpTransport` 按 peer address 缓存成功 MTU probe 的 confirmed datagram payload size。
- probe 黑洞后，path cache 同步记录 cooldown；同 peer session 关闭再打开时会 seed cooldown，避免立即重复黑洞探测。
- 同 peer session reopen 后，可直接复用 confirmed size 发送更大的单 frame，而不必重新 probe。
- path cache 是 transport-local advisory cache：不跨 `KcpTransport` 实例、不跨进程、不持久化，也不拥有 session。
- `stop()` 清理 session / flow state 时一并清理 path cache。
- 该阶段不实现 ICMP blackhole signal、全局路径 MTU 服务、跨 transport 持久 cache 或已发送 oversized data frame 的重分片恢复。

3) 线程归属
- `mtuPathCache_`、per-session MTU state、probe ACK 应用和 blackhole cooldown 均归 `KcpTransport` owner loop。
- cache seed / record 在 owner-loop packet callback、openSession/createOrGetSession 和 flush tick 路径中完成。

4) 所有权
- `KcpTransport` owns path cache entries and options value。
- `KcpSession` 不拥有或暴露 path cache；session close/stop 不需要外部协作即可释放 flow state。
- path cache entry 只保存值语义的 peer MTU/cooldown 信息，不保存 socket、session 或上层 game 对象。

5) 回调重入点
- path cache seed/record 不触发业务 callback。
- complete-message callback 和 flush retry close 仍可能 re-enter send/close；这些操作继续 marshal 到 owner loop。

6) 跨线程
- `send()` / `closeSession()` / `stop()` / `openSession()` 可跨线程；actual mutation 通过 `post()` / `runInLoop()` / `queueInLoop()` 回 owner loop。
- path cache option 只在构造函数传入并归一化/保留，不提供运行时跨线程 setter。

7) 测试映射
- Unit：`tests/unit/kcp/test_kcp_codec.cpp`
- Contract：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

Task-23 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- KCP socket、session map、MTU path cache、probe state、blackhole cooldown、flush timer 和 flow state 全部归 `KcpTransport` owner loop。

2. Who owns it and who releases it?
- `KcpTransport` owns path cache entries；`stop()` 清理 cache，session close 只清理 per-session flow state。

3. Which callbacks may re-enter?
- path cache seed/record 不触发业务回调；complete-message callback 和 flush retry close 仍可能 re-enter send/close。

4. Which operations are allowed cross-thread, and how are they marshaled?
- `send()` / `closeSession()` / `stop()` / `openSession()` 可跨线程；actual mutation 通过 `post()` / `runInLoop()` / `queueInLoop()` 回 owner loop。path cache 没有运行时跨线程 setter。

5. Which test file verifies this change?
- `enableMtuPathCache` option preservation：`tests/unit/kcp/test_kcp_codec.cpp`
- confirmed size and blackhole cooldown reused after same-peer reopen：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

---

### Task-24（P1）KCP XOR parity recovery 预览

1) 文件级变更
- 修改
  - [`intents/modules/kcp_transport.intent.md`](/home/xyq/mini-trantor/intents/modules/kcp_transport.intent.md)
  - [`mini/net/kcp/KcpCodec.h`](/home/xyq/mini-trantor/mini/net/kcp/KcpCodec.h)
  - [`mini/net/kcp/KcpTransport.h`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.h)
  - [`mini/net/kcp/KcpTransport.cc`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.cc)
  - [`tests/unit/kcp/test_kcp_codec.cpp`](/home/xyq/mini-trantor/tests/unit/kcp/test_kcp_codec.cpp)
  - [`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`](/home/xyq/mini-trantor/tests/contract/kcp/test_kcp_transport_stress_contract.cpp)
  - [`docs/game_server_network_base_lifecycle_hardening.md`](/home/xyq/mini-trantor/docs/game_server_network_base_lifecycle_hardening.md)
  - [`docs/game_server_network_base_phase_closure_audit.md`](/home/xyq/mini-trantor/docs/game_server_network_base_phase_closure_audit.md)

2) 目标行为
- 新增 `kKcpFrameFlagXorParity` parity frame flag；parity frame 不消耗 reliable data seq，不进入 `inFlight`。
- 新增可选 `enableXorParityRecovery`，默认关闭；新增 `xorParityGroupSize`，默认 4，构造期归一化并限制上限。
- 发送端只对首发进入 wire 的 reliable data frame 形成 bounded group；queued frame 等进入 `inFlight` 首发时才参与 group。
- 每组发送一个 XOR parity frame；parity payload 记录 base seq、group flags、payload lengths 和 XOR payload。
- 接收端维护 owner-loop bounded history；当 parity group 恰好缺一个 data packet 且其它 packet 已到达时恢复缺包。
- 恢复出的 packet 重新进入 `processDataPayload()`，继续使用原有按序/一次性交付、ACK/SACK 和 fragment assembly 语义。
- parity frame 丢失、每组丢多包、metadata 不匹配或 group 过大时，回退到现有 ACK/RTO 路径。
- 该阶段不实现 Reed-Solomon、多包恢复、adaptive redundancy controller、跨 session FEC 或生产级带宽自适应。

3) 线程归属
- parity send group、receiver history、encode/recovery 和 recovered packet delivery 都归 `KcpTransport` owner loop 的 `SessionFlowState`。
- parity frame 发送由 owner-loop send/drain path 触发；恢复由 owner-loop packet callback 触发。

4) 所有权
- `KcpTransport` owns parity options and per-session parity state。
- parity payload 按值进入本轮发送；receiver history 只保存 bounded value copies，不保存 session/loop/业务对象。
- `KcpSession` 不拥有或暴露 parity state。

5) 回调重入点
- parity encode/recovery 本身不触发业务 callback；只有恢复出的 data packet 连续推进 `nextRecvSeq` 时，才通过现有 message callback 触发应用层。
- complete-message callback 和 flush retry close 仍可能 re-enter send/close。

6) 跨线程
- `send()` / `closeSession()` / `stop()` / `openSession()` 可跨线程；actual mutation 通过 `post()` / `runInLoop()` / `queueInLoop()` 回 owner loop。
- parity options 只在构造函数传入并归一化，不提供运行时跨线程 setter。

7) 测试映射
- Unit：`tests/unit/kcp/test_kcp_codec.cpp`
- Contract：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

Task-24 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- KCP socket、session map、parity send group、receiver history、recovery path、flush timer 和 flow state 全部归 `KcpTransport` owner loop。

2. Who owns it and who releases it?
- `KcpTransport` owns parity options and per-session parity state；session close/stop 释放 send group/history，parity frame 按值发送后无独立生命周期。

3. Which callbacks may re-enter?
- parity encode/recovery 本身不触发业务回调；恢复出的 data packet 可能推进 message callback，该 callback 仍可能 re-enter send/close。

4. Which operations are allowed cross-thread, and how are they marshaled?
- `send()` / `closeSession()` / `stop()` / `openSession()` 可跨线程；actual mutation 通过 `post()` / `runInLoop()` / `queueInLoop()` 回 owner loop。parity 配置没有运行时跨线程 setter。

5. Which test file verifies this change?
- XOR parity flag/payload and option normalization：`tests/unit/kcp/test_kcp_codec.cpp`
- one lost data packet per parity group recovered without RTO：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

---

### Task-25（P1）KCP path MTU failure / ICMP signal 预览

1) 文件级变更
- 修改
  - [`intents/modules/kcp_transport.intent.md`](/home/xyq/mini-trantor/intents/modules/kcp_transport.intent.md)
  - [`mini/net/kcp/KcpTransport.h`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.h)
  - [`mini/net/kcp/KcpTransport.cc`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.cc)
  - [`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`](/home/xyq/mini-trantor/tests/contract/kcp/test_kcp_transport_stress_contract.cpp)
  - [`docs/game_server_network_base_lifecycle_hardening.md`](/home/xyq/mini-trantor/docs/game_server_network_base_lifecycle_hardening.md)
  - [`docs/game_server_network_base_phase_closure_audit.md`](/home/xyq/mini-trantor/docs/game_server_network_base_phase_closure_audit.md)

2) 目标行为
- 新增 `KcpTransport::notifyPathMtuFailure(peerAddr, failedDatagramPayloadSize, suggestedDatagramPayloadSize)`，作为 ICMP Packet Too Big / `EMSGSIZE` 适配层进入 KCP PMTU 状态机的显式入口。
- 该入口可跨线程调用；实际 session lookup、current datagram payload size 降级、in-flight probe 取消、cooldown 设置和 path cache 更新都回到 `KcpTransport` owner loop。
- 当 signal 命中已有 peer session 时，按 suggested size 或 conservative fallback 计算 safe datagram payload size，并把后续 application payload 按 safe size 分片。
- 如果启用 `enableMtuPathCache`，signal 会覆盖该 peer 的 transport-local cached safe size，并携带 cooldown 到 reopened same-peer session。
- signal 不占用 KCP data seq、不发送控制帧、不触发业务 message callback。
- 没有匹配 active peer session、MTU probing 未启用、failed/suggested 都为 0 时丢弃。
- 该阶段不实现 raw ICMP socket listener、平台 error queue 读取、跨 transport 持久 PMTU cache 或已编码 oversized in-flight data frame 的重分片恢复。

3) 线程归属
- path MTU failure signal 的状态应用归 `KcpTransport` owner loop。
- `currentDatagramPayloadSize`、`mtuProbeInFlight`、cooldown 和 path cache 仍属于 owner-loop `SessionFlowState` / `KcpTransport`。

4) 所有权
- `KcpTransport` owns path cache entries and per-session MTU state。
- signal payload 是调用时的值语义参数，不保存 socket、session、ICMP adapter 或上层 game 对象。
- `KcpSession` 不拥有或暴露 path MTU signal state。

5) 回调重入点
- `notifyPathMtuFailure()` 不触发业务 callback。
- 降级后的后续 data send 仍可能通过正常 message callback 交付；complete-message callback 和 flush retry close 仍可能 re-enter send/close。

6) 跨线程
- `notifyPathMtuFailure()` / `send()` / `closeSession()` / `stop()` / `openSession()` 可跨线程；actual mutation 通过 `post()` / `runInLoop()` / `queueInLoop()` 回 owner loop。
- path MTU signal 不是运行时 option setter；它只消费一次显式路径失败事件。

7) 测试映射
- Contract：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

Task-25 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- KCP socket、session map、MTU flow state、path cache、failure-signal application、flush timer 全部归 `KcpTransport` owner loop。

2. Who owns it and who releases it?
- `KcpTransport` owns per-session MTU state and transport-local cache；session close 释放 flow state，`stop()` 清理 session/cache；failure signal 本身无保存生命周期。

3. Which callbacks may re-enter?
- failure signal application 不触发业务 callback；后续按安全尺寸分片后的 data delivery 仍走现有 message callback，该 callback 可能 re-enter send/close。

4. Which operations are allowed cross-thread, and how are they marshaled?
- `notifyPathMtuFailure()`、`send()`、`closeSession()`、`stop()`、`openSession()` 可跨线程；actual mutation 经 owner loop marshal。

5. Which test file verifies this change?
- path MTU failure signal downgrades current and reopened same-peer session sizing：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

---

### Task-26（P1）KCP Linux UDP error queue PMTU signal 接入

1) 文件级变更
- 修改
  - [`intents/modules/udp.intent.md`](/home/xyq/mini-trantor/intents/modules/udp.intent.md)
  - [`intents/modules/kcp_transport.intent.md`](/home/xyq/mini-trantor/intents/modules/kcp_transport.intent.md)
  - [`mini/net/udp/UdpSocket.h`](/home/xyq/mini-trantor/mini/net/udp/UdpSocket.h)
  - [`mini/net/udp/UdpSocket.cc`](/home/xyq/mini-trantor/mini/net/udp/UdpSocket.cc)
  - [`mini/net/kcp/KcpTransport.h`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.h)
  - [`mini/net/kcp/KcpTransport.cc`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.cc)
  - [`tests/unit/udp/test_udp_socket.cpp`](/home/xyq/mini-trantor/tests/unit/udp/test_udp_socket.cpp)
  - [`tests/unit/kcp/test_kcp_codec.cpp`](/home/xyq/mini-trantor/tests/unit/kcp/test_kcp_codec.cpp)
  - [`docs/game_server_network_base_lifecycle_hardening.md`](/home/xyq/mini-trantor/docs/game_server_network_base_lifecycle_hardening.md)
  - [`docs/game_server_network_base_phase_closure_audit.md`](/home/xyq/mini-trantor/docs/game_server_network_base_phase_closure_audit.md)

2) 目标行为
- `UdpSocket` 新增 `PathMtuFailure` event 和 callback，事件只表达 peer、failed UDP payload size、suggested safe UDP payload size 与 errno，不解析 KCP/game 协议。
- Linux 下 `UdpSocket::enablePathMtuErrorQueue(true)` 打开 `IP_RECVERR` / `IPV6_RECVERR`，并在 owner-loop `EPOLLERR` 回调中用 `recvmsg(MSG_ERRQUEUE)` drain `EMSGSIZE`。
- error queue 中的 kernel path MTU 建议会被转换为 UDP payload size 后上报；local `sendTo()` 返回 `EMSGSIZE` 时同步上报 failed UDP payload size。
- `KcpTransportOptions::enablePlatformPathMtuSignals` 默认关闭；开启且 `enableMtuProbing` 为 true 时，KCP socket 会启用 UDP error queue 并把事件接入 Task-25 的 `notifyPathMtuFailure()` 状态机。
- 非 Linux build 下 error queue 开关安全返回 false；local `EMSGSIZE` callback 仍可按平台 errno 行为工作。
- 该阶段不实现 raw ICMP socket listener、跨平台 error queue adapter、用户态 ICMP packet authentication、跨 transport 持久 PMTU cache 或已编码 oversized in-flight data frame 的重分片恢复。

3) 线程归属
- UDP error queue drain 由 `UdpSocket` owner loop 的 Channel error handler 执行。
- KCP 对 platform path MTU signal 的消费仍回到 `KcpTransport` owner loop flow state。

4) 所有权
- `UdpSocket` owns fd/channel and only emits value-semantic `PathMtuFailure` samples。
- `KcpTransport` owns KCP PMTU options、per-session MTU state 和 transport-local path cache。
- error queue event 不保存 socket/session/loop/game 对象，也不延长 KCP session 生命周期。

5) 回调重入点
- `PathMtuFailureCallback` 可在 owner-loop error handler 中触发；KCP wiring 只进入 path MTU failure state machine，不触发业务 message callback。
- local `sendTo()` `EMSGSIZE` callback 与 send path 同步，调用方必须保持既有 owner-loop marshal 语义。

6) 跨线程
- `UdpSocket` 本身不新增跨线程 mutation API；`enablePathMtuErrorQueue()` 预期在 start 前或 owner loop 内调用。
- `KcpTransport::notifyPathMtuFailure()` 仍可跨线程；platform signal 经 KCP owner loop path 应用。

7) 测试映射
- Unit：`tests/unit/udp/test_udp_socket.cpp`
- Unit：`tests/unit/kcp/test_kcp_codec.cpp`

Task-26 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- UDP fd/channel/error queue drain 属于 `UdpSocket` owner loop；KCP PMTU signal consumption 属于 `KcpTransport` owner loop。

2. Who owns it and who releases it?
- `UdpSocket` owns socket resources and callback registration；`KcpTransport` owns PMTU options/state/cache；stop/remove 仍按既有 owner lifecycle 释放。

3. Which callbacks may re-enter?
- UDP `PathMtuFailureCallback` may run from owner-loop error handler or synchronous owner-loop send path; KCP wiring does not invoke user message callback.

4. Which operations are allowed cross-thread, and how are they marshaled?
- `KcpTransport::notifyPathMtuFailure()` / `send()` / `closeSession()` / `stop()` / `openSession()` 可跨线程并经 owner loop marshal；`UdpSocket::enablePathMtuErrorQueue()` 不作为跨线程 setter 使用。

5. Which test file verifies this change?
- UDP path MTU event and Linux error queue toggle：`tests/unit/udp/test_udp_socket.cpp`
- KCP platform PMTU signal option preservation：`tests/unit/kcp/test_kcp_codec.cpp`

---

### Task-27（P1）KCP IPv4 raw ICMP PMTU signal listener 预览

1) 文件级变更
- 新增
  - [`mini/net/udp/IcmpPathMtuListener.h`](/home/xyq/mini-trantor/mini/net/udp/IcmpPathMtuListener.h)
  - [`mini/net/udp/IcmpPathMtuListener.cc`](/home/xyq/mini-trantor/mini/net/udp/IcmpPathMtuListener.cc)
  - [`tests/unit/udp/test_icmp_path_mtu_listener.cpp`](/home/xyq/mini-trantor/tests/unit/udp/test_icmp_path_mtu_listener.cpp)
- 修改
  - [`intents/modules/udp.intent.md`](/home/xyq/mini-trantor/intents/modules/udp.intent.md)
  - [`intents/modules/kcp_transport.intent.md`](/home/xyq/mini-trantor/intents/modules/kcp_transport.intent.md)
  - [`mini/net/udp/UdpSocket.h`](/home/xyq/mini-trantor/mini/net/udp/UdpSocket.h)
  - [`mini/net/udp/UdpSocket.cc`](/home/xyq/mini-trantor/mini/net/udp/UdpSocket.cc)
  - [`mini/net/kcp/KcpTransport.h`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.h)
  - [`mini/net/kcp/KcpTransport.cc`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.cc)
  - [`tests/unit/udp/test_udp_socket.cpp`](/home/xyq/mini-trantor/tests/unit/udp/test_udp_socket.cpp)
  - [`tests/unit/kcp/test_kcp_codec.cpp`](/home/xyq/mini-trantor/tests/unit/kcp/test_kcp_codec.cpp)
  - [`docs/game_server_network_base_lifecycle_hardening.md`](/home/xyq/mini-trantor/docs/game_server_network_base_lifecycle_hardening.md)
  - [`docs/game_server_network_base_phase_closure_audit.md`](/home/xyq/mini-trantor/docs/game_server_network_base_phase_closure_audit.md)

2) 目标行为
- `IcmpPathMtuListener` 在 Linux 下 best-effort 打开 IPv4 raw ICMP socket，监听 ICMP destination unreachable / fragmentation needed。
- parser 只消费 ICMP quoted IPv4 + UDP header，按本地 UDP source port 过滤，再生成 value-semantic `udp::PathMtuFailure`。
- `UdpSocket::enableRawIcmpPathMtuListener(true)` 由 owner loop 调用；raw socket / Channel 由 `UdpSocket` unique ownership 管理，stop 时先停 listener 再移除 UDP Channel。
- `KcpTransportOptions::enableRawIcmpPathMtuSignals` 默认关闭；开启且 `enableMtuProbing` 为 true 时，在 `KcpTransport::start()` owner-loop 分支启用 UDP-owned listener，并把事件接入 Task-25 的 PMTU failure state machine。
- 缺少 `CAP_NET_RAW`、平台不支持或 socket 创建失败时返回 false，不改变 UDP error queue、显式 `notifyPathMtuFailure()` 或 KCP probe/backoff 行为。
- 该阶段不实现 IPv6 raw ICMP、cross-platform raw ICMP adapter、cryptographic PMTU signal authentication、跨 transport 持久 PMTU cache 或 oversized in-flight data frame 重分片恢复。

3) 线程归属
- raw ICMP socket、Channel 和 read/filter callback 属于 `UdpSocket` owner loop。
- KCP 对 raw ICMP PMTU signal 的消费仍通过 `notifyPathMtuFailure()` 回到 `KcpTransport` owner loop flow state。

4) 所有权
- `UdpSocket` owns optional `IcmpPathMtuListener` through `unique_ptr`。
- `IcmpPathMtuListener` owns raw ICMP socket and Channel, but does not own EventLoop。
- `KcpTransport` owns PMTU options、per-session MTU state 和 transport-local path cache；raw ICMP event 不保存 socket/session/game 对象。

5) 回调重入点
- raw ICMP read callback 可在 UDP owner loop 上触发 `PathMtuFailureCallback`；KCP wiring 只进入 PMTU state machine，不触发业务 message callback。
- 降级后的后续 data delivery 仍可能触发 message callback，该 callback 仍按既有规则可 re-enter send/close。

6) 跨线程
- `UdpSocket::enableRawIcmpPathMtuListener()` 不作为跨线程 setter 使用，必须由 owner loop 调用。
- `KcpTransport::start()` 可跨线程调用，但实际 raw ICMP listener enable 在 owner-loop start 分支执行。
- `KcpTransport::notifyPathMtuFailure()` / `send()` / `closeSession()` / `stop()` / `openSession()` 仍可跨线程并经 owner loop marshal。

7) 测试映射
- Unit：`tests/unit/udp/test_icmp_path_mtu_listener.cpp`
- Unit：`tests/unit/udp/test_udp_socket.cpp`
- Unit：`tests/unit/kcp/test_kcp_codec.cpp`

Task-27 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- UDP raw ICMP socket/Channel/read-filter 属于 `UdpSocket` owner loop；KCP PMTU signal consumption 属于 `KcpTransport` owner loop。

2. Who owns it and who releases it?
- `UdpSocket` owns optional `IcmpPathMtuListener`；listener owns raw socket/Channel；`UdpSocket::stop()` 和析构 owner-loop stop path 释放 listener；KCP 只拥有 options/state/cache，不拥有 raw socket。

3. Which callbacks may re-enter?
- UDP `PathMtuFailureCallback` may run from raw ICMP owner-loop read handler; KCP wiring does not invoke user message callback and only mutates PMTU state.

4. Which operations are allowed cross-thread, and how are they marshaled?
- `KcpTransport::start()` / `notifyPathMtuFailure()` / `send()` / `closeSession()` / `stop()` / `openSession()` 可跨线程并经 owner loop marshal；`UdpSocket::enableRawIcmpPathMtuListener()` 不作为跨线程 setter 使用。

5. Which test file verifies this change?
- IPv4 ICMP Packet Too Big parser：`tests/unit/udp/test_icmp_path_mtu_listener.cpp`
- UDP path MTU event type migration：`tests/unit/udp/test_udp_socket.cpp`
- KCP raw ICMP PMTU signal option preservation：`tests/unit/kcp/test_kcp_codec.cpp`

---

### Task-28（P1）KCP ICMPv6 raw PMTU signal listener 预览

1) 文件级变更
- 修改
  - [`intents/modules/udp.intent.md`](/home/xyq/mini-trantor/intents/modules/udp.intent.md)
  - [`intents/modules/kcp_transport.intent.md`](/home/xyq/mini-trantor/intents/modules/kcp_transport.intent.md)
  - [`mini/net/udp/IcmpPathMtuListener.h`](/home/xyq/mini-trantor/mini/net/udp/IcmpPathMtuListener.h)
  - [`mini/net/udp/IcmpPathMtuListener.cc`](/home/xyq/mini-trantor/mini/net/udp/IcmpPathMtuListener.cc)
  - [`mini/net/udp/UdpSocket.cc`](/home/xyq/mini-trantor/mini/net/udp/UdpSocket.cc)
  - [`tests/unit/udp/test_icmp_path_mtu_listener.cpp`](/home/xyq/mini-trantor/tests/unit/udp/test_icmp_path_mtu_listener.cpp)
  - [`docs/game_server_network_base_lifecycle_hardening.md`](/home/xyq/mini-trantor/docs/game_server_network_base_lifecycle_hardening.md)
  - [`docs/game_server_network_base_phase_closure_audit.md`](/home/xyq/mini-trantor/docs/game_server_network_base_phase_closure_audit.md)

2) 目标行为
- `IcmpPathMtuListener` 根据 UDP socket family 选择 IPv4 raw ICMP 或 IPv6 raw ICMPv6 socket。
- ICMPv6 parser 解析 Packet Too Big（type=2/code=0）并读取 32-bit path MTU，按 quoted IPv6/UDP header 的 UDP source port 过滤。
- ICMPv6 parser 接受 raw socket 常见的 ICMPv6 payload 起始形态，也兼容带 outer IPv6 header 的 adapter/test input。
- `PathMtuFailure` 仍只携带 peer、failed UDP payload size、suggested safe UDP payload size 和 errno，不保存 socket/session/game 对象。
- `UdpSocket::enableRawIcmpPathMtuListener(true)` 对 AF_INET / AF_INET6 都可尝试启用；缺少 `CAP_NET_RAW` 或平台不支持时继续非致命失败。
- 该阶段不实现跨平台 raw ICMP adapter、cryptographic PMTU signal authentication、跨 transport 持久 PMTU cache 或 IPv6 extension-header 完整解析。

3) 线程归属
- raw ICMPv6 socket、Channel 和 read/filter callback 属于 `UdpSocket` owner loop。
- KCP 对 ICMPv6 PMTU signal 的消费仍通过既有 path MTU failure state machine 回到 `KcpTransport` owner loop flow state。

4) 所有权
- `UdpSocket` owns optional `IcmpPathMtuListener` through `unique_ptr`。
- `IcmpPathMtuListener` owns IPv4/IPv6 raw socket and Channel, but does not own EventLoop。
- `KcpTransport` owns PMTU options/state/cache；ICMPv6 event 不延长 session 或 socket 生命周期。

5) 回调重入点
- raw ICMPv6 read callback 可在 UDP owner loop 上触发 `PathMtuFailureCallback`；KCP wiring 不触发业务 message callback。
- 降级后的后续 data delivery 仍可能触发 message callback，该 callback 仍按既有规则可 re-enter send/close。

6) 跨线程
- `UdpSocket::enableRawIcmpPathMtuListener()` 不作为跨线程 setter 使用，必须由 owner loop 调用。
- `KcpTransport::start()` 可跨线程调用，但实际 raw ICMPv6 listener enable 在 owner-loop start 分支执行。
- `KcpTransport::notifyPathMtuFailure()` / `send()` / `closeSession()` / `stop()` / `openSession()` 仍可跨线程并经 owner loop marshal。

7) 测试映射
- Unit：`tests/unit/udp/test_icmp_path_mtu_listener.cpp`
- Unit：`tests/unit/udp/test_udp_socket.cpp`

Task-28 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- UDP raw ICMPv6 socket/Channel/read-filter 属于 `UdpSocket` owner loop；KCP PMTU signal consumption 属于 `KcpTransport` owner loop。

2. Who owns it and who releases it?
- `UdpSocket` owns optional `IcmpPathMtuListener`；listener owns raw socket/Channel；`UdpSocket::stop()` 和析构 owner-loop stop path 释放 listener；KCP 只拥有 options/state/cache，不拥有 raw socket。

3. Which callbacks may re-enter?
- UDP `PathMtuFailureCallback` may run from raw ICMPv6 owner-loop read handler; KCP wiring does not invoke user message callback and only mutates PMTU state.

4. Which operations are allowed cross-thread, and how are they marshaled?
- `KcpTransport::start()` / `notifyPathMtuFailure()` / `send()` / `closeSession()` / `stop()` / `openSession()` 可跨线程并经 owner loop marshal；`UdpSocket::enableRawIcmpPathMtuListener()` 不作为跨线程 setter 使用。

5. Which test file verifies this change?
- ICMPv6 Packet Too Big parser：`tests/unit/udp/test_icmp_path_mtu_listener.cpp`
- UDP raw ICMPv6 best-effort enable/disable state contract：`tests/unit/udp/test_udp_socket.cpp`

---

### Task-29（P1）KCP/UDP cross-platform PMTU signal adapter 预览

1) 文件级变更
- 新增
  - [`mini/net/udp/PathMtuSignal.h`](/home/xyq/mini-trantor/mini/net/udp/PathMtuSignal.h)
  - [`mini/net/udp/PathMtuSignalAdapter.h`](/home/xyq/mini-trantor/mini/net/udp/PathMtuSignalAdapter.h)
  - [`mini/net/udp/PathMtuSignalAdapter.cc`](/home/xyq/mini-trantor/mini/net/udp/PathMtuSignalAdapter.cc)
  - [`tests/unit/udp/test_path_mtu_signal_adapter.cpp`](/home/xyq/mini-trantor/tests/unit/udp/test_path_mtu_signal_adapter.cpp)
- 修改
  - [`intents/modules/udp.intent.md`](/home/xyq/mini-trantor/intents/modules/udp.intent.md)
  - [`intents/modules/kcp_transport.intent.md`](/home/xyq/mini-trantor/intents/modules/kcp_transport.intent.md)
  - [`mini/net/udp/IcmpPathMtuListener.h`](/home/xyq/mini-trantor/mini/net/udp/IcmpPathMtuListener.h)
  - [`mini/net/udp/UdpSocket.h`](/home/xyq/mini-trantor/mini/net/udp/UdpSocket.h)
  - [`mini/net/udp/UdpSocket.cc`](/home/xyq/mini-trantor/mini/net/udp/UdpSocket.cc)
  - [`docs/game_server_network_base_lifecycle_hardening.md`](/home/xyq/mini-trantor/docs/game_server_network_base_lifecycle_hardening.md)
  - [`docs/game_server_network_base_phase_closure_audit.md`](/home/xyq/mini-trantor/docs/game_server_network_base_phase_closure_audit.md)

2) 目标行为
- `PathMtuFailure` 从 raw ICMP listener header 中抽到独立共享值类型，避免 error queue adapter 依赖 raw ICMP listener。
- `PathMtuSignalAdapter` 封装 platform UDP PMTU signal plumbing：Linux 使用 `IP_RECVERR` / `IPV6_RECVERR` + `MSG_ERRQUEUE`，非 Linux 使用 no-op stub。
- `UdpSocket` 仍拥有 fd/channel 和 owner-loop 调度；adapter 不拥有 fd、Channel、EventLoop、KCP session 或 game 对象。
- `UdpSocket::enablePathMtuErrorQueue()` 只委托 adapter 配置平台信号源，`handlePathMtuErrorQueue()` 只在 owner-loop error handler 内委托 adapter drain 并转发 value sample。
- local `sendTo()` 的同步 `EMSGSIZE` 仍保留在 `UdpSocket` send path，不被 adapter 隐式拥有。
- 该阶段不实现 BSD/Windows PMTU source、cryptographic PMTU signal authentication 或跨 transport 持久 PMTU cache。

3) 线程归属
- adapter 没有线程归属状态；它只在调用方线程同步执行。
- `UdpSocket` 只能在 owner loop error handler 中调用 `drainUdpErrorQueue()`，因此 callback thread context 仍是 UDP owner loop。
- KCP 对 adapter sample 的消费仍通过既有 path MTU failure state machine 回到 `KcpTransport` owner loop flow state。

4) 所有权
- `UdpSocket` owns fd/channel and callbacks；`PathMtuSignalAdapter` owns no reactor object。
- `PathMtuFailure` 是值语义 event，producer/consumer 不通过它传递 ownership。
- `KcpTransport` owns PMTU options/state/cache；adapter event 不延长 session 或 socket 生命周期。

5) 回调重入点
- adapter drain 可同步触发 `PathMtuFailureCallback`；KCP wiring 只进入 PMTU state machine，不触发业务 message callback。
- adapter error callback 可同步触发 UDP error callback；回调必须保持 owner-loop 轻量语义。

6) 跨线程
- `PathMtuSignalAdapter` 不提供跨线程 mutation API；由 `UdpSocket` 负责在 owner loop 调用配置/drain。
- `KcpTransport::start()` / `notifyPathMtuFailure()` / `send()` / `closeSession()` / `stop()` / `openSession()` 仍可跨线程并经 owner loop marshal。

7) 测试映射
- Unit：`tests/unit/udp/test_path_mtu_signal_adapter.cpp`
- Unit：`tests/unit/udp/test_udp_socket.cpp`
- Unit：`tests/unit/kcp/test_kcp_codec.cpp`

Task-29 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- `PathMtuSignalAdapter` 无内部 owner loop；`UdpSocket` owner loop 负责调用 configure/drain，KCP PMTU signal consumption 属于 `KcpTransport` owner loop。

2. Who owns it and who releases it?
- adapter owns no fd/channel/session；`UdpSocket` owns fd/channel and callback wiring；`PathMtuFailure` 是值语义 sample。

3. Which callbacks may re-enter?
- adapter drain may synchronously invoke UDP `PathMtuFailureCallback` or error callback from owner-loop error handling; KCP wiring does not invoke user message callback.

4. Which operations are allowed cross-thread, and how are they marshaled?
- adapter itself has no cross-thread public mutation; all public KCP operations still marshal through owner loop; `UdpSocket::enablePathMtuErrorQueue()` remains owner-loop/start-time scoped.

5. Which test file verifies this change?
- adapter payload-size conversion and Linux/no-op configure contract：`tests/unit/udp/test_path_mtu_signal_adapter.cpp`
- UdpSocket PMTU option and event behavior：`tests/unit/udp/test_udp_socket.cpp`

---

### Task-30（P1）KCP cross-transport shared PathMtuCache 预览

1) 文件级变更
- 新增
  - [`mini/net/transport/PathMtuCache.h`](/home/xyq/mini-trantor/mini/net/transport/PathMtuCache.h)
  - [`mini/net/transport/PathMtuCache.cc`](/home/xyq/mini-trantor/mini/net/transport/PathMtuCache.cc)
  - [`intents/modules/path_mtu_cache.intent.md`](/home/xyq/mini-trantor/intents/modules/path_mtu_cache.intent.md)
  - [`tests/unit/transport/test_path_mtu_cache.cpp`](/home/xyq/mini-trantor/tests/unit/transport/test_path_mtu_cache.cpp)
- 修改
  - [`intents/modules/kcp_transport.intent.md`](/home/xyq/mini-trantor/intents/modules/kcp_transport.intent.md)
  - [`mini/net/kcp/KcpTransport.h`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.h)
  - [`mini/net/kcp/KcpTransport.cc`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.cc)
  - [`tests/unit/kcp/test_kcp_codec.cpp`](/home/xyq/mini-trantor/tests/unit/kcp/test_kcp_codec.cpp)
  - [`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`](/home/xyq/mini-trantor/tests/contract/kcp/test_kcp_transport_stress_contract.cpp)
  - [`docs/game_server_network_base_lifecycle_hardening.md`](/home/xyq/mini-trantor/docs/game_server_network_base_lifecycle_hardening.md)
  - [`docs/game_server_network_base_phase_closure_audit.md`](/home/xyq/mini-trantor/docs/game_server_network_base_phase_closure_audit.md)

2) 目标行为
- 新增 `transport::PathMtuCache`，用于保存 peer-keyed confirmed datagram payload size、blackhole cooldown 和 blackhole count。
- 新增 `KcpTransportOptions::sharedMtuPathCache`，只有同时开启 `enableMtuPathCache` 时才参与 KCP MTU state seed/record。
- 未注入 shared cache 时，原 transport-local `mtuPathCache_` 行为保持兼容：同 transport 内 reopen 可复用，`stop()` 清理。
- 注入 shared cache 时，KCP 写入外部 shared cache；单个 `KcpTransport::stop()` 只清理 session/flow/local cache，不清理 shared cache。
- shared cache 是进程内、显式 ownership、mutex-protected value store；它不拥有 socket、EventLoop、session、KCP flow state 或 game 对象。
- 该阶段不实现磁盘持久化、分布式 PMTU 服务、自动全局 singleton、eviction policy 或 path key 自定义策略。

3) 线程归属
- `PathMtuCache` 没有 EventLoop 归属；内部 mutex 保护跨 owner loop 调用。
- `KcpTransport` 仍只在 owner loop 上把 cache entry 应用到 `SessionFlowState`。

4) 所有权
- application / transport factory 通过 `std::shared_ptr<PathMtuCache>` 持有 shared cache。
- `KcpTransport` 只观察 shared cache；不在 stop/destructor 中释放或清空外部 cache。
- local path cache 仍由单个 `KcpTransport` owner loop 持有。

5) 回调重入点
- `PathMtuCache` 不触发 callback。
- KCP message callback 仍只由 data delivery 触发；cache seed/record 不触发业务回调。

6) 跨线程
- `PathMtuCache` public API 可跨线程调用，由内部 mutex 同步。
- `KcpTransport::openSession()` / `notifyPathMtuFailure()` / `send()` / `closeSession()` / `stop()` 仍按 owner loop marshal；shared cache 只是 advisory state source/sink。

7) 测试映射
- Unit：`tests/unit/transport/test_path_mtu_cache.cpp`
- Unit：`tests/unit/kcp/test_kcp_codec.cpp`
- Contract：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

Task-30 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- `PathMtuCache` 无 EventLoop 归属并由内部 mutex 保护；KCP 对 cache entry 的 flow-state 应用属于 `KcpTransport` owner loop。

2. Who owns it and who releases it?
- shared cache 由 application/factory 通过 `shared_ptr` 拥有；`KcpTransport` 只观察并读写值语义 entry；local cache 仍由 transport 自己拥有并在 stop 清理。

3. Which callbacks may re-enter?
- cache API 不触发 callback；KCP data message callback 仍可 re-enter send/close，但与 cache seed/record 解耦。

4. Which operations are allowed cross-thread, and how are they marshaled?
- cache API 本身可跨线程同步调用；KCP public operations 仍经 owner loop marshal，cache 不改变 reactor 调度语义。

5. Which test file verifies this change?
- PathMtuCache value semantics and concurrency：`tests/unit/transport/test_path_mtu_cache.cpp`
- KCP option preservation：`tests/unit/kcp/test_kcp_codec.cpp`
- Cross-transport restart confirmed-size/cooldown reuse contract：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

---

### Task-31（P1）KCP userspace raw ICMP PMTU signal authentication 预览

1) 文件级变更
- 新增
  - [`intents/modules/path_mtu_signal_authentication.intent.md`](/home/xyq/mini-trantor/intents/modules/path_mtu_signal_authentication.intent.md)
- 修改
  - [`mini/net/udp/PathMtuSignal.h`](/home/xyq/mini-trantor/mini/net/udp/PathMtuSignal.h)
  - [`mini/net/udp/IcmpPathMtuListener.cc`](/home/xyq/mini-trantor/mini/net/udp/IcmpPathMtuListener.cc)
  - [`mini/net/udp/PathMtuSignalAdapter.cc`](/home/xyq/mini-trantor/mini/net/udp/PathMtuSignalAdapter.cc)
  - [`mini/net/udp/UdpSocket.h`](/home/xyq/mini-trantor/mini/net/udp/UdpSocket.h)
  - [`mini/net/udp/UdpSocket.cc`](/home/xyq/mini-trantor/mini/net/udp/UdpSocket.cc)
  - [`mini/net/kcp/KcpTransport.h`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.h)
  - [`mini/net/kcp/KcpTransport.cc`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.cc)
  - [`intents/modules/udp.intent.md`](/home/xyq/mini-trantor/intents/modules/udp.intent.md)
  - [`intents/modules/kcp_transport.intent.md`](/home/xyq/mini-trantor/intents/modules/kcp_transport.intent.md)
  - [`tests/unit/udp/test_icmp_path_mtu_listener.cpp`](/home/xyq/mini-trantor/tests/unit/udp/test_icmp_path_mtu_listener.cpp)
  - [`tests/unit/udp/test_udp_socket.cpp`](/home/xyq/mini-trantor/tests/unit/udp/test_udp_socket.cpp)
  - [`tests/unit/kcp/test_kcp_codec.cpp`](/home/xyq/mini-trantor/tests/unit/kcp/test_kcp_codec.cpp)
  - [`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`](/home/xyq/mini-trantor/tests/contract/kcp/test_kcp_transport_stress_contract.cpp)
  - [`docs/game_server_network_base_lifecycle_hardening.md`](/home/xyq/mini-trantor/docs/game_server_network_base_lifecycle_hardening.md)
  - [`docs/game_server_network_base_phase_closure_audit.md`](/home/xyq/mini-trantor/docs/game_server_network_base_phase_closure_audit.md)

2) 目标行为
- `udp::PathMtuFailure` 保留 `PathMtuSignalSource` 和 bounded quoted UDP payload prefix。
- raw ICMP/ICMPv6 parser 从 quoted UDP header 后复制最多 `kMaxPathMtuQuotedUdpPayloadPrefix` 字节，作为上层认证证据。
- local `EMSGSIZE` 和 Linux error queue PMTU signal 也保留原始 UDP payload prefix；UDP 不解析 KCP、不拥有 session policy。
- 新增 `KcpTransportOptions::enablePathMtuSignalAuthentication`，默认关闭以保持兼容。
- 开启后，KCP 仅对 `PathMtuSignalSource::kRawIcmp` 信号要求 quoted prefix 包含有效 KCP magic/version 和匹配 peer active session id；不匹配则忽略，不改变 MTU/probe/cache state。
- 显式三参数 `notifyPathMtuFailure(peer, failed, suggested)` 仍作为可信人工/测试信号，不要求 quoted packet evidence。
- 该阶段不实现 HMAC/token、router trust、anti-replay、cryptographic PMTU signal authentication、BSD/Windows PMTU source 或跨进程 PMTU 服务。

3) 线程归属
- UDP raw ICMP listener 和 error queue drain 仍在 `UdpSocket` owner loop。
- KCP 认证检查在 `KcpTransport::post()` marshal 后进入 owner loop，并在 peer address 映射到 active session id 后执行。

4) 所有权
- `PathMtuFailure` 以值语义拥有 quoted prefix 字节。
- `UdpSocket` 不保存 packet buffer 引用，不持有 KCP/session/game 对象。
- `KcpTransport` 持有认证 option 和 helper；认证 helper 不保存外部对象。

5) 回调重入点
- UDP path MTU callback 可进入 KCP public overload，但实际认证和状态变更收口到 owner loop。
- 认证 helper 本身不触发 message callback；只有通过认证后的 PMTU failure 才进入既有 safe-size downgrade 状态机。

6) 跨线程
- `notifyPathMtuFailure(const udp::PathMtuFailure&)` 可跨线程调用，并通过 `post()` marshal。
- UDP raw ICMP enable/disable 仍不是跨线程 setter；start 时由 KCP owner loop 启用。

7) 测试映射
- Unit：`tests/unit/udp/test_icmp_path_mtu_listener.cpp`
- Unit：`tests/unit/udp/test_udp_socket.cpp`
- Unit：`tests/unit/kcp/test_kcp_codec.cpp`
- Contract：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

Task-31 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- UDP signal extraction belongs to `UdpSocket` owner loop; KCP session authentication and PMTU state mutation belong to `KcpTransport` owner loop.

2. Who owns it and who releases it?
- `PathMtuFailure` owns quoted prefix bytes by value; UDP owns raw socket/listener resources; KCP owns session maps, flow state, options, and authentication helpers.

3. Which callbacks may re-enter?
- UDP PMTU callback may call into KCP, and KCP message callback may still re-enter send/close; the authentication helper itself does not invoke callbacks.

4. Which operations are allowed cross-thread, and how are they marshaled?
- `KcpTransport::notifyPathMtuFailure(const udp::PathMtuFailure&)` may be called cross-thread and is marshaled through `post()`; raw ICMP setup remains owner-loop scoped.

5. Which test file verifies this change?
- Raw ICMP quoted prefix extraction：`tests/unit/udp/test_icmp_path_mtu_listener.cpp`
- Local `EMSGSIZE` source/prefix preservation：`tests/unit/udp/test_udp_socket.cpp`
- KCP option preservation：`tests/unit/kcp/test_kcp_codec.cpp`
- Wrong-session reject and matching-session accept contract：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

---

### Task-32（P1）UDP platform PMTU source capability/query 预览

1) 文件级变更
- 新增
  - [`intents/modules/platform_path_mtu_signal.intent.md`](/home/xyq/mini-trantor/intents/modules/platform_path_mtu_signal.intent.md)
- 修改
  - [`mini/net/udp/PathMtuSignalAdapter.h`](/home/xyq/mini-trantor/mini/net/udp/PathMtuSignalAdapter.h)
  - [`mini/net/udp/PathMtuSignalAdapter.cc`](/home/xyq/mini-trantor/mini/net/udp/PathMtuSignalAdapter.cc)
  - [`mini/net/udp/UdpSocket.h`](/home/xyq/mini-trantor/mini/net/udp/UdpSocket.h)
  - [`mini/net/udp/UdpSocket.cc`](/home/xyq/mini-trantor/mini/net/udp/UdpSocket.cc)
  - [`mini/net/kcp/KcpTransport.cc`](/home/xyq/mini-trantor/mini/net/kcp/KcpTransport.cc)
  - [`intents/modules/udp.intent.md`](/home/xyq/mini-trantor/intents/modules/udp.intent.md)
  - [`intents/modules/kcp_transport.intent.md`](/home/xyq/mini-trantor/intents/modules/kcp_transport.intent.md)
  - [`tests/unit/udp/test_path_mtu_signal_adapter.cpp`](/home/xyq/mini-trantor/tests/unit/udp/test_path_mtu_signal_adapter.cpp)
  - [`tests/unit/udp/test_udp_socket.cpp`](/home/xyq/mini-trantor/tests/unit/udp/test_udp_socket.cpp)
  - [`docs/game_server_network_base_lifecycle_hardening.md`](/home/xyq/mini-trantor/docs/game_server_network_base_lifecycle_hardening.md)
  - [`docs/game_server_network_base_phase_closure_audit.md`](/home/xyq/mini-trantor/docs/game_server_network_base_phase_closure_audit.md)

2) 目标行为
- `PathMtuSignalAdapter::platformCapabilities()` 返回 IPv4/IPv6 的 async configure、async drain、connected MTU query 支持情况。
- Linux 继续使用真实 `IP_RECVERR` / `IPV6_RECVERR` + `MSG_ERRQUEUE` 路径，并通过 generic `configurePlatformPathMtuSignals()` / `drainPlatformPathMtuSignals()` 暴露给 `UdpSocket`。
- 新增 `queryConnectedUdpPayloadMtu()`，在 OS 暴露 `IP_MTU` / `IPV6_MTU` 风格 socket option 时尝试读取 connected UDP path MTU，并转换成 UDP payload size；未连接或 unsupported 时返回 `std::nullopt`。
- `UdpSocket` 新增 `enablePlatformPathMtuSignals()` / `platformPathMtuSignalsEnabled()`；旧 `enablePathMtuErrorQueue()` / `pathMtuErrorQueueEnabled()` 保留为兼容 alias。
- `KcpTransportOptions::enablePlatformPathMtuSignals` 改走 generic socket API。
- 该阶段不声称 BSD/Windows runtime source 已在 CI 验证；它先把 capability/query 边界固定下来。

3) 线程归属
- capability 与 payload-size conversion 是无 owner loop 的纯查询。
- configure/drain/query 都观察 fd，必须由 socket owner path 调用。
- KCP 仍只在 owner loop 消费 `PathMtuFailure` value sample。

4) 所有权
- `PathMtuSignalAdapter` 不拥有 fd、Channel、EventLoop、session 或 KCP flow state。
- `UdpSocket` 继续拥有 fd 与 callback wiring。
- adapter 不保存 query/drain 期间的 buffer 或 socket 引用。

5) 回调重入点
- adapter drain 仍可能同步触发 UDP path MTU failure callback。
- capability/query/configure 不触发 callback。

6) 跨线程
- adapter 本身不提供跨线程 mutation API；调用者必须按 fd owner 规则调用。
- KCP public operations 仍经 owner loop marshal；platform source capability 不改变调度语义。

7) 测试映射
- Unit：`tests/unit/udp/test_path_mtu_signal_adapter.cpp`
- Unit：`tests/unit/udp/test_udp_socket.cpp`
- Unit：`tests/unit/kcp/test_kcp_codec.cpp`
- Contract：`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

Task-32 变更门控（提交/PR 必答）

1. Which loop/thread owns this module?
- `PathMtuSignalAdapter` 没有 owner loop；`UdpSocket` owner loop/owner path 负责 configure/drain/query；KCP PMTU consumption 仍属于 `KcpTransport` owner loop。

2. Who owns it and who releases it?
- adapter owns no resources；`UdpSocket` owns fd/channel；`KcpTransport` owns PMTU options/state/cache。

3. Which callbacks may re-enter?
- only adapter drain may synchronously emit `PathMtuFailureCallback`; capability, configure, and query helpers do not invoke callbacks.

4. Which operations are allowed cross-thread, and how are they marshaled?
- adapter helpers do not marshal; callers must respect fd owner rules. KCP public APIs continue to marshal through `post()` / owner loop paths.

5. Which test file verifies this change?
- Capability/generic configure/query contract：`tests/unit/udp/test_path_mtu_signal_adapter.cpp`
- Socket generic platform API and old alias compatibility：`tests/unit/udp/test_udp_socket.cpp`
- KCP platform option remains preserved and stress path still passes：`tests/unit/kcp/test_kcp_codec.cpp`、`tests/contract/kcp/test_kcp_transport_stress_contract.cpp`

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

### 先决顺序（M1-M32）
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
13. **M13**：Task-13（游戏网关安全骨架）
14. **M14**：Task-14（游戏层背压优先级与自适应 soft threshold）
15. **M15**：Task-15（Metrics exporter 标签化与文本导出适配）
16. **M16**：Task-16（KCP 固定 MTU 分片与 large payload contract）
17. **M17**：Task-17（KCP 高丢包长时与 retry/RTO 参数化）
18. **M18**：Task-18（KCP selective ACK 预览）
19. **M19**：Task-19（KCP dynamic MTU probe/backoff 预览）
20. **M20**：Task-20（KCP congestion-window 预览）
21. **M21**：Task-21（KCP redundant-copy 预览）
22. **M22**：Task-22（KCP PMTU blackhole cooldown 预览）
23. **M23**：Task-23（KCP transport-local MTU path cache 预览）
24. **M24**：Task-24（KCP XOR parity recovery 预览）
25. **M25**：Task-25（KCP path MTU failure / ICMP signal 预览）
26. **M26**：Task-26（KCP Linux UDP error queue PMTU signal 接入）
27. **M27**：Task-27（KCP IPv4 raw ICMP PMTU signal listener 预览）
28. **M28**：Task-28（KCP ICMPv6 raw PMTU signal listener 预览）
29. **M29**：Task-29（KCP/UDP cross-platform PMTU signal adapter 预览）
30. **M30**：Task-30（KCP cross-transport shared PathMtuCache 预览）
31. **M31**：Task-31（KCP userspace raw ICMP PMTU signal authentication 预览）
32. **M32**：Task-32（UDP platform PMTU source capability/query 预览）
33. **M33**：Scope Boundary Hardening（冻结 foundation，标记 transport preview，分流 out-of-core integrations）

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
- Task-03（KCP）、Task-13（游戏网关安全骨架）、Task-14（背压优先级）、Task-15（指标导出适配）、Task-16 到 Task-32（KCP/PMTU preview）完成后，不继续把 BSD/Windows runtime PMTU source verification、cryptographic PMTU signal authentication、Reed-Solomon/multi-loss FEC、生产级 congestion/window tuning、AOI/多分片高级优化、可部署观测端点和跨进程/持久化 PMTU 服务接入 core。
- 这些后续能力必须先进入 adapter/example/experimental 设计，或者新建独立 intent 证明它们仍属于网络底座。

### Sprint E（M33，必须先做）
- 完成 `docs/game_server_network_base_scope_boundary.md` 和 `intents/architecture/game_network_base_scope.intent.md` 的边界落地。
- 给 KCP/PMTU preview 测试加范围标签，保证回归时能区分 foundation 与 experimental。
- 更新 overview/module map，让读者先看到分层，而不是把所有能力理解为 core。
- 冻结新的 KCP/FEC/PMTU 生产化任务，直到 preview 层和 adapter 边界拆清楚。

---

## 6. 直接行动清单（本地执行）

1. 先维护 `intents/architecture/game_network_base_scope.intent.md` 和 `docs/game_server_network_base_scope_boundary.md`，再改 core。
2. 对每个新增能力先判定：core、game-foundation、transport-preview、adapter、example。
3. 所有 KCP/PMTU/FEC 类 preview 测试必须带 `transport-experimental`、`kcp-preview` 或 `pmtu-preview` 标签。
4. 每个 PR 结束时补齐：
   - `Core Module Change Gate`
   - `Scope Gate`
   - 新增/修改测试文件列表
   - “未完待续”项（非阻塞问题）声明。
