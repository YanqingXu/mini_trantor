# 游戏服务器网络底座收口边界

本文把 M1-M32 之后的 mini-trantor 明确收口为：

> game-network foundation + explicit transport preview

也就是说，当前项目可以继续服务于游戏服务器网络底座，但不能继续无边界地膨胀为完整商业网关、游戏框架或自研生产级传输协议栈。

## 1. 当前结论

M1-M32 没有整体跑偏，但 KCP/PMTU/FEC 方向已经逼近边界。继续推进前必须先完成边界硬化：

- 核心网络底座保持小而明确。
- 游戏层只保留网络入口、session、logic handoff、broadcast、backpressure、安全骨架。
- KCP 高级能力统一标记为 transport preview / experimental。
- 账号、安全审计、AOI、分布式网关、生产 FEC、生产拥塞控制、观测平台全部移出 core 路线图。

## 2. 分层边界

| 层级 | 定位 | 可继续内置 | 不应继续扩张 |
|---|---|---|---|
| L0 Reactor Core | 网络运行时核心 | EventLoop、Channel、Poller、TcpConnection、TcpServer、TimerQueue、线程池、协程桥 | 业务状态、协议研究、平台服务 |
| L1 Transport Foundation | 通用传输底座 | TCP/TLS、UDP 基线、transport endpoint、framing、codec adapter、轻量 metrics hook | 生产级 KCP/QUIC、跨进程 PMTU、复杂 FEC |
| L2 Game Foundation | 游戏网络接入层 | PlayerSession、SessionManager、GameServerPipeline、LogicLoop、Broadcast、基础安全、基础背压 | 账号系统、房间状态、AOI、分片调度 |
| L3 Transport Preview | 实验传输层 | KCP preview、PMTU adapter、raw ICMP preview、SACK、cwnd preview、redundant copy、XOR parity | 声称生产可用、默认接管 core 语义 |
| L4 Out-of-Core Integrations | 外部适配层 | 示例、adapter、plugin、下游服务 | 进入 `EventLoop`/`UdpSocket`/`KcpTransport` 主职责 |

## 3. M33 收口动作

M33 不继续加传输特性。它只做边界硬化：

1. 新增 `intents/architecture/game_network_base_scope.intent.md`，让范围判断先有 intent。
2. 新增本文档，作为 roadmap 和 review 的 scope gate。
3. 将 roadmap 中的“继续 KCP 生产化”改为“暂停核心功能扩张，先分层”。
4. 给 KCP/PMTU preview 相关测试加 `transport-experimental`、`kcp-preview`、`pmtu-preview` 标签。
5. 后续新增高级传输、安全、AOI、观测平台能力时，默认进入 adapter/example/experimental，而不是 core。

## 4. 允许继续作为底座推进的事项

这些事项仍属于网络底座：

- 修正 EventLoop、Channel、Poller、TcpConnection、UdpSocket、KcpTransport 的线程亲和与生命周期 bug。
- 为已有 public API 补 contract test。
- 强化 UDP read budget、基础 backpressure、公平性指标。
- 优化 GameServerPipeline 到 SessionManager / LogicLoop 的 handoff 语义。
- 补齐 BroadcastRouter / BroadcastDispatcher 的生命周期、线程和批量发送 contract。
- 保持 KCP preview 的现有 contract 不回退。
- 把现有 preview 能力拆小、标注、文档化、隔离。

## 5. 必须外置或降级为实验的事项

这些事项不能直接成为 core roadmap：

- auth provider、账号系统、封禁、风控、设备指纹、审计日志。
- AOI、空间索引、房间/副本状态、游戏世界同步。
- gateway shard、session migration、跨进程 fanout、service discovery。
- Prometheus scrape server、push gateway、告警、dashboard、持久化指标系统。
- cryptographic PMTU authentication、router trust model。
- Reed-Solomon、多丢包 FEC、自适应冗余控制器。
- 生产级 congestion control 和 KCP window tuning。
- disk/cross-process/distributed PMTU cache。

## 6. Scope Gate

每个后续变更必须先回答：

1. 这是多数游戏服务器网络底座都需要的能力吗？
2. 它是否仍保持 owner EventLoop 语义？
3. 它是否没有拥有业务状态、部署拓扑或平台服务？
4. 它的 contract 是否能用测试表达，而不是变成协议研究？
5. 它应该属于 core、game foundation、transport preview、adapter 还是 example？
6. 哪个测试文件和标签证明它没有越界？

无法明确回答时，默认不要进 core。

## 7. 测试标签约定

- `transport`：通用传输底座。
- `game` / `logic`：游戏网络接入和逻辑 handoff。
- `metrics`：轻量观测钩子或无依赖 snapshot。
- `transport-experimental`：高级传输预览，不代表生产能力。
- `kcp-preview`：KCP-style reliable UDP preview。
- `pmtu-preview`：PMTU signal/cache/query/ICMP preview。

实验标签只用于辨认范围，不用于隐藏失败测试。默认构建仍应覆盖这些 preview contract，避免已有能力悄悄退化。

## 8. 后续路线建议

下一阶段应命名为“scope hardening / boundary split”，而不是新的 KCP 生产化阶段。

推荐顺序：

1. 清点 KCP advanced options，把 preview 默认关闭、文档标注和测试标签保持一致。
2. 把可外置的观测、安全、AOI、分布式网关想象成 adapter seam，不在 core 中继续增加实现。
3. 更新框架理解文档和模块地图，让读者先看到分层边界。
4. 再决定是否把 transport preview 物理移动到 `mini/net/kcp/experimental` 或独立 target。

完成这些之前，不建议继续添加 cryptographic PMTU、Reed-Solomon FEC、adaptive redundancy、AOI 或可部署 metrics endpoint。
