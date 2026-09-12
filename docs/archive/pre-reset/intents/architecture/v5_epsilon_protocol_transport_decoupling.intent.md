# Architecture Intent: v5-epsilon — Protocol and Transport Decoupling

## 1. Intent

v5-epsilon 的核心目标不是新增协议能力，而是把现有协议层对传输层的依赖
收敛到更窄、更稳定的边界上，为后续 HTTP client、更多协议适配和未来传输替换
保留演进空间。

当前仓库里的 HTTP、WebSocket、RPC 虽然都已经作为协议层存在，但它们仍直接依赖
`TcpConnection` / `TcpConnectionPtr` / `Buffer*` 这组具体传输主线接口：

1. HTTP 直接在 `HttpServer` 中调用 `conn->send()` / `conn->shutdown()` /
   `conn->forceClose()`，并通过 `TcpConnection::setContext()` 承载协议状态
2. WebSocket upgrade 与帧处理路径直接消费 `TcpConnectionPtr + Buffer*`，升级前后
   都直接操作底层连接对象
3. RPC 直接以 `TcpConnectionPtr` 作为请求发送、响应回包和 pending request 生命周期
   关联对象

这些依赖在当前版本可以工作，但会造成几个长期问题：

1. 协议层继续绑定 `TcpConnection` 的具体 public surface，后续 HTTP client 或其他协议
   复用时会被迫复制这套耦合方式
2. 协议层边界与 transport/lifecycle 边界混杂，不利于明确哪些语义属于协议，哪些语义
   属于连接生命周期
3. TCP 和 TLS 虽然已经在 `ConnectionTransport` 中完成了一层收敛，但协议层还没有真正
   依赖“窄传输接口”，仍然在直接依赖具体连接类型

v5-epsilon 要解决的问题是：

- 协议层继续是协议适配器，而不是 `TcpConnection` 的深度朋友类
- `TcpConnection` 继续是连接生命周期中心，但协议层只依赖它暴露出来的一小组、
  明确受约束的 transport-facing 能力
- decoupling 不能引入第二套调度器、第二套 teardown 路径或隐藏所有权

业务价值：

1. 为 v6-alpha 的 HTTP client / client-side reuse 提前清理边界
2. 降低协议层新增能力时继续把 `TcpConnection` public API 越用越宽的风险
3. 保持 TCP/TLS 两种传输实现都能在同一协议边界下工作

---

## 2. In Scope

### 2.1 阶段目标

建立一层**窄的 protocol-facing transport abstraction**，让协议层只依赖以下稳定能力：

1. 发送字节序列
2. 请求半关闭或强制关闭
3. 访问和维护 per-connection protocol state
4. 查询连接是否仍处于可用状态
5. 在 owner loop 线程上处理输入缓冲中的增量数据

这层 abstraction 的目标是“缩窄协议层依赖面”，不是重新发明一个网络栈。

### 2.2 涉及模块

- `mini/net/TcpConnection.h/.cc`
- `mini/net/detail/ConnectionTransport.h/.cc`
- `mini/http/HttpServer.h/.cc`
- `mini/ws/WebSocketServer.h/.cc`
- `mini/ws/WebSocketConnection.h/.cc`
- `mini/rpc/RpcChannel.h/.cc`
- `mini/rpc/RpcServer.h/.cc`
- `mini/rpc/RpcClient.h/.cc`

建议新增模块：

- `mini/net/ProtocolConnection.h` 或同等语义命名的窄连接接口
- `mini/net/ProtocolConnectionAdapter.h/.cc` 或同等语义命名的适配层
- 若后续确有必要，再考虑 `mini/net/ProtocolCodec.h`；但它不是 v5-epsilon 的前置必需品

### 2.3 范围内工作项

1. 明确协议层允许依赖的最小 transport-facing surface
2. 将 HTTP 改为首先通过该窄接口工作，作为试点协议
3. 将 WebSocket 的 upgrade 和 post-upgrade 消息处理改为依赖该窄接口
4. 将 RPC 的 request send / response send / pending lifecycle 路径改为依赖该窄接口
5. 保留 `ConnectionTransport` 作为 plain TCP / TLS 的内部 transport helper，不将 TLS 细节上推到协议层
6. 明确协议状态与连接生命周期状态的边界文档

### 2.4 明确的阶段边界

v5-epsilon 完成后，仓库应达到以下状态：

1. 协议层不再需要直接依赖 `TcpConnection` 的宽接口来完成主路径
2. `TcpConnection` 仍是唯一的连接生命周期中心
3. plain TCP / TLS transport 差异继续留在 `ConnectionTransport` 和连接层，不向协议层外溢
4. HTTP / WS / RPC 对用户可见行为不变

