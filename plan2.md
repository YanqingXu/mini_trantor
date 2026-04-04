# 《mini-trantor v1-coro-preview 收敛版落地计划》

本文不是“通用 coroutine 落地大全”。
本文是基于 mini-trantor 当前仓库状态、README、AGENTS、intents、rules 的一版项目内执行稿。

目标只有一个：

在不偏离 mini-trantor 开发哲学的前提下，
把 `v1-coro-preview` 收敛为一条可验证、可评审、可持续演进的最小主线。

---

# 0. 文档定位

本计划服务于以下约束：

1. 本项目是 Intent 驱动，而不是功能堆砌项目
2. 协程接入必须是 Reactor 之上的桥接层，而不是第二套调度系统
3. 线程亲和、所有权、生命周期安全优先级高于“API 看起来更高级”
4. 所有 public contract 都必须由测试证明
5. 生命周期敏感改动必须同步更新文档与图示说明

因此，后续所有 coroutine 相关设计，都必须先回答：

1. 它属于哪个 v1 阶段？
2. 它是否会破坏 EventLoop 的 owner-thread 纪律？
3. 它是否引入隐藏 ownership 或额外 scheduler？
4. 它是否能由 unit / contract / integration 三层测试证明？
5. 它是否真的属于当前最小闭环，而不是未来能力透支？

---

# 1. 项目约束基线

## 1.1 v1 的阶段边界

mini-trantor v1 不是一次性铺开，而是按以下顺序冻结：

1. `v1-alpha`：同步 Reactor 主链路稳定
2. `v1-beta`：线程模型稳定
3. `v1-coro-preview`：协程桥接跑通

这意味着：

- 当前 coroutine 工作只能建立在 `alpha + beta` 已经成立的基础上
- coroutine 改动不能反过来重写 Reactor 主体边界
- coroutine 功能不能抢跑到 timer、独立 scheduler、复杂 cancellation 这些后续阶段

---

## 1.2 当前阶段允许的协程范围

`v1-coro-preview` 的正式范围应固定为：

- `mini::coroutine::Task`
- `TcpConnection::asyncReadSome`
- `TcpConnection::asyncWrite`
- `TcpConnection::waitClosed`
- coroutine echo 主链路

必须保证：

- awaiter 不绕开 EventLoop 调度
- resume 发生在正确的 owner loop
- close/error 路径可以安全唤醒 waiter
- coroutine 只是 Reactor bridge，不是 standalone scheduler

---

## 1.3 必须继续服从的全局原则

### 线程原则

- 一个 `EventLoop` 只属于一个线程
- loop-owned mutable state 只能在 owner thread 上改
- 跨线程交互只能走 `runInLoop` / `queueInLoop` / `wakeup`

### 所有权原则

- `EventLoop` 不被 `TcpConnection` 拥有
- `TcpConnection` 不发明模糊 ownership
- awaiter 不能靠“对象大概率还活着”这种假设工作

### 生命周期原则

- remove-before-destroy 必须保持成立
- close/error 要向同一条 teardown 路径收敛
- waiter 恢复不能跑到失效对象之上

### 测试原则

- unit test 验局部逻辑
- contract test 验 API、线程、生命周期契约
- integration test 验端到端主链路

---

# 2. 当前仓库基线

当前仓库已经不是“从零设计 coroutine”状态，而是已经具备一版最小实现：

## 2.1 已有协程基础

- `mini/coroutine/Task.h`
- `tests/unit/coroutine/test_task.cpp`

说明：

- 已有 `Task<T>` / `Task<void>`
- 已有 lazy start
- 已有 continuation 恢复
- 已有 `detach()`
- 已有最基础链式 co_await 验证

---

## 2.2 已有连接 awaitable

- `mini/net/TcpConnection.h`
- `mini/net/TcpConnection.cc`

说明：

- 已有 `asyncReadSome`
- 已有 `asyncWrite`
- 已有 `waitClosed`
- 已有按连接保存 waiter 状态的最小实现
- 已有通过 `loop_->queueInLoop()` 恢复协程的路径

---

## 2.3 已有验证主线

- `tests/contract/tcp_connection/test_tcp_connection.cpp`
- `tests/integration/coroutine/test_coroutine_echo_server.cpp`
- `examples/coroutine_echo_server/main.cpp`

说明：

- coroutine echo 主链路已经跑通
- 已经验证 resume 发生在 worker loop，而不是 base loop
- 但 contract 仍然偏薄，离“阶段冻结”还差一层系统化收口

