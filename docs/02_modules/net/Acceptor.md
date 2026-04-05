# Acceptor —— 工程级源码拆解

## 1. 类定位

* **角色**：监听 socket 的**薄适配层**，与 Connector 对称
* **层级**：Net 层（TcpServer 的内部组件）

## 2. 解决的问题

封装 listen + accept 循环，将已连接 fd 通过回调交付给 TcpServer。
用户不直接接触 Acceptor，它是 TcpServer 的内部实现细节。

## 3. 对外接口

| 方法 | 用途 |
|------|------|
| `setNewConnectionCallback(cb)` | 设置新连接到达的回调 |
| `listen()` | 开始监听，enableReading |
| `listening()` | 查询是否在监听 |

## 4. 核心成员变量

```cpp
EventLoop* loop_;                   // 所属 EventLoop（base loop）
Socket acceptSocket_;               // 监听 socket（值语义，拥有 fd）
Channel acceptChannel_;             // 监听 fd 的 Channel（值语义）
NewConnectionCallback newConnectionCallback_;
bool listening_;
```

## 5. 执行流程

### 构造

```
Acceptor(loop, listenAddr, reusePort):
  ├─ acceptSocket_ = createNonblockingOrDie()
  ├─ setReuseAddr(true), setReusePort(reusePort)
  ├─ bindAddress(listenAddr)
  └─ acceptChannel_.setReadCallback(handleRead)
```

### listen

```
listen():
  ├─ listening_ = true
  ├─ acceptSocket_.listen()          // ::listen(fd, SOMAXCONN)
  └─ acceptChannel_.enableReading()  // 注册 EPOLLIN
```

### handleRead（accept 循环）

```cpp
void handleRead() {
    while (true) {
        int connfd = acceptSocket_.accept(&peerAddr);
        if (connfd >= 0) {
            newConnectionCallback_(connfd, peerAddr);  // 交给 TcpServer
            continue;
        }
        if (errno == EAGAIN) break;   // 没有更多连接
        if (errno == EINTR) continue; // 被信号中断，重试
        break;
    }
}
```

**while 循环**：LT 模式下一次可能有多个连接就绪，循环 accept 直到 EAGAIN。

## 6. 关键设计点

* **值语义拥有 Socket 和 Channel**：Acceptor 不使用 unique_ptr，直接成员变量持有
* **无 tie()**：Acceptor 不需要 tie，因为它的生命周期由 TcpServer 直接管理
* **析构时检查 listening_**：如果在监听，先 disableAll + remove

## 7. 面试角度

**Q: accept 为什么用 while 循环？**
A: LT 模式下一次 EPOLLIN 可能对应多个 pending 连接。循环 accept 直到 EAGAIN，一次性处理所有。

**Q: 如果 newConnectionCallback 为空，接受的 fd 怎么办？**
A: 直接 `close(connfd)`，防止 fd 泄漏。