---

## 3. Non-Responsibilities

v5-epsilon 明确**不**负责以下事项：

1. 不实现 HTTP client；这属于 v6-alpha 的 client ecosystem 范围
2. 不引入第二套 scheduler、executor 或 coroutine runtime
3. 不绕过 `TcpConnection` 的正常 close/error/force-close 收敛路径
4. 不引入“协议拥有底层连接”的隐藏所有权模型
5. 不为了抽象而抽象，不发明覆盖全仓库所有未来场景的 mega interface
6. 不改变现有 HTTP / WS / RPC 的用户可见协议行为，除非 contract test 也同步修改
7. 不将 backpressure、awaiter、TLS handshake 细节直接暴露给协议层
8. 不顺带推进跨平台、多 poller 后端或 HTTP/2 等额外目标

---

## 4. Core Invariants

### 4.1 边界不变量

1. 协议代码依赖的是窄 transport-facing surface，而不是 `TcpConnection` 的完整 public API
2. `TcpConnection` 仍然是连接 state machine、callback ordering 和 teardown convergence 的唯一中心
3. `ConnectionTransport` 继续只负责 transport 读写/握手/shutdown，不承担协议解析职责
4. Buffer 继续保持 protocol-agnostic；协议层只消费字节和 framing，不把协议语义下沉到 Buffer

### 4.2 生命周期不变量

5. 所有协议级 close / protocol error / malformed frame 的最终 teardown 都必须回到既有连接 close path
6. decoupling 不能创建“协议层自己直接释放连接资源”的旁路
7. protocol state 仍是一连接一份，不允许跨连接共享可变协议状态
8. 协议状态释放必须随连接 teardown 自然完成，不能要求额外的手工回收阶段

### 4.3 线程不变量

9. 协议状态只在 owner loop 线程访问和修改
10. 协议回调仍在 owner loop 线程上触发
11. cross-thread 用户调用仍然先经由 `TcpConnection` / `TcpClient` / `TcpServer` 现有投递语义回到 loop，再进入协议路径

### 4.4 传输不变量

12. TCP 和 TLS 都必须能在同一协议边界下工作
13. transport 替换不会改变协议层 callback ordering
14. 协议层不需要知道底层当前是 plain transport 还是 TLS transport

---

## 5. Threading Rules

### 5.1 协议输入处理

- HTTP request parsing、WebSocket frame decoding、RPC frame decoding 都发生在连接 owner loop 线程
- protocol adapter 不得把输入解析转移到其他线程
- 协议层不得在非 owner 线程直接读取或修改 protocol context

### 5.2 协议输出处理

- 协议层发送响应、pong、RPC response/error 时，只能通过窄 transport-facing interface 发起
- 该 interface 的实际执行仍回到 `TcpConnection` 的 owner-thread discipline

### 5.3 生命周期交错

- 协议回调不得制造新的跨线程 teardown 入口
- 协议层请求关闭连接时，必须通过已定义的 send/shutdown/force-close 风格接口回到现有生命周期模型
- 任何 protocol error 都不能绕开 `TcpConnection` 既有的 close/error 收敛规则

---

## 6. Ownership Rules

1. `TcpConnection` 继续拥有底层 Socket / Channel / Buffer / transport helper / connection context
2. 若引入 `ProtocolConnectionAdapter`，它的所有权必须明确，且生命周期不得超过其对应的 `TcpConnection`
3. 协议层自己的解析状态（如 `HttpContext`、WebSocket upgrade state、RPC pending map）仍属于 per-connection state
4. 协议层不直接拥有 transport-private 资源，不持有 `SSL*` 或等价 transport 细节对象
5. 如果协议层需要延迟响应，只能持有窄接口允许的安全引用形式，不能重新扩散对 `TcpConnection` 宽接口的依赖

---

## 7. Failure Semantics

### 7.1 协议错误

1. malformed HTTP request、invalid WebSocket frame、invalid RPC frame 的处理策略保持现有语义不变
2. 协议错误需要关闭连接时，关闭动作仍通过现有连接 teardown 路径完成

### 7.2 传输错误

3. transport failure 仍然先由连接层感知，再以既有 callback ordering 影响协议层
4. 协议抽象层不得吞掉 transport failure 或发明独立的 transport error 恢复模型

### 7.3 半关闭与关闭顺序

5. HTTP `Connection: close`、WebSocket close handshake、RPC 连接异常下 pending fail-all 的行为保持现有 contract
6. decoupling 后不得改变 send-then-shutdown 的顺序语义

