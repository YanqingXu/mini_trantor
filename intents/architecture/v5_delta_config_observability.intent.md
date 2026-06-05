# Architecture Intent: v5-delta — Configuration and Observability

## 1. Intent

v5-delta 的核心目标是将 mini-trantor 从"参数硬编码、行为只能靠读代码和日志排查"的状态，
升级为**运行时行为可配置、关键事件可观测**的库。

当前问题：
1. **配置碎片化** — Connector 重试延迟、DnsResolver 线程数/缓存 TTL、TcpServer 线程数/空闲超时/背压阈值等参数，散落在各模块的 `set*()` 方法或构造函数默认值中，无法批量、显式地设置
2. **TcpClient 无法配置 Connector** — `Connector::setRetryDelay()` 存在但 TcpClient 未暴露；连接超时完全缺失
3. **可观测性为零** — 无任何 metrics hook、事件回调或结构化诊断点。连接建立/关闭/超时/重试/背压暂停恢复/TLS 握手等关键事件仅依赖 LOG_INFO/LOG_ERROR，无回调通道
4. **drain 策略缺失** — v5-beta 中 TcpServer::stop() 只做 force-close，drain-aware API（等待在飞请求完成）被标记为延迟到 v5-delta

业务价值：
- **运维效率** — 无需修改代码即可调优重连策略、连接超时、DNS 缓存等参数
- **故障诊断** — 通过 hook 回调获取连接生命周期、背压、TLS 握手等结构化事件，替代 grep 日志
- **安全加固** — 连接超时防止半开连接耗尽资源；drain 策略防止关闭时丢数据
- **库成熟度** — 从"能用"到"可观测、可调优"的必要一步

---

## 2. In Scope

### 2.1 配置体系（Options 结构体）

| Options 类型 | 覆盖的配置项 | 默认值（与现有行为一致） |
|---|---|---|
| `ConnectorOptions` | `initRetryDelay`, `maxRetryDelay`, `connectTimeout`, `enableRetry` | 500ms, 30s, 0(无超时), false |
| `DnsResolverOptions` | `numWorkerThreads`, `cacheTtl`, `enableCache` | 2, 60s, false |
| `TcpServerOptions` | `numThreads`, `idleTimeout`, `backpressureHighWaterMark`, `backpressureLowWaterMark`, `reusePort` | 1, 0(无超时), 0(禁用), 0, true |
| `TcpClientOptions` | `connector`(内嵌 ConnectorOptions), `retry` | 默认 ConnectorOptions, false |

设计原则：
- Options 是简单值类型 struct，可复制、可比较
- 支持 builder 风格链式设置
- 在构造/启动前设置，不支持运行时动态变更
- 不引入全局配置框架或配置文件解析

### 2.2 可观测性（MetricsHook）

事件分类与回调类型：

| 事件域 | 事件枚举 | 回调签名 | 触发时机 |
|---|---|---|---|
| 连接生命周期 | `ConnectionEvent::Connected/Disconnected/IdleTimeout/ForceClosed` | `void(const TcpConnectionPtr&, ConnectionEvent)` | owner loop 线程 |
| 背压控制 | `BackpressureEvent::ReadPaused/ReadResumed` | `void(const TcpConnectionPtr&, BackpressureEvent, size_t bufferedBytes)` | owner loop 线程 |
| 连接器 | `ConnectorEvent::ConnectAttempt/ConnectSuccess/ConnectFailed/RetryScheduled/SelfConnectDetected/ConnectTimeout` | `void(const InetAddress&, ConnectorEvent)` | owner loop 线程 |
| TLS 握手 | `TlsEvent::HandshakeStarted/HandshakeCompleted/HandshakeFailed` | `void(const TcpConnectionPtr&, TlsEvent)` | owner loop 线程 |

注入 API：
- `TcpServer::setConnectionEventCallback(...)`
- `TcpServer::setBackpressureEventCallback(...)`
- `TcpServer::setTlsEventCallback(...)`
- `TcpClient::setConnectorEventCallback(...)`
- `TcpClient::setConnectionEventCallback(...)`
- `TcpClient::setTlsEventCallback(...)`

