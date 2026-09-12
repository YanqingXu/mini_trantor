# 极简 TCP Server 重写指南

## 1. 目标

在极简 Reactor 基础上，添加 TCP 连接管理，实现一个可运行的 echo server。
新增组件：Socket, InetAddress, Acceptor, TcpConnection, TcpServer, Buffer。

约 500 行增量代码。

## 2. 新增 API

### InetAddress（值类型，~30 行）

```cpp
class InetAddress {
public:
    explicit InetAddress(uint16_t port);
    const sockaddr_in& getSockAddr() const;
    std::string toIpPort() const;
private:
    sockaddr_in addr_{};
};
```

### Socket（RAII fd，~50 行）

```cpp
class Socket {
public:
    explicit Socket(int fd);
    ~Socket();  // close(fd)
    void bindAddress(const InetAddress& addr);
    void listen();
    int accept(InetAddress* peerAddr);
    void setReuseAddr(bool on);
private:
    int fd_;
};
```

### Buffer（读写缓冲，~80 行）

```cpp
class Buffer {
public:
    void append(const char* data, size_t len);
    size_t readableBytes() const;
    std::string retrieveAllAsString();
    ssize_t readFd(int fd, int* savedErrno);
private:
    std::vector<char> buffer_;
    size_t readerIndex_{0};
    size_t writerIndex_{0};
};
```

### Acceptor（监听器，~60 行）

```cpp
class Acceptor {
public:
    Acceptor(EventLoop* loop, const InetAddress& listenAddr);
    void setNewConnectionCallback(std::function<void(int, const InetAddress&)> cb);
    void listen();
private:
    void handleRead();  // accept loop
    Socket acceptSocket_;
    Channel acceptChannel_;
    std::function<void(int, const InetAddress&)> newConnectionCallback_;
};
```

### TcpConnection（连接，~150 行）

```cpp
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    TcpConnection(EventLoop* loop, int fd, const InetAddress& peerAddr);
    void send(const std::string& data);
    void connectEstablished();
    void setMessageCallback(std::function<void(TcpConnectionPtr, Buffer*)> cb);
    void setCloseCallback(std::function<void(TcpConnectionPtr)> cb);
private:
    void handleRead();
    void handleWrite();
    void handleClose();
    EventLoop* loop_;
    std::unique_ptr<Socket> socket_;
    std::unique_ptr<Channel> channel_;
    Buffer inputBuffer_;
    Buffer outputBuffer_;
};
```

### TcpServer（服务器，~80 行）

```cpp
class TcpServer {
public:
    TcpServer(EventLoop* loop, const InetAddress& listenAddr);
    void setMessageCallback(MessageCallback cb);
    void start();
private:
    void newConnection(int fd, const InetAddress& peerAddr);
    void removeConnection(TcpConnectionPtr conn);
    EventLoop* loop_;
    Acceptor acceptor_;
    std::map<std::string, TcpConnectionPtr> connections_;
};
```

## 3. 核心实现

### Acceptor::handleRead

```cpp
void Acceptor::handleRead() {
    InetAddress peerAddr;
    int connfd = acceptSocket_.accept(&peerAddr);
    if (connfd >= 0) {
        if (newConnectionCallback_) {
            newConnectionCallback_(connfd, peerAddr);
        } else {
            ::close(connfd);
        }
    }
}
```

### TcpConnection::handleRead

```cpp
void TcpConnection::handleRead() {
    int savedErrno = 0;
    ssize_t n = inputBuffer_.readFd(socket_->fd(), &savedErrno);
    if (n > 0) {
        messageCallback_(shared_from_this(), &inputBuffer_);
    } else if (n == 0) {
        handleClose();
    } else {
        handleClose();
    }
}
```

### TcpConnection::send

```cpp
void TcpConnection::send(const std::string& data) {
    if (loop_->isInLoopThread()) {
        sendInLoop(data);
    } else {
        loop_->runInLoop([this, data] { sendInLoop(data); });
    }
}

void TcpConnection::sendInLoop(const std::string& data) {
    ssize_t nwrote = ::write(socket_->fd(), data.data(), data.size());
    if (nwrote < static_cast<ssize_t>(data.size())) {
        outputBuffer_.append(data.data() + nwrote, data.size() - nwrote);
        channel_->enableWriting();
    }
}
```

## 4. Echo Server

```cpp
int main() {
    EventLoop loop;
    InetAddress listenAddr(9999);
    TcpServer server(&loop, listenAddr);

    server.setMessageCallback([](TcpConnectionPtr conn, Buffer* buf) {
        conn->send(buf->retrieveAllAsString());
    });

    server.start();
    loop.loop();
}
```

测试：
```bash
echo "hello" | nc localhost 9999
```

## 5. 从单线程到多线程的演进

```
单线程 echo server (本文)
  ↓ + EventLoopThread
  ↓ + EventLoopThreadPool
  ↓ + TcpServer::setThreadNum(N)
  ↓ + round-robin 连接分配
多线程 echo server
```

关键变更：
- Acceptor 在 base loop，连接在 worker loop
- TcpConnection 的回调通过 runInLoop 序列化
- removeConnection 需要两次线程跳转（worker → base → worker）

## 6. 关键学习要点

| 概念 | 实现位置 |
|------|----------|
| Non-blocking accept | Acceptor::handleRead |
| shared_ptr 生命周期 | TcpConnection + ConnectionMap |
| 输出缓冲区 | send → direct write → outputBuffer → handleWrite |
| 连接关闭 | read 返回 0 → handleClose → removeConnection |
| RAII fd 管理 | Socket 析构关闭 fd |
