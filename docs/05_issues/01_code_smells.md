# 代码异味分析

## 概览

以下是对 mini-trantor 代码库的代码异味分析，按严重程度排序。
代码异味不一定是 bug，但可能影响可维护性、可读性或未来扩展。

## 高关注度

### 1. TcpConnection 过度膨胀

**症状**: TcpConnection 同时承担回调模式和协程模式的所有职责

```
TcpConnection 职责:
├── 回调管理 (6 种回调设置)
├── 状态机 (4 状态)
├── 读写 I/O (handleRead/handleWrite/send/sendInLoop)
├── 关闭逻辑 (shutdown/forceClose/handleClose)
├── 3 种协程 Awaitable (内嵌类)
├── 3 种 Awaiter 状态 (readAwaiter_/writeAwaiter_/closeAwaiter_)
├── TLS 支持 (SSL 握手/读写)
├── 背压策略 (BackpressurePolicy + 状态)
├── 空闲超时 (IdleTimeoutState)
└── queueResume / resumeAllWaitersOnClose
```

**建议**: 考虑将协程 awaiter 管理、TLS 适配、背压策略抽取为独立组件

### 2. 缺少错误传播到协程

**症状**: ReadAwaitable 在连接关闭时返回空字符串，没有异常或 error_code

```cpp
string await_resume() {
    return inputBuffer_.retrieveAllAsString();  // 关闭时返回 ""
}
```

**问题**: 调用者无法区分 "读到 0 字节" 和 "连接异常关闭"

**建议**: 返回 `expected<string, error_code>` 或在异常关闭时 throw

### 3. DnsResolver::cacheEnabled_ 非 atomic

**症状**: 主线程调用 `enableCache()`，工作线程读取 `cacheEnabled_`

```cpp
bool cacheEnabled_{false};  // 主线程写，工作线程读 → 数据竞争
```

**建议**: 改为 `std::atomic<bool>`

## 中等关注度

### 4. Channel::index_ 的魔数

**症状**: 使用 -1, 1, 2 表示 Channel 状态

```cpp
static constexpr int kNew = -1;
static constexpr int kAdded = 1;
static constexpr int kDeleted = 2;
```

**建议**: 使用 enum class 代替魔数

### 5. 回调类型签名冗长

**症状**: 大量 using 声明和 std::function 签名

```cpp
using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*, Timestamp)>;
using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;
using HighWaterMarkCallback = std::function<void(const TcpConnectionPtr&, size_t)>;
using CloseCallback = std::function<void(const TcpConnectionPtr&)>;
```

**影响**: 不是严重问题，但增加了阅读噪音

### 6. setsockopt 返回值被忽略

**症状**: Socket.cc 中所有 setsockopt 调用不检查返回值

```cpp
void setSocketOption(int sockfd, int level, int option, bool on) {
    const int optval = on ? 1 : 0;
    ::setsockopt(sockfd, level, option, &optval, sizeof(optval));  // 返回值丢弃
}
```

**影响**: 设置失败时静默，可能导致调试困难

### 7. TcpServer 连接名由 InetAddress 拼接

**症状**: 连接名格式为 `"server#ip:port-ip:port"`，可能在高并发下重复

```cpp
// 如果同一 peer 快速断连重连，port 不同但名称格式类似
```

**建议**: 加入唯一递增 ID

## 低关注度

### 8. Timestamp 只是 int64_t 别名

**症状**: `using Timestamp = int64_t`，没有类型安全

```cpp
// 以下两行都合法
Timestamp ts = 42;        // 42 毫秒？微秒？秒？
Timestamp bytes = 1024;   // 不是时间戳，但编译通过
```

**建议**: 使用 `std::chrono::time_point` 或强类型包装

### 9. EventLoop 析构不 join 任何线程

**症状**: EventLoop 析构只是关闭 wakeupFd_，不等待线程退出

**影响**: EventLoopThread 负责 join，但用户如果直接创建 EventLoop 可能不安全

### 10. Buffer 无 shrink 机制

**症状**: Buffer 只会增长不会缩小

**影响**: 偶尔大消息后内存不释放

**建议**: 可选的 shrinkToFit() 或高水位后自动 shrink
