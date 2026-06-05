# v6-alpha: Client Ecosystem — HTTP Client & RPC Connection Pool

## 1. Intent

v5 完成了服务端核心、底座一致性和工程护栏。v6-alpha 的目标是补齐 **客户端侧** 的核心能力：

- HTTP/1.1 Client（基于现有 TcpClient + HttpServer 生态）
- RPC 连接池（基于现有 RpcClient + RpcChannel 复用机制）
- 以协程接口为主、回调接口为辅，约束统一的错误处理模式

本阶段不引入服务发现（留给 v6-beta）、不引入 HTTP/2（留给 v7）、不引入 WebSocket Client（独立扩展）。

---

## 2. Scope

### In Scope

#### Sub-stage A: 基础设施增强（并行任务）

- **A1: HttpResponse 解析器**
  - 新增 `mini/http/HttpResponseContext.h` / `.cc`
  - 基于 HttpContext 的 HTTP 响应状态机（状态行 + 头部 + 正文）
  - 单元测试覆盖正常响应、分块响应、Connection:close、400/404/500

- **A2: RpcPoolOptions 定义**
  - 新增 `mini/rpc/RpcPoolOptions.h`
  - v5-delta Options 风格：minConnections / maxConnections / createOnDemand / connector

#### Sub-stage B: HTTP Client（与 C 并行）

- **B1: HttpClient 核心类**
  - 文件：`mini/http/HttpClient.h` / `.cc`
  - 内部持有 TcpClient，管理 HTTP/1.1 keep-alive 连接
  - GET / POST / 自定义方法的回调接口
  - 连接状态机：kIdle → kWaitingResponse → kClosed/Reconnecting

- **B2: Keep-alive 连接复用**
  - 内联在 HttpClient 中，不独立模块
  - 单连接串行请求/响应（非 pipeline）
  - Connection:close 检测与自动重连
  - 服务端空闲断开时自动重建

- **B3: 协程 awaitable 接口**
  - `CallAwaitable` 与 RpcClient::CallAwaitable 对称
  - 返回 `Expected<HttpResponse, NetError>`（不抛异常）
  - 超时通过 `withTimeout` 或内部定时器实现

- **B4: HttpClientOptions**
  - enableKeepAlive / timeoutMs / enableRetry
  - 内嵌 ConnectorOptions（复用 v5-delta 配置体系）

- **B5: 测试**
  - unit: HttpResponseContext 解析
  - contract: callback 线程亲和、keep-alive 复用、Connection:close、错误处理
  - integration: GET/POST 全链路 round-trip、超时

#### Sub-stage C: RPC 连接池（与 B 并行）

- **C1: RpcConnectionPool 核心**
  - 文件：`mini/rpc/RpcConnectionPool.h` / `.cc`
  - 固定大小连接池（minConnections ~ maxConnections）
  - Round-robin 分发请求
  - 内部池化管理 TcpClient + RpcChannel，不额外包装 RpcClient

- **C2: Lazy 健康检查**
  - 不做主动心跳，采用被动检测
  - 连接断开时自动从池移除并补充重建
  - engine 池中移除后 fail 该连接所有 pending 请求

- **C3: 池生命周期**
  - `start()`: 建立 minConnections 条连接
  - `stop()`: 标记停止、failAllPending、关闭所有连接
  - 池操作限定在 owner loop 线程（无锁设计）

- **C4: RpcPoolOptions**
  - minConnections / maxConnections / createOnDemand
  - 内嵌 ConnectorOptions

- **C5: 测试**
  - unit: 池状态管理、连接选择、round-robin
  - contract: 生命周期、pendint 飞请求 teardown
  - integration: 并发分发、连接断开后自动恢复

#### Sub-stage D: 收尾（并行）

- **D1: 示例程序**
  - `examples/http_client.cpp` — HTTP GET/POST 示例（回调 + 协程）
  - `examples/rpc_pool_client.cpp` — RPC 连接池调用示例

- **D2: 文档**
  - README 更新进度
  - docs/ 更新 HTTP Client 和 RPC Pool 使用文档

---

### Out of Scope

- HTTP/2 / HTTP/3
- HTTPS Client（已有 TlsContext，可独立扩展，本阶段不绑定）
- WebSocket Client（已有 WebSocketServer，Client 可独立扩展）
- 服务发现（v6-beta）
- 熔断器 / 限流（v6-gamma）
- 代理 / 隧道
- HTTP cookie / redirect / chunked encoding（v6-beta）

---

## 3. Architecture & Design

### 3.1 HttpClient

```
┌─────────────────────────────────────┐
│           HttpClient                 │
│  ┌──────────┐  ┌──────────────────┐ │
│  │Options   │  │ Status Machine   │ │
│  │(timeout, │  │ Idle→WaitResp→...│ │
│  │ keepAlive)│  └────────┬─────────┘ │
│  └──────────┘           │           │
│  ┌──────────┐           ▼           │
│  │ TcpClient│◄── TCP 连接 ──────────│
│  └──────────┘                       │
│  ┌─────────────────────────┐        │
│  │ ResponseContext (parser)│        │
│  └─────────────────────────┘        │
└─────────────────────────────────────┘
```