设计原则：
- 回调在 owner loop 线程调用，不违反 one-loop-per-thread 纪律
- 不设回调时零开销（`if (callback_) callback_(...)` 模式）
- 回调是 `std::function`，不持有有状态计数器对象
- 回调中用户代码不应抛异常（文档约束）

### 2.3 连接超时

- `ConnectorOptions::connectTimeout` — 当值 > 0 时，在 `connecting(sockfd)` 中注册 `EventLoop::runAfter(connectTimeout, ...)` 定时器
- 超时回调：检查 `state_ == kConnecting`，若是则关闭 socket 并触发重试或失败
- 连接成功时立即取消超时定时器

### 2.4 Drain-Aware API（延迟项兑现）

- `TcpServer::stop(Duration drainTimeout)` — 优雅关闭重载
- 在 drain 时间内等待在飞连接正常关闭；超时后 force-close 剩余连接
- 原有 `TcpServer::stop()` 行为不变（立即 force-close）

### 2.5 涉及模块

- `mini/net/Connector.h/.cc` — 接受 ConnectorOptions，新增连接超时
- `mini/net/DnsResolver.h/.cc` — 接受 DnsResolverOptions
- `mini/net/TcpServer.h/.cc` — 接受 TcpServerOptions，新增 hook 注入，新增 drain stop
- `mini/net/TcpClient.h/.cc` — 接受 TcpClientOptions，暴露 Connector 配置，新增 hook 注入
- `mini/net/TcpConnection.h/.cc` — 在关键路径调用 hook
- `mini/net/ConnectionBackpressureController.h/.cc` — 在暂停/恢复时调用 hook
- `mini/net/ConnectionTransport.h/.cc`（TLS 路径） — 在握手事件时调用 hook
- 新增：`mini/net/ConnectorOptions.h`
- 新增：`mini/net/DnsResolverOptions.h`
- 新增：`mini/net/TcpServerOptions.h`
- 新增：`mini/net/TcpClientOptions.h`
- 新增：`mini/base/MetricsHook.h`

---

## 3. Non-Responsibilities

- 不引入全局配置框架（如 YAML/JSON 配置文件解析、全局单例 Registry）
- 不引入跨线程可观测路径（hook 必须在 owner loop 线程调用）
- 不引入有状态的 metrics 收集器（如内置计数器、直方图）
- 不实现分布式追踪（tracing）或 OpenTelemetry 集成
- 不替换 Logger 为异步日志（可作为 v5-zeta 的工程加固项）
- 不用日志断言替代 contract 测试
- 不在数据面（每消息路径）添加 hook，仅在控制面（连接建立/关闭/背压变更）添加
- 不实现自定义 backoff 策略插件接口（留作 v6 扩展）

---

## 4. Core Invariants

### 4.1 配置不变量

1. **Options 是值类型** — 无虚函数、无堆分配、可 trivially copy
2. **默认值与现有行为一致** — 不设 Options 时，所有行为与 v5-gamma 完全相同
3. **Options 在启动前设置** — 构造函数或 `start()` 前设置生效；运行时变更不被支持
4. **配置边界窄且局部** — 每模块独立 Options，不产生跨模块耦合

### 4.2 可观测性不变量

5. **Hook 在 owner loop 线程调用** — 不引入跨线程可观测路径
6. **Hook 不持有状态** — 回调是 `std::function<void(...)>`，不是有状态对象
7. **不设 Hook 时零开销** — 仅一次 null 检查的分支预测成本
8. **Hook 不改变行为语义** — hook 是纯通知，不影响连接生命周期、重试决策或背压逻辑

### 4.3 连接超时不变量

9. **超时与连接成功无竞态** — 两者都在 owner loop 上序列化；超时回调先检查 `state_`
10. **超时触发走正常失败路径** — 不创建旁路 teardown

### 4.4 向后兼容不变量

11. **现有 `set*()` API 保留** — 不删除任何公共 API
12. **现有构造函数签名保留** — Options 通过新增重载提供，不修改已有签名
13. **现有行为不变** — 不设 Options 时的行为与 v5-gamma 完全一致

---

## 5. Threading Rules

### 5.1 配置

- Options 设置在构造函数或 `start()` 前完成，无跨线程读写
- Options 值在对象构造后不可变（const 语义或文档约束）
- TcpClient 的 `setConnectorOptions()` 必须在 `connect()` 前调用（owner loop 线程或启动前）

