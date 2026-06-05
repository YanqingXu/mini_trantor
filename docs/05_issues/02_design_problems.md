# 设计问题分析

## 概览

以下是对 mini-trantor 架构层面的设计问题分析，重点关注可扩展性、一致性和抽象质量。

## 1. 无通用取消机制

**问题**: 协程层缺少通用的 cancellation token / cancellation source

**现状**:
- SleepAwaitable 有 cancel() 方法
- ReadAwaitable / WriteAwaitable / CloseAwaitable 无法取消
- WhenAny 的败者无法被取消

**影响**:
```
co_await whenAny(
    conn->asyncReadSome(),    // 如果 sleep 先赢，这个 read 继续等待
    asyncSleep(loop, 5s)      // sleep 可以被取消，但 read 不行
);
```

**建议**: 引入 CancellationToken + CancellationSource，所有 Awaitable 在 await_suspend 时检查 token

## 2. ~~错误模型不统一~~ ✅ 已解决（协程层）

**原问题**: 不同模块的错误处理方式不一致

**已完成的修复**:
- 新增 `mini/net/NetError.h`：定义 `enum class NetError { PeerClosed, ConnectionReset, NotConnected }` + `Expected<T>` 别名（基于 C++23 `std::expected<T, NetError>`）
- `ReadAwaitable::await_resume()` 返回 `Expected<std::string>`
- `WriteAwaitable::await_resume()` 返回 `Expected<void>`
- 所有协程调用方已更新为 `if (!result)` 模式处理错误

**剩余工作**: 回调层错误模型（DnsResolver、Buffer::readFd、Socket ops）尚未统一，可在后续版本中继续改进

| 模块 | 错误处理 | 状态 |
|------|----------|------|
| TlsContext | throw runtime_error | 未变 |
| DnsResolver | 回调传空 vector | 未变 |
| TcpConnection read (coroutine) | `Expected<std::string>` | ✅ 已统一 |
| TcpConnection write (coroutine) | `Expected<void>` | ✅ 已统一 |
| Buffer::readFd | 返回 -1 + savedErrno | 未变 |
| Socket ops | sockets::xxxOrDie → abort | 未变 |

## 3. 仅支持 IPv4

**问题**: InetAddress 只封装 sockaddr_in

**影响**: 无法在 IPv6 环境使用

**建议**: 
```cpp
class InetAddress {
    union {
        sockaddr_in addr4_;
        sockaddr_in6 addr6_;
    };
    sa_family_t family_;
};
```

## 4. 单 Poller 实现绑定

**问题**: 虽然有 Poller 抽象基类，但只有 EPollPoller 实现

**影响**: 
- 不能在非 Linux 平台使用
- 测试中无法替换为 mock poller

**建议**: v1 可接受（仅面向 Linux），后续考虑添加 PollPoller 或 KqueuePoller

## 5. 背压策略与连接耦合

**问题**: BackpressurePolicy 直接嵌入 TcpConnection

```cpp
struct BackpressurePolicy {
    bool enabled{false};
    size_t highWaterMark{64 * 1024 * 1024};
    size_t lowWaterMark{0};
};
```

**影响**: 不能针对不同类别的连接使用不同策略（如 control channel vs data channel）

**建议**: 将 BackpressurePolicy 作为独立策略对象，通过接口注入

## 6. HTTP 和 WebSocket 与 Net 层的耦合

**问题**: HttpServer 直接操作 TcpConnection 和 Buffer

**影响**: 难以在不同传输层（如 QUIC）上复用 HTTP/WS 逻辑

**建议**: 引入 ProtocolCodec 抽象层，HTTP/WS 只操作 codec 接口

## 7. TimerQueue::timers_ 的遍历效率

**问题**: 使用 `std::multimap<TimePoint, shared_ptr<Timer>>` 存储定时器

**影响**: 
- 每次 getExpired 需要 lower_bound + 逐个 erase
- 与 min-heap 相比，插入 O(log n) 相同，但缓存局部性差

**替代**: 
```
方案 A (当前): multimap → O(log n) 插入, O(k) 取出
方案 B: 4-ary heap → O(log n) 插入, O(k log n) 取出，但缓存友好
方案 C: timing wheel → O(1) 插入, O(1) 取出（精度有限）
```

v1 使用 multimap 是合理选择（简单正确），大量定时器场景再优化。

## 8. 无连接池 / 对象池

**问题**: 每个新连接都 new TcpConnection + new Channel + new Socket

**影响**: 高频短连接场景下有内存分配压力

**建议**: v1 可接受，后续考虑对象池复用 TcpConnection 资源

## 9. ~~日志系统缺失~~ ✅ 已解决

**原问题**: 整个项目只使用 `fprintf(stderr, ...)` 和 `assert()`

**已完成的修复**:
- 新增 `mini/base/Logger.h` + `mini/base/Logger.cc`：自实现轻量级日志系统
- 6 个日志级别：TRACE / DEBUG / INFO / WARN / ERROR / FATAL
- 流式 API：`LOG_INFO << "message: " << value`
- 自动记录文件名和行号
- FATAL 级别自动调用 `std::abort()`
- 可配置输出函数和 flush 函数（`setOutputFunction()` / `setFlushFunction()`）
- 默认输出到工作目录下的 `log` 文件（append 模式）
- 全项目 25 处 fprintf/fputs/perror 已全部替换为 LOG_* 宏

**剩余工作**: 异步日志缓冲（AsyncLogger）可在后续版本中添加

## 10. 配置硬编码

**问题**: 关键参数硬编码在源码中

```
- DnsResolver 工作线程数: 2
- Connector 重试延迟: 500ms 初始, 30s 上限
- EPollPoller 初始 events 大小: 16
- Buffer kCheapPrepend: 8
- Channel poll timeout: 10000ms
```

**建议**: 提供 Config 结构体或 builder pattern 配置
