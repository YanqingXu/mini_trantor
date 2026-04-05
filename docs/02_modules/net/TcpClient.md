# TcpClient —— 工程级源码拆解

## 1. 类定位

* **角色**：客户端 TCP 连接管理器，与 TcpServer 对称
* **层级**：Net 层（用户直接使用）
* 封装 Connector + TcpConnection + DNS 解析 + 自动重连

## 2. 解决的问题

用户只需设置回调 + 调用 connect，TcpClient 自动处理：连接、重连、DNS 解析、TLS、连接生命周期。

## 3. 对外接口

| 方法 | 用途 | 线程安全 |
|------|------|----------|
| `connect()` | 发起连接 | 跨线程 |
| `disconnect()` | 断开连接 | 跨线程 |
| `stop()` | 停止连接器 | 跨线程 |
| `enableRetry()` | 启用自动重连 | 任意线程 |
| `enableSsl(ctx, hostname)` | 启用 TLS | start 前 |
| `connection()` | 获取当前连接 | 跨线程（mutex） |
| `setConnectionCallback` | 设置连接回调 | start 前 |
| `setMessageCallback` | 设置消息回调 | start 前 |

## 4. 核心成员变量

```cpp
EventLoop* loop_;
std::string name_;
std::shared_ptr<Connector> connector_;   // 可能初始为空（hostname 模式延迟创建）
bool retry_;                             // 是否自动重连
bool connect_;                           // 是否应该连接
int nextConnId_;                         // 连接 ID 递增
mutable std::mutex mutex_;               // 保护 connection_
TcpConnectionPtr connection_;            // 当前连接

// DNS 支持
std::string hostname_;
uint16_t port_;
std::shared_ptr<DnsResolver> resolver_;
std::shared_ptr<bool> resolveGuard_;     // 防止 DNS 回调访问已销毁的 TcpClient
```

## 5. 执行流程

### IP 直连模式

```
TcpClient(loop, serverAddr, name)
  → initConnector(serverAddr) → connector_ = new Connector(loop, addr)
  → connect() → connector_->start()
  → 连接成功 → newConnection(sockfd) → 创建 TcpConnection
```

### 主机名模式

```
TcpClient(loop, hostname, port, name)
  → resolver_ = DnsResolver::getShared()
  → connect() → resolveAndConnect()
      → resolver_->resolve(hostname, port, loop, callback)
          → 回调：initConnector(addr[0]) → connector_->start()
```

### 断线重连

```
连接关闭 → removeConnection(conn)
  ├─ connection_.reset()
  ├─ conn->connectDestroyed()
  └─ if (retry_ && connect_):
      ├─ connector_ 存在? → connector_->restart()
      └─ 否则 → resolveAndConnect()  // hostname 模式重新解析
```

### 析构安全

```
~TcpClient():
  ├─ *resolveGuard_ = false       // 使 DNS 回调失效
  ├─ if (connection_):
  │   conn->setCloseCallback({})  // 断开回调绑定
  │   conn->connectDestroyed()
  └─ connector_->stop()
```

## 6. 关键设计点

* **resolveGuard_**：shared_ptr<bool>，DNS 回调中检查 `*guard` 是否为 true，TcpClient 析构时设为 false
* **mutex 保护 connection_**：connection() 和 disconnect() 可能从非 loop 线程调用
* **connector 延迟创建**：hostname 模式下，DNS 解析完成后才创建 Connector

## 7. 面试角度

**Q: TcpClient 的 mutex 保护什么？**
A: 只保护 `connection_` 指针。因为 `connection()` 和 `disconnect()` 可能跨线程调用。

**Q: hostname 模式的 DNS 安全如何保证？**
A: `resolveGuard_` 是 shared_ptr<bool>，DNS 回调捕获它，TcpClient 析构时设为 false。回调中检查后才继续。
