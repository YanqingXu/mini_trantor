# 研发路线：先形成可信的小型 Reactor

本路线替代历史 v2–v6 与游戏网络 M1–M32 的功能扩张路线。
阶段以退出证据推进，不按版本编号递增或模块数量推进。

## 目标与取舍

交付一套可以讲清、测试和维护的 TCP Reactor：接收连接、处理字节、跨线程投递、
关闭并释放资源。协程只适配这些语义；游戏会话、房间、AOI、账号、应用协议和服务调度
由应用层负责。Linux 为第一验证平台，Windows 保留可测试的预览后端。

保留 TimerQueue、基础背压和 TLS 接点的理由是已有 TCP 超时、资源控制和连接机制依赖它们。
不能为了恢复早期目录而倒退到没有超时和关闭约束的实现。

## S0：裁剪与证据重置（本次）

- 移除游戏框架、协议生态、自研 KCP/PMTU/FEC 源码、安装 API 和专属测试。
- TcpServer 撤掉广播、session/group/AOI 以及 logic callback。
- 基础 MetricsHook 撤掉游戏、UDP 和广播类型，停止发展 metrics exporter。
- TLS 默认关闭；安装包仅请求所需依赖。
- 修复 Release 断言、PR 触发分支、机器绝对路径和提交构建缓存的问题。
- 修复本次定位的 EventLoop 队列启动、活动 Channel 移除、重复 loop 创建资源泄漏，
  以及 TcpServer 关闭 hook 的线程和次数问题。
- 建立当前理解文档与审计账本；旧文档进入 archive。

退出证据：裁剪边界由源码扫描及安装消费测试验证；保留测试全量运行。
具体结果以审计记录为准。S0 完成不意味着以下安全债务已完成。

## S1：正确性阻塞项，暂停新增功能

执行记录：S1-01 已开始。先以 `test_task_lifetime.cpp` 固化 sleep 等待时的提前销毁、
排队取消、移动赋值和父子 Task 析构合同；随后扩展 TCP read/write/close、组合器及发布顺序。
本记录不是 S1 完成声明，其余阶段继续按本路线执行。
S1-01a（sleep）已实现并通过 Linux ASan/UBSan 62/62、Windows Release 22/22、
定向 TSan 4/4；状态图、所有权合同和剩余范围见[协程实施记录](s1_coroutine_lifecycle.md)。
S1-01b（TCP 等待注销与发布）通过 ASan 全量 63/63、Windows Release 23/23；
完整 TSan 为 54/60，剩余 6 项已记录。Task 启动、组合器与 DNS 的 frame 生命周期仍待处理。
S1-01c（Task 启动与所有权转移）已通过 ASan 64/64、Linux Release 61/61、Windows Release 24/24。
继续处理组合器父 frame 和 DNS，S1 整体保持进行中。
S1-01d（组合器父 frame、同步完成与恢复权限）通过 ASan/UBSan 65/65、
Linux Release 62/62、Windows Release 25/25；完整 TSan 本轮 57/62。
新的协程合同全部通过，5 个失败入口均在已有 6 项账本中；本轮未复现的 threaded
TcpServer 报告继续保持开放，不能以一次通过关闭。接下来处理 DNS 生命周期。
S1-02a 已修复 DNS cache hit 的锁内回调、预先取消被 cache hit 覆盖，以及 registration
安装/注销竞争；[DNS 执行记录](s1_dns_lifecycle.md)保存回归证据。安全投递和 loop 关闭
仍在后续范围，DNS 整体生命周期尚未关闭。
S1-02b 建立 LoopHandle 的入队/关闭互斥，DNS worker 和取消回调不再保存裸 loop
指针，cache hit 统一排队。下一步补齐 ResolveAwaitable 的析构取消与其他 awaitable
迟到通知，随后进入线程启停状态机。
本轮 ASan 67/67、Linux Release 64/64、Windows Release 27/27，TSan 59/64；
新增 coroutine_idle_timeout 的 TcpServer 控制块报告进入同一分诊账本，不能因 DNS
合同通过而关闭其余阻塞项。
S1-02c 补齐 ResolveAwaitable 的单次等待/析构取消，并将 sleep/TCP 的迟到通知迁移到
安全投递边界；TCP 强引用只在 owner-loop 获取。当前转入线程启停与统一关闭，
完整 S1 门槛仍需剩余 TSan 报告和 TLS 身份验证等工作共同满足。
S1-03 已落实线程启动结果、提前退出保留、部分线程池回滚与拥有者停止；
原 pool wakeup/close 竞争通过状态锁消除。最终 ASan/UBSan 70/70、Linux Release
67/67、Windows Release 30/30，完整 TSan 本轮 65/67；历史未复现报告继续开放。
新增 DNS 控制块报告与独立 shared/weak 最小复现进入标准库插桩分诊，详见
[线程实施记录](s1_thread_lifecycle.md)与 [DNS 追加记录](s1_dns_lifecycle.md)。
下一步依次完成关闭重入/异常策略、剩余 TSan 分诊和 TLS 对端身份合同。
S1-03b 已关闭 Connector 错误线程入口和测试 TimerId 发布的两项同步违约；
ASan 70/70、Linux Release 67/67、Windows 30/30。系统 libc++ TSan 为 65/67，
本轮仅报告两个 TcpServer 控制块入口；其余历史控制块入口继续保留，正准备插桩标准库对照。
S1-04a 已修复取消捕获在锁内析构的重入死锁、观察者异常截断通知，以及 WhenAny
取消失败后遗留父任务挂起；见[回调实施记录](s1_callback_reentry.md)。ASan 71/71、
Release 68/68、Windows 31/31；插桩标准库下 TSan 首轮 68/68，但重复验证抓到
TcpServer 实例字段的真实竞争。下一项处理 worker 关闭通知与 base-loop 析构的边界，
不能用首轮通过或标准库误报归因代替此修复。