### 5.2 Hook 回调

- 所有 hook 回调在 owner EventLoop 线程上同步调用
- Hook 设置在 `start()` 前完成（与现有 callback 设置模式一致）
- Hook 不引入新的线程交互点

### 5.3 连接超时

- 超时定时器注册和取消在 owner loop 线程
- 超时回调在 owner loop 线程
- 与 handleWrite 的交错在 owner loop 上序列化

### 5.4 Drain

- `stop(Duration)` 在 base loop 线程执行
- drain 超时定时器在 base loop 线程
- 连接关闭回调正常回流到 base loop

---

## 6. Ownership Rules

- Options 结构体由调用者创建，值传递（copy）到目标模块
- TcpClient 拥有 Connector（及 ConnectorOptions 副本）
- TcpServer 拥有 Acceptor、EventLoopThreadPool（及 TcpServerOptions 副本）
- DnsResolver 持有 DnsResolverOptions 副本
- Hook 回调 `std::function` 由 TcpServer/TcpClient 持有副本
- Hook 不引入新的共享所有权

---

## 7. Failure Semantics

### 7.1 配置

- 无效 Options 值（如 `connectTimeout < 0`、`numWorkerThreads == 0`）在设置时被拒绝（assert 或 throw）
- 超出合理范围的值（如极大重试延迟）由文档约束，不做运行时裁剪

### 7.2 Hook

- Hook 回调中抛异常：文档明确禁止；v1 不加 try-catch 保护（依赖文档约束，避免增加复杂度）
- Hook 回调中调用阻塞操作：文档明确禁止（会阻塞 owner loop 线程）

### 7.3 连接超时

- 超时触发时关闭 socket，走正常 connect-failure + retry 路径
- 超时回调检查 `state_ == kConnecting`，若已变为 kConnected 则忽略
- 连接成功时取消超时定时器；若定时器已触发（不可能在同一轮 loop 中交错），正常路径胜出

### 7.4 Drain

- drain 超时后强制关闭所有剩余连接，行为与 `stop()` 一致
- 在 drain 期间新连接被拒绝（Acceptor 已停止）

---

## 8. State Machine Changes

### 8.1 Connector 新增超时状态交互

```
                   start()
  kDisconnected ──────────── kConnecting
                                    │
                 ┌──────────────────┤
                 │                  │
         handleWrite()       connectTimeout (if configured)
         SO_ERROR==0               │
                 │                  │
                 ▼                  ▼
           kConnected         retry() / fail
```

连接超时是 kConnecting 状态的退出条件之一，与 handleWrite/handleError 并列。
超时触发后状态转换与 connect-failure 相同：关闭 socket → 可选 retry。

### 8.2 TcpServer 新增 drain 状态

```
  Running ──stop()──► Draining ──drain timeout──► Stopped
                         │
                   all connections closed
                         │
                         ▼
                      Stopped
```

Draining 状态：Acceptor 已停止，等待在飞连接关闭。超时后强制关闭。

---

## 9. Collaboration

```
TcpClientOptions ──► TcpClient ──► Connector (with ConnectorOptions)
                                    │
                                    ├── ConnectorEventCallback (hook)
                                    └── connectTimeout (via EventLoop timer)

TcpServerOptions ──► TcpServer ──► Acceptor
                     │  ├── EventLoopThreadPool (numThreads)
                     │  ├── idleTimeout (via EventLoop timer)
                     │  ├── backpressure (via ConnectionBackpressureController)
                     │  ├── ConnectionEventCallback (hook)
                     │  ├── BackpressureEventCallback (hook)
                     │  └── TlsEventCallback (hook)

DnsResolverOptions ──► DnsResolver ──► worker thread pool
                                       └── cache (TTL)

MetricsHook (event enums + callback types)
  ├── used by TcpServer (connection, backpressure, TLS hooks)
  ├── used by TcpClient (connector, connection, TLS hooks)
  └── all callbacks fire on owner loop thread
```

---

## 10. Extension Points

- 自定义 backoff 策略接口（v6-alpha：指数/线性/固定/无重试策略可插拔）
- 异步日志缓冲（v5-zeta：AsyncLogger）
- 连接超时后的回调策略（v6：onTimeout 回调 vs 自动重试）
- drain 策略可配置化（v6：自定义 drain 判定谓词）
- 分布式追踪集成（v6：OpenTelemetry span 注入）
- metrics 聚合接口（v6：内置计数器/直方图，可选导出）

