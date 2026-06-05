# Socket 源码拆解

## 1. 类定位

* **角色**: 工具类（Utils）
* **层级**: Net 层基础设施
* Socket 是 **fd 级 socket 操作的 RAII 封装**——负责 listen/accept/shutdown/setsockopt 的系统调用边界

## 2. 解决的问题

* **核心问题**: 将 socket fd 的系统调用封装为 RAII 对象，确保 fd 在析构时被关闭
* 如果没有 Socket:
  - fd 的 close 容易遗忘 → fd 泄漏
  - setsockopt 调用散落在各处
  - accept/listen 的错误处理不统一

## 3. 对外接口（API）

| 方法 | 场景 | 调用者 |
|------|------|--------|
| `Socket(sockfd)` | 构造，接管 fd 所有权 | Acceptor |
| `~Socket()` | 关闭 fd | 自动 |
| `fd()` | 获取底层 fd | Channel 绑定 |
| `bindAddress(addr)` | 绑定地址 | Acceptor |
| `listen()` | 开始监听 | Acceptor |
| `accept(peerAddr)` | 接受连接 | Acceptor |
| `shutdownWrite()` | 关闭写端 | TcpConnection |
| `setReuseAddr(on)` | 设置 SO_REUSEADDR | Acceptor |
| `setReusePort(on)` | 设置 SO_REUSEPORT | Acceptor |
| `setKeepAlive(on)` | 设置 SO_KEEPALIVE | TcpConnection |
| `setTcpNoDelay(on)` | 设置 TCP_NODELAY | TcpConnection |

## 4. 核心成员变量

```
Socket
├── sockfd_: int    // socket 文件描述符（唯一所有权）
```

* 不可拷贝（noncopyable 基类）
* 析构调用 `sockets::close(sockfd_)`

## 5. 执行流程

### 5.1 Acceptor 中的典型使用

```
Acceptor 构造:
  → int fd = sockets::createNonblockingOrDie()
  → Socket acceptSocket_(fd)     // RAII 接管

Acceptor::listen():
  → acceptSocket_.setReuseAddr(true)
  → acceptSocket_.setReusePort(reusePort_)
  → acceptSocket_.bindAddress(localAddr_)
  → acceptSocket_.listen()

Acceptor::handleRead():
  → int connfd = acceptSocket_.accept(&peerAddr)
  → newConnectionCallback_(connfd, peerAddr)
```

### 5.2 accept 实现

```cpp
int Socket::accept(InetAddress* peerAddr) {
    sockaddr_in addr{};
    int connfd = sockets::accept(sockfd_, &addr);  // accept4 + SOCK_NONBLOCK
    if (connfd >= 0 && peerAddr != nullptr) {
        peerAddr->setSockAddrInet(addr);
    }
    return connfd;
}
```

### 5.3 setsockopt 统一包装

```cpp
namespace {
void setSocketOption(int sockfd, int level, int option, bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd, level, option, &optval, sizeof(optval));
}
}
```

* 所有 set 方法都委托给这个统一的辅助函数
* 简洁明了，避免重复

## 6. 关键交互关系

```
┌──────────┐  owns     ┌────────┐  delegates   ┌─────────────┐
│ Acceptor │──────────▶│ Socket │─────────────▶│ SocketsOps  │
│          │           │ (RAII) │              │ (syscalls)  │
└──────────┘           └────────┘              └─────────────┘
                           │
┌──────────────┐  owns     │
│TcpConnection │───────────┘
│              │  shutdownWrite / setKeepAlive / setTcpNoDelay
└──────────────┘
```

* **上游**: Acceptor（listening socket），TcpConnection（connected socket）
* **下游**: SocketsOps（sockets:: namespace 中的系统调用包装）

## 7. 关键设计点

* **RAII 封装**: fd 在构造时接管，析构时关闭
* **不可拷贝**: 防止 double close
* **薄包装**: 每个方法直接委托 sockets:: 命名空间函数，不添加额外逻辑
* **SO_REUSEPORT 条件编译**: 老内核可能不支持

## 8. 潜在问题

| 问题 | 描述 | 严重程度 |
|------|------|----------|
| setsockopt 无错误检查 | 返回值被忽略 | 低 |
| 无 SO_LINGER 配置 | 某些场景需要 linger 控制 | 低 |

## 9. 面试角度总结

| 问题 | 答案要点 |
|------|----------|
| Socket 类的职责？ | fd 的 RAII 管理 + 系统调用的类型安全封装 |
| 为什么用 RAII？ | 确保 fd 在异常/提前返回时也能被关闭 |
| 和 Channel 的区别？ | Socket 管理 fd 的系统调用，Channel 管理 fd 的事件注册（职责分离） |