S1-04b 已将 TcpServer 关闭通知限定为 base LoopHandle 上的连接名消息，移除
worker 对 server 成员的读取和排队通知的强连接保活；析构先分离 map 再回调、
join worker。见[关闭记录](s1_server_close.md)。本地 ASan 72/72、Release 69/69、
插桩 libc++ TSan 69/69、Windows 32/32，五个历史/新增入口各重复 20 次通过。
永久 [TSan 运行库脚本与正负对照](tsan_toolchain.md) 已进入 CI，无测试排除或 suppression。
远端上一提交另暴露 DNS 首地址 IPv4 假设和 hostname echo 失败；下一项先核实
双栈候选顺序与 TcpClient 回退，再继续一般回调重入/异常及 TLS 身份验证。
S1、S2、S3 的整体退出要求保持不变。

| 顺序 | 任务 | 必须守住的合同 | 退出证据 |
| --- | --- | --- | --- |
| 1 | 协程挂起帧注销 | Task 提前销毁后，timer/I/O/queued resume/cancel 不访问失效 handle；先定义谁能销毁、在哪个 loop 销毁 | `tests/contract/coroutine/test_task_lifetime.cpp`；sleep/read/write/close、完成队列交错的 ASan 回归 |
| 2 | await_suspend 发布顺序 | 跨线程 arming 后不能继续访问可能已恢复/销毁的 awaiter；注册/取消 state 只能在 owner loop 修改 | `tests/contract/coroutine/test_suspend_publication.cpp`；显式同步构造竞争，TSan 验证 |
| 3 | DNS 关闭、缓存重入与取消注册 | cache hit 用户回调不在 cacheMutex 内执行；callbackLoop 活到最后一次投递；registration 初始化/注销不得竞争 | `tests/contract/dns/test_dns_lifetime.cpp`；cache callback 调 clearCache、pending resolve 后停 loop，并关闭已有 TSan 失败 |
| 4 | 线程启停状态机 | startLoop 不能因 init callback 提前 quit 永久等待；失败可回传；pool 不通过失效的裸 loop 指针 stop | `tests/contract/event_loop_thread/test_start_stop_failure.cpp`；init-quit、init-throw、早退、重复 stop |
| 5 | 关闭、重入和异常策略 | 分清“回调发起 close/stop”与“在回调中销毁 owner”；事件批处理、析构、drain deadline 有一致规则 | 扩展 `test_shutdown_ordering.cpp`、`test_connection_event_contract.cpp` 与 Channel failure contracts |
| 6 | TLS 对端身份 | 客户端默认验证证书链；hostname 同时用于名称校验；明确不验证模式 | 新增不可信证书及 hostname mismatch 拒绝合同，不能只证明自签 echo 成功 |