---

## 11. Compatibility Plan

### 11.1 保留的现有 API

| 类 | 方法 | 兼容方式 |
|---|---|---|
| TcpServer | `setThreadNum(int)` | 保留，内部委托 Options |
| TcpServer | `setIdleTimeout(Duration)` | 保留，内部委托 Options |
| TcpServer | `setBackpressurePolicy(size_t, size_t)` | 保留，内部委托 Options |
| TcpServer | `setThreadInitCallback(...)` | 保留 |
| TcpServer | `enableSsl(...)` | 保留 |
| TcpClient | `enableRetry()` / `disableRetry()` | 保留，与 Options 优先级一致 |
| TcpClient | `enableSsl(...)` | 保留 |
| Connector | `setRetryDelay(Duration, Duration)` | 保留，与 Options 优先级一致 |
| DnsResolver | `DnsResolver(size_t)` | 保留，委托到 DnsResolverOptions 构造 |
| DnsResolver | `enableCache(seconds)` / `clearCache()` | 保留 |

### 11.2 新增 API

| 类 | 新增方法/构造器 |
|---|---|
| TcpServer | `TcpServer(EventLoop*, InetAddress, string, TcpServerOptions)` |
| TcpServer | `setConnectionEventCallback(...)` |
| TcpServer | `setBackpressureEventCallback(...)` |
| TcpServer | `setTlsEventCallback(...)` |
| TcpServer | `stop(Duration drainTimeout)` |
| TcpClient | `TcpClient(EventLoop*, InetAddress, string, TcpClientOptions)` |
| TcpClient | `setConnectorEventCallback(...)` |
| TcpClient | `setConnectionEventCallback(...)` |
| TcpClient | `setTlsEventCallback(...)` |
| Connector | `Connector(EventLoop*, InetAddress, ConnectorOptions)` |
| DnsResolver | `DnsResolver(DnsResolverOptions)` |

### 11.3 优先级规则

当 Options 和 `set*()` 同时使用时：
- Options 在构造时应用
- `set*()` 在构造后调用，覆盖 Options 中的对应字段
- 这是向后兼容的合理行为：用户先构造 Options，再用 `set*()` 微调

---

## 12. Test Contracts

### 12.1 单元测试

| 测试文件 | 覆盖内容 |
|---|---|
| `tests/unit/net/test_connector_options.cpp` | ConnectorOptions 默认值验证；自定义值验证；无效值拒绝 |
| `tests/unit/net/test_dns_resolver_options.cpp` | DnsResolverOptions 默认值验证；自定义值验证 |
| `tests/unit/net/test_server_options.cpp` | TcpServerOptions 默认值验证；与 set*() 优先级验证 |
| `tests/unit/net/test_client_options.cpp` | TcpClientOptions 默认值验证；内嵌 ConnectorOptions 传播验证 |
| `tests/unit/base/test_metrics_hook.cpp` | 事件枚举值正确；回调类型可构造 |

### 12.2 契约测试

| 测试文件 | 覆盖内容 |
|---|---|
| `tests/contract/net/test_options_contract.cpp` | Options 传播到内部模块；默认行为不变；选项在 start() 前设置生效；set*() 覆盖 Options |
| `tests/contract/net/test_metrics_hook_contract.cpp` | Hook 在 owner loop 线程调用；不设 hook 时行为不变；各事件类型正确触发；hook 不改变生命周期语义 |
| `tests/contract/net/test_connect_timeout_contract.cpp` | 连接超时配置生效；超时触发走正常失败路径；超时与连接成功无竞态 |
| `tests/contract/net/test_drain_stop_contract.cpp` | drain stop 等待连接关闭；drain 超时后 force-close；drain 期间无新连接 |

### 12.3 集成测试

| 测试文件 | 覆盖内容 |
|---|---|
| `tests/integration/net/test_connect_timeout.cpp` | 连接不可达 IP 验证超时；超时后重试 |
| `tests/integration/net/test_metrics_e2e.cpp` | 端到端 metrics：连接→消息→断开 全流程 hook 触发验证 |
| `tests/integration/net/test_drain_shutdown.cpp` | 多线程 server drain shutdown 在飞连接正常关闭 |