---

# 3. 这次 plan2 的收敛原则

旧版 plan2 的问题不是方向完全错误，
而是它混入了太多“通用 coroutine 框架建设”的内容，
容易让本项目在 `v1-coro-preview` 阶段提前膨胀。

因此新 plan2 采用下面的收敛规则：

## 3.1 保留最小主线

只保留与当前阶段直接相关的三段链路：

1. `Task`
2. `TcpConnection awaitables`
3. coroutine echo end-to-end path

---

## 3.2 不提前建设第二套系统

当前不建设：

- standalone coroutine scheduler
- 通用 loop scheduler 抽象
- timer-based sleep system
- Accept/Connect awaiter 全家桶
- complex cancellation tree
- async mutex / async channel / RPC coroutine framework

原因不是它们永远不做，
而是它们不属于当前阶段闭环。

---

## 3.3 所有 coroutine 设计都必须回到 Reactor 语义

协程只是写法变化，不是语义变化。

不能出现以下倾向：

- 为了 co_await 方便而绕过 `EventLoop`
- 在 Poller 回调里直接做复杂协程调度决策
- 让 `TcpConnection` 成为“半连接管理器 + 半 coroutine runtime”
- 用共享状态和额外锁补偿不清晰的线程模型

---

# 4. v1-coro-preview 的正式范围定义

## 4.1 In Scope

### A. `Task` 最小契约冻结

`Task` 在本阶段只负责：

- coroutine return object
- continuation 组合
- lazy start
- result / exception 传播
- detach 语义

它不负责：

- 线程调度
- loop 选择
- timer 驱动
- cancellation tree
- future/promise 泛化

---

### B. `TcpConnection` 协程桥冻结

`TcpConnection` 在 coroutine 方向只暴露：

- `asyncReadSome`
- `asyncWrite`
- `waitClosed`

它们的职责是：

- 把连接读写关闭事件转换成 awaitable suspend/resume
- 保证 resume 通过 EventLoop 回到正确线程
- 保证 close/error 下 waiter 能安全收敛

它们不负责：

- 独立异步 socket API 家族
- 跨连接调度
- 协议级 session 管理

---

### C. coroutine echo 主链路冻结

最终要稳定证明的不是“协程功能很多”，而是：

```cpp
mini::coroutine::Task<void> echoSession(mini::net::TcpConnectionPtr connection) {
    while (connection->connected()) {
        std::string message = co_await connection->asyncReadSome();
        if (message.empty()) {
            break;
        }
        co_await connection->asyncWrite(std::move(message));
    }
}
```

这条链必须在以下层面被证明：

- example 可读
- integration 可跑
- thread context 可验证
- close 路径可收敛

---

## 4.2 Out of Scope

在 `v1-coro-preview` 关闭之前，以下内容明确不进入主线：

- `ResumeOnLoop`
- `LoopScheduler`
- `SleepAwaiter`
- `TimerQueue`
- async timer API
- `AcceptAwaiter`
- `ConnectAwaiter`
- `Connector::asyncConnect`
- `Acceptor::asyncAccept`
- `CancellationToken`
- `DetachedTask` 专门框架
- coroutine 专用任务拥有者容器体系
- `Expected` 风格大改
- include/src 目录结构大搬迁
- protocol stack / RPC / Lua coroutine
- backpressure policy 扩展

若要做这些能力，必须等本阶段收口后再开新意图文档与新计划。

---

# 5. 核心契约定义

## 5.1 `Task` 契约

### Intent

`Task` 是最小可组合 coroutine 结果对象，不替代 EventLoop 调度语义。

### 必须保持的语义

1. 默认 lazy：创建后不自动执行
2. `start()` 只推进当前 coroutine 本身
3. `co_await Task<T>` 时，子协程结束后恢复 continuation
4. 异常通过 promise 保存，并在取结果时抛出
5. `detach()` 允许 fire-and-forget，但不改变 EventLoop 纪律

### 当前阶段不扩展的语义

- 不引入 executor 参数
- 不引入 scheduler 绑定
- 不引入 `sync_wait` 生产 API
- 不引入 cancellation 协议

### 对应测试

- `tests/unit/coroutine/test_task.cpp`

后续应补的 unit contract：

- exception propagation
- move-only correctness
- `detach()` 完成后 frame 正常回收
- nested await 链路稳定

---

## 5.2 `TcpConnection` coroutine bridge 契约

### Intent