表内新测试文件名为待实施任务，不宣称已经存在。
S1 退出要求：所有已登记 P0 有回归，Linux Debug/Release 与 ASan/UBSan 全量通过，
TSan 全量通过并保存证据。本次已实跑 49/57，8 个失败入口及分诊见[审计记录](audit_2026-09-12.md#71-tsan-失败分诊)；
逐项修复库合同、测试同步或证明工具链归因，不能以排除/suppressions 代替关闭。
环境无法运行 TSan 时保持缺口，不记作通过。
所有新测试避免用 sleep 猜时序，优先 promise/barrier、可控 I/O 和确定的阶段边界。

## S2：可发布的最小 TCP 基线

依赖 S1 退出；每次只做一个边界清楚的变更。

1. 定义 queued work 在 quitting/stopped 时是否接受及如何反馈；外部生产者先停再销毁 loop。
2. 明确配置 API 何时可调用；统一 0 worker = base-loop 的含义和非法配置校验。
3. 完整验证 half-close、慢读者、输出硬上限、accept 公平性、连接拒绝和 FD 耗尽。
4. 把 Windows TCP server/client、关闭、backpressure 测试从编译预览扩展为行为验证；
   select 容量上限保持显式，不开始 IOCP/io_uring 后端扩张。
5. 固化两个小示例：callback echo 和受生命周期约束的 coroutine echo。

退出要求：当前 API 都有合同映射；安装可从全新前缀消费；Linux/Windows 的支持差异
有明确列表；关闭后 socket、Channel、timer、coroutine frame 数量回到基线。

## S3：负载证据与 0.1 发布候选

- 建立 TCP 请求/响应吞吐与 p50/p95/p99 延迟基线，记录硬件、编译模式、线程数、负载模型。
- 增加慢读者、突发连接、反复重连、停服期间写入的 soak；记录 RSS/FD/排队量，而非只测 echo 成功。
- PacketFramer fuzz 保存语料和最小化失败；smoke 次数不等于安全结论。
- 达到预先写下的资源和延迟阈值后，再考虑优化；禁止先写 lock-free、payload pool 或批量广播。

发布门槛：S1 无未关闭阻塞项，S2 合同与平台矩阵完整，S3 有可复现基线和至少一轮
持续运行记录。未满足前不重新标注 Stable。

## 明确撤销的研发方向

| 原方向 | 决策 | 重新进入的前提 |
| --- | --- | --- |
| Session/Pipeline/LogicLoop/AOI/安全策略 | 不再属于本库；应用工程自行组织 | 下游真实需求，独立仓库或适配工程，core 不持有业务类型 |
| KCP、SACK、PMTU、raw ICMP、FEC、拥塞调优 | 撤销自研生产化路线，源码退出 | 如有真实可靠 UDP 需求，先写独立 ADR、互操作和失败模型，再评估外部实现适配 |
| HTTP/WS/RPC、HttpClient、RPC pool | 停止客户端生态扩张，移出发布包 | TCP 基线发布后，由独立扩展消费窄接口，无反向依赖 |
| transport manager/session/channel 多层统一抽象 | 撤销预先统一多种协议的工作 | 至少两个真实消费者证明存在相同合同；不能以未来想象驱动 |
| metrics exporter、观测平台 | 仅留必要 hook，移除业务指标聚合 | 应用侧实现导出；新 hook 必须服务具体诊断问题并量化成本 |

## 日常研发规则

一次任务先给出一个失败场景、一个 invariant 和一份可执行验收，再写代码。
当前方向以 `reactor_scope_reset.intent.md` 为准；历史文档不构成新增功能授权。
变更说明必须回答五项 core gate，并列出未验证部分。不得以减少测试来处理保留能力的失败。