**请求生命周期**：
1. 用户调用 `asyncGet()` → 创建 Awaitable，注册回调
2. 构建 HTTP 请求字符串 → 通过 TcpClient::send() 发送
3. 状态切换到 `kWaitingResponse`
4. 响应数据到达 → ResponseContext 逐字节解析
5. 解析完成 → 触发回调 / 恢复协程
6. 状态切换回 `kIdle`，可发送下一请求

### 3.2 RpcConnectionPool

```
┌─────────────────────────────────────┐
│        RpcConnectionPool            │
│  ┌────────────┐  ┌───────────────┐  │
│  │ Options    │  │ round-robin   │  │
│  └────────────┘  │ selector      │  │
│                   └───────┬───────┘  │
│  ┌──────┬──────┬──────┐   │         │
│  │PoolEntry[0..N]      ├───┘         │
│  │  TcpClient+RpcChannel            │
│  └────────────────────────────────── │
└─────────────────────────────────────┘
```

**请求分发**：
1. 用户调用 `coroCall()` → 创建 Awaitable
2. `selectConnection()` 从 pool 中选一条 `inUse == false` 的连接
3. 标记 `inUse = true`，通过对应 RpcChannel 发送
4. 响应到达 → RpcChannel 触发回调 → 标记 `inUse = false`
5. 如果所有连接都在使用中 → `createOnDemand` 临时创建新连接

---

## 4. Dependencies

```
Phase 1 (并行)          Phase 2 (并行)          Phase 3 (收尾)
┌─────────┐           ┌──────────────────┐     ┌──────────┐
│ A1: Resp│─────┐     │ B1~B5: HTTP Cli│─────│ D1+D2   │
│ Parser  │     ├────▶│ (依赖 A1)       │     │ 示例+文档│
└─────────┘     │     └──────────────────┘     └──────────┘
┌─────────┐     │     ┌──────────────────┐          │
│ A2: Pool│─────┤     │ C1~C5: RPC Pool │───────────┘
│ Options │     └────▶│ (依赖 A2)       │
└─────────┘           └──────────────────┘
```

- A1 + A2 可并行（基础设施无共同依赖）
- B1~B5 依赖 A1
- C1~C5 依赖 A2
- B 与 C 可并行（HTTP 和 RPC 不共享代码）
- D 依赖 B + C 完成

---

## 5. Risk & Mitigation

| Risk | Level | Mitigation |
|---|---|---|
| HttpClient keep-alive 状态机与连接关闭交错 | 🟡 Mid | 参考 TcpClient 的 removeConnection 回调；请求在飞计数器保护 |
| RPC 池中连接重建时在飞请求的处理 | 🟡 Mid | failAllPending 在 close 回调中触发（复用 RpcChannel 已有机制） |
| 池中 round-robin 的线程安全 | 🟢 Low | 池操作限定在 owner loop 线程（无锁） |
| 与既有 HttpServer 测试的兼容性 | 🟢 Low | 不修改 HttpServer 已有接口 |
| 测试隔离（端口复用） | 🟢 Low | 随机端口 + unique name |

---

## 6. Exit Criteria

- [ ] `HttpResponseContext` 能正确解析 HTTP 响应（unit 覆盖正常/异常）
- [ ] `HttpClient` 能完成 GET/POST 完整 round-trip（integration）
- [ ] `HttpClient` keep-alive 连接能复用处理多个请求（contract）
- [ ] `HttpClient` Connection:close 后自动重建连接（contract）
- [ ] `HttpClient` 协程接口能正确返回响应或错误（contract）
- [ ] `RpcConnectionPool` 管理 N 条连接，round-robin 分发（contract）
- [ ] `RpcConnectionPool` 连接断开自动重建 + fail pending（contract）
- [ ] `RpcConnectionPool` stop() 时 fail 所有在飞请求（contract）
- [ ] 所有既有测试无 regression（unit + contract + integration）
- [ ] 示例程序可编译运行
- [ ] README / docs 与 intent 对齐

---

## 7. Workload Estimation

| Task | Code (est.) | Test (est.) | Risk |
|---|---|---|---|
| A1: HttpResponseParser | ~150 行 | ~150 行 | 🟢 Low |
| A2: RpcPoolOptions | ~50 行 | ~50 行 | 🟢 Very Low |
| B1~B4: HttpClient | ~520 行 | — | 🟡 Mid |
| B5: HTTP 测试 | — | ~400 行 | 🟡 Mid |
| C1~C4: RpcPool | ~440 行 | — | 🟡 Mid |
| C5: RPC 池测试 | — | ~350 行 | 🟡 Mid |
| D1: 示例 | ~150 行 | — | 🟢 Low |
| D2: 文档 | ~200 行 | — | 🟢 Low |
| **Total** | **~1510 行** | **~950 行** | |