---

## 8. Proposed Implementation Stages

### E1. Boundary Definition

目标：先固定协议层允许依赖的最小接口，再动实现。

产物：✅

1. `mini/net/ProtocolConnection.h` — 窄接口 `IProtocolConnection`（send/shutdown/forceClose/connected/getLoop/name/setProtocolContext/getProtocolContext）
2. `mini/net/ProtocolConnectionAdapter.h/.cc` — `ProtocolConnectionAdapter` 实现（createAndBind/getFrom/sharedFrom）
3. `tests/unit/net/test_protocol_connection_adapter.cpp` — 7 个单元测试全部通过
4. HTTP / WS / RPC 依赖清单分析完成

### E2. HTTP Pilot ✅ COMPLETE

目标：让 HTTP 成为第一个迁移到窄接口的协议层。

完成的工作：

1. `mini/http/HttpServer.cc`：onConnection 调用 createAndBind，send/shutdown 通过 adapter
2. `mini/http/HttpServer.h`：onRequest 签名改为 `IProtocolConnection*`；新增 `stop()`
3. `tests/contract/http/test_http_transport_contract.cpp`：T1~T4 全部通过
4. `tests/contract/http/test_http_server.cpp` 失败为预存在 bug，非本次回归

### E3. WebSocket and RPC Migration ✅ COMPLETE

目标：把更复杂的 upgrade / bidirectional response 语义也迁到窄接口。

完成的工作：

1. `mini/ws/WebSocketServer.cc`：onConnection 调用 createAndBind，ConnectionContext 存储在 adapter->setProtocolContext()，send/shutdown 通过 adapter
2. `mini/rpc/RpcServer.cc`：createAndBind + RpcChannel 存储在 adapter->setProtocolContext()
3. `mini/rpc/RpcChannel.cc`：respond/respondError lambda 捕获 shared_ptr<ProtocolConnectionAdapter>
4. 所有 WS/RPC contract + integration 测试无回归

### E4. Documentation and Cleanup ✅ COMPLETE

目标：补齐架构文档、移除多余直接依赖、收敛边界说明。

完成的工作：

1. 本意图文档更新，标注各阶段完成状态
2. 协议层不再通过 TcpConnection::setContext() 直接存储协议状态
3. 协议层仍保留 TcpConnection.h 用于 lifecycle check (conn->connected()) 和接口签名

---

## 9. Test Contracts

### 9.1 Unit

- `tests/unit/net/test_protocol_connection_adapter.cpp`
  - 验证窄接口对 send / shutdown / force-close / context / connected-state 的语义
- 若确实引入 codec 适配层，再补 `tests/unit/net/test_protocol_codec_adapter.cpp`

### 9.2 Contract

- `tests/contract/http/test_http_server.cpp`
- `tests/contract/http/test_http_transport_contract.cpp`
- `tests/contract/ws/test_ws_server.cpp`
- `tests/contract/rpc/test_rpc_server.cpp`

重点验证：

1. decoupling 后协议行为对用户保持不变
2. 回调仍在 owner loop 线程调用
3. 关闭顺序与错误处理不变

### 9.3 Integration

- `tests/integration/rpc/test_rpc.cpp`
- 现有 HTTP / WS / RPC main path integration 用例（如存在）

重点验证：

1. HTTP keep-alive 主路径无回归
2. WebSocket upgrade -> frame -> close 主路径无回归
3. RPC request/response/timeout/coroutine 主路径无回归

---

## 10. Exit Signals

v5-epsilon 退出时，必须满足以下信号：

1. 协议层不再依赖 `TcpConnection` 的过多内部或宽 public 细节
2. 传输、缓冲、编解码、协议状态的边界比当前更清晰
3. `TcpConnection` 仍是唯一生命周期中心，没有出现第二套 teardown 语义
4. TCP/TLS 主路径都不被破坏
5. HTTP / WS / RPC 现有 contract 和 integration 主路径保持通过

---

## 11. Review Questions

1. 这次改动是真正减少了协议层依赖面，还是只是给 `TcpConnection` 又套了一层同样宽的 wrapper？
2. 新 abstraction 的 owner-thread 语义是否清晰？
3. 协议层是否仍能通过已有测试证明行为不变？
4. 是否把 transport、protocol、lifecycle 三类职责分清了？
5. 是否出现了新的隐藏所有权或新的 teardown 旁路？
6. 哪个精确测试文件证明了 HTTP / WS / RPC 主路径没有被解耦改坏？