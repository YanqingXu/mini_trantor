# S1：DNS 生命周期执行记录

## 缓存重入与取消发布（S1-02a）

`test_dns_lifetime.cpp` 的 cache 模式在修改前被 5 秒 watchdog 终止（exit 124）：
首次解析填充缓存，第二次命中的回调调用 clearCache 时再次获取 cacheMutex，形成死锁。
cancel 模式另有明确断言失败：预先取消的 token 在 cache hit 时仍返回成功结果。

缓存结果现在在锁内复制、锁外投递；enableCache 的 TTL 写入与 worker 的 TTL 读取
共用 cacheMutex。缓存命中在 owner 线程仍可以同步回调，因此调用者必须允许重入。

取消回调保留弱操作状态。installRegistration 与 deliver 使用 request mutex，完成标记
与 registration 的移出是一个受保护的步骤；实际注销和用户回调都在解锁后执行。
已经完成的 request 不再安装迟到的 registration，不会留下 token/operation 持有环。
预先取消在查缓存之前排队返回 Cancelled。

```mermaid
sequenceDiagram
    participant Caller as resolve 调用方
    participant Request as 操作状态与 registrationMutex
    participant Loop as callback EventLoop
    Caller->>Caller: 复制 hostname 等请求数据
    Caller->>Request: 注册 weak cancellation callback
    par 注册返回后安装
        Caller->>Request: 加锁，未完成才保存 registration
    and 取消或结果到达
        Loop->>Request: 加锁，置 completed，移出 registration
        Loop->>Loop: 解锁后注销并调用用户 callback
    end
```

ResolveAwaitable 在发布前持有局部 resolver 引用，防止同步完成销毁 awaitable 时，
正在执行 resolve 的 resolver 提前释放。请求入口也先复制 hostname，不能在发布后
继续借用可能随协程 frame 一起消失的输入字符串。

## 核心变更 gate

| 问题 | 回答 |
| --- | --- |
| 哪个线程拥有？ | blocking getaddrinfo 在 worker；用户完成在 callbackLoop；缓存及 registration 各有明确的 mutex |
| 谁拥有与释放？ | resolver 拥有并 join worker；request/排队闭包共享操作状态；取消回调 weak 借用；frame 仍归 Task/Awaiter |
| 哪些回调可重入？ | cache hit 的用户 callback 可以 clearCache、enableCache、再次 resolve 或完成协程；执行时不持库内缓存/registration 锁 |
| 跨线程如何处理？ | resolve/cancel 可并发；取消经 queueInLoop 完成；install/reset 通过同一 request mutex 串行 |
| 测试在哪里？ | test_dns_lifetime 覆盖缓存重入、预取消缓存、200 个取消/注册交错和 TTL 并发更新、Task 提前销毁；原 DNS unit/contract/integration 继续运行 |

## 尚未关闭

callbackLoop 仍是裸借用指针。当前调用方必须让它活到最后一次投递；下一步建立
不拥有 loop 的安全投递句柄，再覆盖 loop 先关闭、迟到 worker 结果与取消回调。
本次不把 resolver 的 worker join 误当作 loop 中用户 callback 已经完成的屏障。

## 验证

Linux GCC ASan/UBSan 与 Clang/libc++ TSan 的 DNS unit/contract/integration 均为
4/4；Windows Release 全量 26/26。随后以每请求三方 barrier 加强取消/注册/TTL
更新交错，新增合同在 ASan、TSan、Windows 各通过 1/1。原 dns_contract 的 TSan
registration 报告在本轮通过，其他 S1 失败入口没有因此关闭。