---

## 13. Dependency Analysis

```text
Phase 1.1: ConnectorOptions          ──┐
Phase 1.2: DnsResolverOptions        ──┤── Phase 4 测试
Phase 1.3: TcpServerOptions          ──┤
Phase 1.4: TcpClientOptions          ──┘
                                          │
Phase 2.1: MetricsHook 定义          ──┐
Phase 2.2: Hook 注入 API            ──┤── Phase 4 测试
Phase 2.3: 关键路径 hook 调用       ──┘
                                          │
Phase 3.1: Connector 连接超时       ────── Phase 4 测试
Phase 3.2: TcpServer drain stop     ────── Phase 4 测试
                                          │
Phase 5:   文档/意图更新           ────── 最后

Phase 1 和 Phase 2 无依赖关系，可并行推进。
Phase 3.1 依赖 Phase 1.1 (ConnectorOptions)。
Phase 3.2 独立于 Phase 1/2。
```

---

## 14. Risk Analysis

### 风险 1：Options 与 set*() 并存导致 API 碎片化

**影响**：用户困惑"该用哪种配置方式"。

**应对**：
- Options 是推荐方式；文档明确推荐
- Options 内部最终委托已有的 `set*()` 方法，避免两套独立逻辑
- 考虑在 v6 中 deprecate 散装 `set*()`

### 风险 2：Hook 回调引入性能开销

**影响**：关键路径增加分支判断。

**应对**：
- 不设回调时仅一次 null 检查（分支预测几乎零成本）
- 回调同步调用，不引入锁或队列
- 不在数据面（每消息）添加 hook

### 风险 3：连接超时与连接成功竞态

**影响**：逻辑错误导致 double-close 或泄漏。

**应对**：
- 超时回调和 handleWrite 在 owner loop 上序列化，天然无竞态
- 超时回调先检查 `state_ == kConnecting`，状态已变则忽略
- 连接成功时立即取消定时器

### 风险 4：Hook 回调中用户代码异常

**影响**：异常传播到 owner loop，中断事件处理。

**应对**：
- 文档明确禁止 hook 回调抛异常或阻塞
- v1 不加 try-catch（避免复杂度），依赖文档约束
- v5-zeta 可考虑增加 try-catch 保护作为工程加固

### 风险 5：构造函数签名变更破坏现有调用者

**影响**：编译错误。

**应对**：
- 所有现有构造函数签名保留不变
- Options 通过新增重载提供
- 旧构造函数内部构造默认 Options 委托

---

## 15. Exit Signals

v5-delta 完成的充分必要条件：

1. ✅ Connector 重连延迟、连接超时可通过 ConnectorOptions 显式配置
2. ✅ DnsResolver 线程数、缓存 TTL 可通过 DnsResolverOptions 显式配置
3. ✅ TcpServer 线程数、空闲超时、背压阈值可通过 TcpServerOptions 批量配置
4. ✅ TcpClient 可通过 TcpClientOptions 配置 Connector 重试策略
5. ✅ 连接建立/关闭/超时/强制关闭有 ConnectionEvent hook
6. ✅ 背压暂停/恢复有 BackpressureEvent hook
7. ✅ 连接尝试/成功/失败/重试/自连接/超时有 ConnectorEvent hook
8. ✅ TLS 握手开始/完成/失败有 TlsEvent hook
9. ✅ 所有 hook 在 owner loop 线程调用
10. ✅ 不设 Options 时不设 hook 时，行为与 v5-gamma 完全一致
11. ✅ TcpServer 支持 drain-aware stop
12. ✅ 连接超时实际生效
13. ✅ 所有现有测试无回归
14. ✅ 新增 unit / contract / integration 测试覆盖

---

## 16. Review Questions

- 这个配置项是否属于库层职责？（不应暴露应用层参数）
- Hook 回调的线程上下文是否明确？
- 是否增加了可诊断性但模糊了所有权？
- 默认值是否与 v5-gamma 行为完全一致？
- Options 和 set*() 的优先级是否清晰？
- 连接超时的状态机交互是否完整？
- drain stop 是否引入了新的生命周期复杂性？