把连接级读、写、关闭事件转成 awaitable，
但不改变连接仍然由 owner `EventLoop` 管理这一事实。

### 必须保持的语义

1. `asyncReadSome` 只在连接可读或连接关闭后恢复
2. `asyncWrite` 完成条件是数据已写完或连接进入不可继续状态
3. `waitClosed` 在 close 路径收敛后恢复
4. resume 必须通过 `EventLoop` 回到 owner thread
5. close/error 必须统一唤醒相关 waiter

### 并发约束

当前阶段显式允许最小模型：

- 一个连接同时最多一个 read waiter
- 一个连接同时最多一个 write waiter
- 一个连接同时最多一个 close waiter

如果用户违反此约束：

- 应明确抛错或显式拒绝
- 不允许沉默覆盖旧 waiter

### 回调与协程共存原则

- coroutine waiter 与 callback 模式必须有明确分发边界
- 不能因为引入 awaiter 而破坏既有 callback 主线
- message callback 不应在 waiter 已接管读取语义时出现语义冲突

### 对应测试

- `tests/contract/tcp_connection/test_tcp_connection.cpp`
- `tests/integration/coroutine/test_coroutine_echo_server.cpp`

后续应补的 contract：

- read waiter 在 close 时被唤醒
- write waiter 在 close/error 时被唤醒
- double waiter 被拒绝
- cross-thread 发起 await arm 后仍在 owner loop 恢复
- repeated close/error 不产生 stale waiter

---

## 5.3 线程契约

所有 coroutine 相关改动必须继续满足：

1. loop-owned mutable state 只在 owner thread 上修改
2. waiter arm 如来自非 owner thread，必须 marshal 回 loop
3. waiter resume 不能在任意 caller thread 直接发生
4. Poller / Channel 注册路径不能被 coroutine 绕开

一句话：

协程恢复是 EventLoop 里的一个任务，不是外部线程对 coroutine frame 的任意 resume。

---

## 5.4 生命周期契约

所有 coroutine 相关改动必须继续满足：

1. `TcpConnection` close/error 向一条 teardown 路径收敛
2. `Channel` remove-before-destroy 不被 coroutine 逻辑破坏
3. waiter 不得持有导致循环 ownership 的隐藏结构
4. 已经进入 teardown 的对象不得再产生失控 resume

---

# 6. 分阶段落地顺序

## Phase 0：冻结设计入口

目标：

先把“当前到底做什么、不做什么”写清楚，避免继续扩张。

产出：

- 本 `plan2.md`
- 如有必要，补一份 coroutine bridge intent
- 为 `TcpConnection` coroutine path 补状态/时序图说明

退出条件：

- 所有人对 `v1-coro-preview` 范围一致
- 后续 PR 不再把 timer/scheduler/connect/accept 混入当前阶段

---

## Phase 1：冻结 `Task` 契约

目标：

把 `Task` 从“能跑”提升到“边界清楚、测试完整”。

工作项：

1. 审视 `Task` 当前 promise/final_suspend/detach 语义
2. 补充 unit tests
3. 明确哪些行为是 API contract，哪些只是当前实现细节

重点检查：

- continuation 是否稳定恢复
- `detach()` 是否安全
- 异常传播是否明确
- move 后对象是否保持可推理状态

退出条件：

- `tests/unit/coroutine/test_task.cpp` 覆盖最小 contract
- `Task` 不再承载 scheduler ambitions

---

## Phase 2：冻结 `TcpConnection` awaiter 契约

目标：

把当前 awaitable 从“示例可用”提升到“连接语义清晰”。

工作项：

1. 审视 read/write/close waiter 状态模型
2. 明确 callback 与 waiter 的协作边界
3. 补全 contract tests
4. 校验 close/error/repeated teardown 行为

重点检查：

- 是否存在 waiter 被覆盖
- 是否存在 close 后漏 resume
- 是否存在错误线程 resume
- 是否存在读取路径和 message callback 冲突

退出条件：

- `tests/contract/tcp_connection/test_tcp_connection.cpp` 增强
- awaiter 的失败路径可复现、可验证

---

## Phase 3：冻结 coroutine echo 主链路

目标：

把 coroutine 使用方式稳定成项目推荐范式。

工作项：

1. 固定 `examples/coroutine_echo_server/main.cpp` 的推荐写法
2. 保持 integration test 可跑
3. 验证 worker loop 恢复线程语义
4. 验证连接关闭后的 session 收敛

退出条件：

- `tests/integration/coroutine/test_coroutine_echo_server.cpp` 稳定通过
- example 与 integration 的写法一致

---

## Phase 4：补文档与评审模板

目标：

让后续 coroutine 改动进入可持续维护状态。

产出：

- coroutine bridge 的 review checklist
- `TcpConnection` coroutine path 的序列图
- change gate 模板示例

退出条件：

- 后续变更可以直接套用，不再靠口头约定

---

# 7. 当前阶段的推荐文件边界

为避免“为了抽象而抽象”，当前阶段尽量维持现有仓库布局：

## 7.1 允许继续演进的核心文件

- `mini/coroutine/Task.h`
- `mini/net/TcpConnection.h`
- `mini/net/TcpConnection.cc`
- `tests/unit/coroutine/test_task.cpp`
- `tests/contract/tcp_connection/test_tcp_connection.cpp`
- `tests/integration/coroutine/test_coroutine_echo_server.cpp`
- `examples/coroutine_echo_server/main.cpp`

---

## 7.2 当前阶段不建议新增的大块文件

当前不要为了“看起来完整”而新增：

- `mini/coroutine/Scheduler.h`
- `mini/coroutine/SleepAwaiter.h`
- `mini/coroutine/IoAwaiter.h`
- `mini/coroutine/Cancellation.h`
- `mini/net/Connector.*`
- `mini/net/TimerQueue.*`

如果未来真的进入新阶段，应先补 intent，再新增文件。

---

# 8. 测试收口计划

## 8.1 Unit

文件：

- `tests/unit/coroutine/test_task.cpp`

应覆盖：

- lazy start
- nested co_await
- void/int 返回
- exception propagation
- detach completion

---

## 8.2 Contract

文件：

- `tests/contract/tcp_connection/test_tcp_connection.cpp`

应覆盖：

- owner loop 上恢复
- close/error 唤醒 waiter
- double read waiter 被拒绝
- double write waiter 被拒绝
- repeated teardown 不遗留 waiter 状态
- callback/coroutine 共存边界

---

## 8.3 Integration

文件：

- `tests/integration/coroutine/test_coroutine_echo_server.cpp`

应覆盖：

- coroutine echo 主路径
- worker loop 恢复线程验证
- client close 后 session 收敛
- thread pool 模式下行为不偏离 Reactor 语义

---

# 9. 变更门禁模板

以后只要改 coroutine 相关核心模块，提交说明必须回答：

## 9.1 所属阶段

- 本改动属于 `v1-coro-preview`
- 不引入 `post-v1-coro-preview` 范围能力

## 9.2 五个核心问题

1. 哪个 loop/thread 拥有这段状态？
2. 谁拥有它，谁释放它？
3. 哪些回调或 resume 点可能重入？
4. 哪些操作允许跨线程，如何 marshal？
5. 哪个具体测试文件验证？

## 9.3 示例回答模板

- Owner loop/thread：连接所属 `EventLoop`
- Ownership：`TcpConnection` 由 shared ownership 协调；awaiter 仅观察/挂接 resume 点，不转移 loop ownership
- Re-entry points：`handleRead`、`handleWrite`、`handleClose`、`handleError`、waiter resume
- Cross-thread operations：仅允许通过 `runInLoop` / `queueInLoop` arm 或恢复
- Tests：填写具体测试文件路径，不能只写“covered by tests”

---

# 10. 明确延期的后续方向

下面这些方向不是被否定，而是明确延期：

## 10.1 `post-v1-coro-preview` 候选主题

- `TimerQueue`
- `SleepAwaiter`
- `AcceptAwaiter`
- `ConnectAwaiter`
- `Connector`
- cancellation model
- backpressure policy

---

## 10.2 开启这些主题前必须先做的事

1. 新建对应 intent
2. 重新定义 responsibilities / non-responsibilities
3. 更新 threading / ownership / lifecycle 规则
4. 明确 contract tests
5. 再写实现

---

# 11. 最终结论

mini-trantor 当前阶段不需要一套“完整 coroutine 框架”。

它需要的是一条被明确约束的最小闭环：

1. `Task` 只负责协程组合
2. `TcpConnection` 只负责把连接事件桥接成 awaitable
3. `EventLoop` 继续是唯一线程调度核心
4. coroutine echo 作为 `v1-coro-preview` 的阶段证明
5. 所有未来能力在本阶段闭环之后再开启

用一句话概括本计划：

不是“把 coroutine 体系做大”，
而是“把 coroutine bridge 做对，然后冻结边界”。
