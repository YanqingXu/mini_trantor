下面我给你一份偏“工程落地”的《**mini-trantor v1 完整实现蓝图**》。目标不是做一个玩具 demo，而是做一个**可持续演进到 mini-drogon 网络底座**的版本。

我会按你指定的五个核心块展开，并把它们串成一条实现路径：

1. Channel + Poller 完整实现
2. TcpConnection 生命周期设计
3. Buffer（含零拷贝优化思路）
4. 多线程 EventLoop（one loop per thread）
5. coroutine 接入点设计（重点）

最后我会给你：

* 推荐目录结构
* 类图/流程图
* v1 开发顺序
* 每阶段验收标准
* 明确哪些先不做，避免失控

---

# 一、mini-trantor v1 的目标边界

先把边界钉死，不然后面会越做越散。

## 1. v1 必须实现的能力

### 网络核心

* 基于 `epoll` 的 Reactor
* `EventLoop` 驱动 IO 事件
* `Channel` 封装 fd + 关注事件 + 回调
* `Poller` 封装 epoll 的增删改查
* `Acceptor` 接受新连接
* `TcpServer` 管理监听与连接分发
* `TcpConnection` 表示单个 TCP 连接
* `Buffer` 负责读写缓冲
* `EventLoopThread` / `EventLoopThreadPool` 支持 one loop per thread

### 回调模型

* connection callback
* message callback
* write complete callback
* close callback
* high water mark callback（可后置到 v1.1）

### coroutine 接入

* 给 `TcpConnection` 提供 coroutine-friendly 的 await 接口
* 支持“读一条消息 / 写一段数据 / 等待连接关闭”这类基础 await
* 保持 Reactor 为底座，协程只是上层语法糖，不破坏事件驱动模型

---

## 2. v1 暂时不要做的能力

这些很诱人，但我建议全部延后：

* SSL/TLS
* 定时器系统（可先只做 runInLoop/queueInLoop）
* 完整 HTTP/WebSocket
* 真正跨平台 Poller（先只做 Linux epoll）
* 内存池
* 完整无锁队列
* 完整 backpressure 策略
* 半关闭复杂状态机（先做基础 close/shutdown）
* coroutine scheduler 独立调度器

一句话：
**v1 的本质是：把 trantor 的“Reactor + Connection + ThreadPool + Coroutine Hook”最小闭环做出来。**

---

# 二、整体架构蓝图

---

## 1. 核心模块关系

```text
TcpServer
 ├── Acceptor
 │    └── Channel(listenfd)
 ├── EventLoop(mainLoop)
 ├── EventLoopThreadPool
 │    ├── EventLoopThread
 │    ├── EventLoop(subLoop1)
 │    ├── EventLoop(subLoop2)
 │    └── ...
 └── connections: map<connName, TcpConnectionPtr>

TcpConnection
 ├── EventLoop* ownerLoop
 ├── Socket
 ├── Channel(sockfd)
 ├── Buffer inputBuffer
 ├── Buffer outputBuffer
 ├── callbacks...
 └── coroutine waiters...
```

---

## 2. 线程模型

采用经典的：

* **mainLoop**：只负责监听 socket 的 accept
* **subLoop**：负责已建立连接的 IO 读写事件
* **one loop per thread**：

  * 一个线程一个 `EventLoop`
  * 一个连接只归属于一个 `EventLoop`
  * 连接上的所有 IO 操作都必须在所属 loop 线程中执行

这非常关键，因为这决定了：

* `TcpConnection` 内部大多数逻辑可以假设线程亲和
* 减少锁
* coroutine 恢复时知道恢复到哪个 loop

---

## 3. 数据流

```text
客户端 -> listenfd
        -> Acceptor::handleRead()
        -> TcpServer::newConnection(sockfd)
        -> 选择 subLoop
        -> 在 subLoop 中创建 TcpConnection
        -> Channel 注册读事件

客户端发数据
        -> epoll_wait 返回可读
        -> Channel::handleEvent()
        -> TcpConnection::handleRead()
        -> inputBuffer 追加数据
        -> messageCallback(conn, &inputBuffer)

业务要发送
        -> TcpConnection::send(data)
        -> 如果当前可直接写，先写
        -> 未写完部分进入 outputBuffer
        -> Channel 打开 EPOLLOUT
        -> handleWrite() 持续发送
        -> 发完后关闭 EPOLLOUT
```

---

# 三、模块一：Channel + Poller 完整实现

这是整个 Reactor 的骨架。

---

## 1. Channel 的职责定位

`Channel` 不是 socket，也不是连接。
它只是：

> **一个 fd 在 EventLoop 中的“事件订阅实体”**

它保存：

* fd
* 关注的事件 `events_`
* 实际返回的事件 `revents_`
* 各类回调（读 / 写 / 关闭 / 错误）
* 当前是否已注册到 Poller

---

## 2. Channel 建议字段

```cpp
class Channel : noncopyable {
public:
    using EventCallback = std::function<void()>;

    Channel(EventLoop* loop, int fd);
    ~Channel();

    void handleEvent();
    void handleEventWithGuard();

    void setReadCallback(EventCallback cb);
    void setWriteCallback(EventCallback cb);
    void setCloseCallback(EventCallback cb);
    void setErrorCallback(EventCallback cb);

    int fd() const;
    uint32_t events() const;
    void set_revents(uint32_t revt);

    bool isNoneEvent() const;
    bool isWriting() const;
    bool isReading() const;

    void enableReading();
    void disableReading();
    void enableWriting();
    void disableWriting();
    void disableAll();

    int index() const;
    void set_index(int idx);

    EventLoop* ownerLoop();
    void remove();

private:
    void update();

private:
    static constexpr uint32_t kNoneEvent = 0;
    static constexpr uint32_t kReadEvent = EPOLLIN | EPOLLPRI;
    static constexpr uint32_t kWriteEvent = EPOLLOUT;

    EventLoop* loop_;
    const int fd_;
    uint32_t events_;
    uint32_t revents_;
    int index_;   // new / added / deleted

    bool eventHandling_;
    bool addedToLoop_;

    EventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};
```

---

## 3. Channel 关键语义

### enableReading / enableWriting

本质是：

* 修改 `events_`
* 调用 `update()`
* 通知 `Poller::updateChannel(this)`

### handleEvent

要按事件类型拆分：

* `EPOLLHUP` 且不是 `EPOLLIN`：close
* `EPOLLERR`：error
* `EPOLLIN | EPOLLPRI | EPOLLRDHUP`：read
* `EPOLLOUT`：write

推荐顺序：

```cpp
if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) closeCallback_();
if (revents_ & EPOLLERR) errorCallback_();
if (revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) readCallback_();
if (revents_ & EPOLLOUT) writeCallback_();
```

---

## 4. Poller 抽象层设计

虽然 v1 只做 epoll，但仍建议保留抽象基类。

```cpp
class Poller : noncopyable {
public:
    using ChannelList = std::vector<Channel*>;

    explicit Poller(EventLoop* loop);
    virtual ~Poller();

    virtual Timestamp poll(int timeoutMs, ChannelList* activeChannels) = 0;
    virtual void updateChannel(Channel* channel) = 0;
    virtual void removeChannel(Channel* channel) = 0;

    bool hasChannel(Channel* channel) const;

    static std::unique_ptr<Poller> newDefaultPoller(EventLoop* loop);

protected:
    using ChannelMap = std::unordered_map<int, Channel*>;
    ChannelMap channels_;

private:
    EventLoop* ownerLoop_;
};
```

---

## 5. EPollPoller 实现重点

### 核心成员

```cpp
class EPollPoller : public Poller {
private:
    static constexpr int kInitEventListSize = 16;

    int epollfd_;
    std::vector<epoll_event> events_;
};
```

### 必备方法

* `poll()`
* `updateChannel()`
* `removeChannel()`
* `fillActiveChannels()`
* `update(operation, channel)`

---

## 6. Channel 在 Poller 中的状态

建议用三态：

```cpp
kNew = -1
kAdded = 1
kDeleted = 2
```

### 状态机

* `kNew`：从未加入 epoll
* `kAdded`：已在 epoll 中
* `kDeleted`：从 epoll 中删除但仍在 channels_ 映射中

### updateChannel 逻辑

* `kNew / kDeleted` + 有事件 => `EPOLL_CTL_ADD`
* `kAdded` + 无事件 => `EPOLL_CTL_DEL`
* `kAdded` + 有事件变化 => `EPOLL_CTL_MOD`

这是 Reactor 的核心稳定点之一。

---

## 7. EventLoop 与 Poller 的关系

`EventLoop` 持有一个 `unique_ptr<Poller>`，每次 loop：

```cpp
while (!quit_) {
    activeChannels_.clear();
    pollReturnTime_ = poller_->poll(timeoutMs, &activeChannels_);
    eventHandling_ = true;
    for (Channel* ch : activeChannels_) {
        currentActiveChannel_ = ch;
        ch->handleEvent();
    }
    currentActiveChannel_ = nullptr;
    eventHandling_ = false;
    doPendingFunctors();
}
```

---

## 8. Channel + Poller 的 v1 验收标准

你至少要验证这些：

* 一个 fd 可注册读事件
* 可修改为读+写
* 可取消写事件
* 可 remove channel
* 多个 fd 同时活跃时，activeChannels 正确填充
* 对端关闭时，close/read 路径能触发
* 不会重复 add 同一 fd 导致 epoll_ctl 报错

---

# 四、模块二：TcpConnection 生命周期设计

这一块决定整个库是否“像一个真正的网络库”。

---

## 1. TcpConnection 的定位

`TcpConnection` 是：

> **一个已建立 TCP 连接的面向对象封装，负责 socket 生命周期、读写缓冲、事件处理、状态流转、用户回调触发。**

它通常由 `shared_ptr<TcpConnection>` 管理，因为回调链条很多，裸指针非常危险。

---

## 2. TcpConnection 状态机

建议最少 4 态：

```cpp
enum StateE {
    kConnecting,
    kConnected,
    kDisconnecting,
    kDisconnected
};
```

### 状态语义

* `kConnecting`：对象刚创建，还未正式建立“可用连接”
* `kConnected`：正常通信中
* `kDisconnecting`：用户请求优雅关闭，等待 outputBuffer 发完
* `kDisconnected`：已关闭，不再处理任何 IO

---

## 3. TcpConnection 核心成员

```cpp
class TcpConnection : public std::enable_shared_from_this<TcpConnection>,
                      noncopyable {
public:
    using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

private:
    EventLoop* loop_;
    std::string name_;
    StateE state_;

    std::unique_ptr<Socket> socket_;
    std::unique_ptr<Channel> channel_;

    InetAddress localAddr_;
    InetAddress peerAddr_;

    Buffer inputBuffer_;
    Buffer outputBuffer_;

    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    CloseCallback closeCallback_;

    bool reading_;
};
```

---

## 4. 生命周期全流程

---

### 阶段 A：连接建立

由 `Acceptor` accept 到 `sockfd` 后：

1. `TcpServer::newConnection(sockfd, peerAddr)`
2. 选择一个 `subLoop`
3. 创建 `TcpConnectionPtr`
4. 设置各类 callback
5. 在目标 `subLoop` 中执行 `connectEstablished()`

---

### 阶段 B：connectEstablished

```cpp
void TcpConnection::connectEstablished() {
    loop_->assertInLoopThread();
    assert(state_ == kConnecting);
    setState(kConnected);
    channel_->enableReading();
    connectionCallback_(shared_from_this());
}
```

关键点：

* 必须在所属 loop 线程中做
* 状态切到 `kConnected`
* 打开读事件
* 通知上层“连接已建立”

---

### 阶段 C：正常收发

#### 可读

`handleRead()`：

* 从 fd 读入 `inputBuffer_`
* 读到数据 => `messageCallback_`
* 返回 0 => 对端关闭 => `handleClose()`
* 返回 <0 => `handleError()`

#### 可写

`handleWrite()`：

* 从 `outputBuffer_` 向 fd 写
* 写完则关闭写关注
* 若 `kDisconnecting` 且 outputBuffer 空，则执行 shutdown

---

### 阶段 D：用户发起关闭

`shutdown()`：

* 若 `kConnected` -> `kDisconnecting`
* 若当前不在写，则立即 `shutdownInLoop()`
* 否则等 `handleWrite()` 把数据发完再 shutdown

这样可以保证“优雅关闭”。

---

### 阶段 E：对端关闭 / 异常关闭

最终都会走 `handleClose()`：

```cpp
void TcpConnection::handleClose() {
    loop_->assertInLoopThread();
    setState(kDisconnected);
    channel_->disableAll();

    TcpConnectionPtr guardThis(shared_from_this());
    connectionCallback_(guardThis);
    closeCallback_(guardThis);
}
```

其中：

* `connectionCallback_` 可用于通知上层连接状态变化
* `closeCallback_` 一般由 `TcpServer` 绑定，用来从连接表移除该连接

---

### 阶段 F：connectDestroyed

通常由 `TcpServer::removeConnectionInLoop()` 调用：

```cpp
void TcpConnection::connectDestroyed() {
    loop_->assertInLoopThread();
    if (state_ == kConnected) {
        setState(kDisconnected);
        channel_->disableAll();
        connectionCallback_(shared_from_this());
    }
    channel_->remove();
}
```

这是“彻底从 loop 和 poller 中移除”。

---

## 5. TcpServer 与 TcpConnection 的生命周期协作

### TcpServer 持有

```cpp
std::unordered_map<std::string, TcpConnectionPtr> connections_;
```

### 关闭流程

* `TcpConnection` 检测到 close
* 调用 `closeCallback_`
* `TcpServer::removeConnection(conn)`
* 在 mainLoop 或 ownerLoop 中执行移除
* 从 `connections_` 擦除
* 调 `connectDestroyed()`

---

## 6. 线程亲和原则

非常重要：

* `TcpConnection` 属于哪个 `EventLoop`，它的 IO 处理就必须在那个线程
* `send()` 可以跨线程调用，但最终要转发到 `sendInLoop()`
* `shutdown()` 可以跨线程调用，但最终要转发到 `shutdownInLoop()`

这决定了接口设计：

```cpp
void TcpConnection::send(const std::string& msg) {
    if (state_ == kConnected) {
        if (loop_->isInLoopThread()) {
            sendInLoop(msg.data(), msg.size());
        } else {
            auto self = shared_from_this();
            loop_->runInLoop([self, msg]{
                self->sendInLoop(msg.data(), msg.size());
            });
        }
    }
}
```

---

## 7. TcpConnection v1 验收标准

* 建立连接后能回调 `connectionCallback`
* 收到消息能进 `messageCallback`
* `send()` 可跨线程安全调用
* outputBuffer 未清空时能自动打开/关闭写事件
* 对端关闭能正确触发 close 路径
* 自己调用 shutdown 后能优雅关闭
* 从 `TcpServer::connections_` 中移除无泄漏

---

# 五、模块三：Buffer（零拷贝优化）

这一块你点得非常对。
如果你未来要走高并发，这里绝不能只做一个“简单 string 拼接器”。

---

## 1. Buffer 的职责

`Buffer` 不是消息对象。它是：

> **面向 socket IO 的字节缓冲区，负责高效追加、读取、查找、回收空间。**

典型场景：

* 从 fd 读数据进 inputBuffer
* 从 outputBuffer 写数据到 fd
* 上层协议从 inputBuffer 中取完整包
* 未发完数据继续留在 outputBuffer

---

## 2. 推荐的 Buffer 结构

采用 trantor/muduo 风格的：

```text
+-------------------+------------------+------------------+
| prependable bytes |   readable bytes |  writable bytes  |
+-------------------+------------------+------------------+
0                readerIndex       writerIndex        size
```

### 成员

```cpp
class Buffer {
public:
    static constexpr size_t kCheapPrepend = 8;
    static constexpr size_t kInitialSize = 1024;

private:
    std::vector<char> buffer_;
    size_t readerIndex_;
    size_t writerIndex_;
};
```

初始化：

* `readerIndex_ = kCheapPrepend`
* `writerIndex_ = kCheapPrepend`

这样前面留一点 prepend 区，方便协议头回填等扩展。

---

## 3. 核心接口

```cpp
size_t readableBytes() const;
size_t writableBytes() const;
size_t prependableBytes() const;

const char* peek() const;

void retrieve(size_t len);
void retrieveUntil(const char* end);
void retrieveAll();
std::string retrieveAllAsString();

void append(const char* data, size_t len);
void append(std::string_view sv);

char* beginWrite();
const char* beginWrite() const;
void hasWritten(size_t len);

void ensureWritableBytes(size_t len);
void makeSpace(size_t len);

ssize_t readFd(int fd, int* savedErrno);
ssize_t writeFd(int fd, int* savedErrno);
```

---

## 4. 为什么这种结构适合网络库

因为它能避免频繁：

* `erase(0, n)` 这种 O(n) 数据搬移
* 小块数据反复申请释放
* 每读一次就构造一个新的 `std::string`

它是典型的“读指针 / 写指针”模型。

---

## 5. readFd 的关键：readv 双缓冲策略

这是 v1 很值得做的一个“轻量零拷贝优化点”。

### 问题

假设当前 Buffer 可写空间只有 128 字节，但内核 socket 接收缓冲区里有 4KB 数据。
如果只用一次 `read(fd, beginWrite(), writableBytes())`：

* 只能读 128
* 还得再扩容再读一次

### 更好的做法：`readv`

准备两块 iovec：

1. Buffer 当前剩余可写空间
2. 一个栈上的临时大数组 `extrabuf[65536]`

```cpp
char extrabuf[65536];
struct iovec vec[2];
vec[0].iov_base = begin() + writerIndex_;
vec[0].iov_len = writableBytes();
vec[1].iov_base = extrabuf;
vec[1].iov_len = sizeof extrabuf;

int iovcnt = (writable < sizeof extrabuf) ? 2 : 1;
ssize_t n = ::readv(fd, vec, iovcnt);
```

### 结果

* 如果数据 <= writableBytes，直接进 Buffer
* 如果数据 > writableBytes，多余部分先进 `extrabuf`
* 然后再一次 append(extrabuf, extraLen)

这避免了：

* 先扩容再读
* 多次系统调用

严格说这不是“应用层绝对零拷贝”，但它是非常实用的**低成本高收益优化**。

---

## 6. writeFd

`outputBuffer` 的写出相对简单：

```cpp
ssize_t Buffer::writeFd(int fd, int* savedErrno) {
    ssize_t n = ::write(fd, peek(), readableBytes());
    if (n < 0) *savedErrno = errno;
    return n;
}
```

真正的逻辑控制通常放在 `TcpConnection::handleWrite()` 中。

---

## 7. 零拷贝优化该做到什么程度

v1 我建议分三层理解：

### 第一层：接口层避免无谓 copy

* `append(std::string_view)`
* `send(const void*, size_t)`
* 不强制用户传 `std::string`

### 第二层：读路径优化

* `readv` 双缓冲

### 第三层：写路径减少 copy

* `sendInLoop()` 先尝试直接 `write`
* 只把“写不完的尾巴” append 到 `outputBuffer`

这个非常重要。
因为很多小响应其实能直接从用户 buffer 发出去，根本不必进 outputBuffer。

---

## 8. v1 暂不做的高级零拷贝

这些留到 v2/v3：

* `sendfile`
* `splice`
* mmap 文件发送
* 引用计数分片 buffer / rope buffer
* scatter-gather write queue
* slab / arena allocator

---

## 9. Buffer v1 验收标准

* append / retrieve / readableBytes 正常
* 自动扩容正常
* 前部空间可复用
* `readFd()` 可一次读入大数据块
* `sendInLoop()` 能优先直写，剩余才进 outputBuffer
* 压测时不会因为频繁 string 拷贝明显抖动

---

# 六、模块四：多线程 EventLoop（one loop per thread）

这是把单线程 Reactor 变成工程化服务器的关键。

---

## 1. 目标模型

### 线程职责

* 主线程：

  * mainLoop
  * 监听 listenfd
  * accept 新连接
* 工作线程：

  * 每线程一个 EventLoop
  * 负责各自连接的读写

### 核心原则

* 一个 `EventLoop` 只在创建它的线程中运行
* 一个 `TcpConnection` 固定绑定一个 `EventLoop`
* 连接不会在多个 loop 之间迁移（v1 不做迁移）

---

## 2. EventLoop 必备能力

`EventLoop` 除了 `loop()` 以外，还要有：

```cpp
void runInLoop(Functor cb);
void queueInLoop(Functor cb);
void wakeup();
void doPendingFunctors();
bool isInLoopThread() const;
void assertInLoopThread() const;
```

---

## 3. 为什么必须有 wakeup 机制

因为其他线程可能调用：

* `conn->send(...)`
* `loop->queueInLoop(...)`
* `server` 要在 subLoop 创建连接

如果 subLoop 正阻塞在 `epoll_wait()`，必须有方式把它唤醒。

Linux 下最推荐的是：

* `eventfd`

### EventLoop 内部

* `wakeupFd_`
* `wakeupChannel_`

流程：

* 其他线程 `queueInLoop(cb)`
* 把 cb 放进 `pendingFunctors_`
* 调 `wakeup()`
* 向 eventfd 写 8 字节
* epoll_wait 返回
* `handleRead()` 读 eventfd 清空
* `doPendingFunctors()`

---

## 4. EventLoopThread

它的职责是：

> 启一个线程，并在线程中创建和运行 EventLoop，然后把 EventLoop* 暴露给外部。

建议结构：

```cpp
class EventLoopThread : noncopyable {
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;

    EventLoopThread(ThreadInitCallback cb = {}, const std::string& name = {});
    ~EventLoopThread();

    EventLoop* startLoop();

private:
    void threadFunc();

    EventLoop* loop_;
    bool exiting_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cond_;
    ThreadInitCallback callback_;
    std::string name_;
};
```

### startLoop()

* 启线程
* 等待 threadFunc 中 EventLoop 创建完成
* 返回 `EventLoop*`

---

## 5. EventLoopThreadPool

作用：

* 管理多个 `EventLoopThread`
* 提供 `getNextLoop()` 做 round-robin 分发

```cpp
class EventLoopThreadPool : noncopyable {
public:
    EventLoopThreadPool(EventLoop* baseLoop, const std::string& nameArg);

    void setThreadNum(int numThreads);
    void start(const ThreadInitCallback& cb = ThreadInitCallback());

    EventLoop* getNextLoop();
    std::vector<EventLoop*> getAllLoops();

private:
    EventLoop* baseLoop_;
    std::string name_;
    bool started_;
    int numThreads_;
    int next_;
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;
};
```

---

## 6. TcpServer 中如何使用线程池

### start()

* 启动线程池
* 在 mainLoop 中启动 acceptor listen

### 新连接到来

```cpp
void TcpServer::newConnection(int sockfd, const InetAddress& peerAddr) {
    EventLoop* ioLoop = threadPool_->getNextLoop();
    auto conn = std::make_shared<TcpConnection>(ioLoop, ...);

    connections_[connName] = conn;

    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);
    conn->setCloseCallback(
        [this](const TcpConnectionPtr& conn){ removeConnection(conn); });

    ioLoop->runInLoop([conn]{
        conn->connectEstablished();
    });
}
```

---

## 7. 多线程下最关键的工程原则

### 原则 1：IO 对象线程归属不可乱

`Channel / TcpConnection / Buffer` 默认只在 owner loop 线程中操作。

### 原则 2：跨线程只发任务，不直接碰对象内部状态

跨线程调用时：

* 不直接 `sendInLoop`
* 不直接 `channel_->enableWriting()`
* 要 `runInLoop / queueInLoop`

### 原则 3：对象销毁必须回到 owner loop

尤其是：

* `channel_->remove()`
* `fd close`
* connection state 切换收尾

---

## 8. 多线程 EventLoop v1 验收标准

* 主线程 accept，新连接能均匀分发到多个 subLoop
* 每个连接固定在所属线程处理 IO
* 跨线程 send 正常
* queueInLoop 能唤醒目标 loop
* 多客户端并发时无明显竞态崩溃
* 连接关闭能在所属 loop 正确清理

---

# 七、模块五：coroutine 接入点设计（重点）

这个部分我重点讲，因为你后面想做的是“mini-trantor + coroutine 网络库”，这一步设计如果走错，后面会很痛苦。

核心原则先说：

> **coroutine 不应该替代 Reactor。**
> **coroutine 应该建立在 Reactor 事件通知之上。**

也就是说：

* 底层仍然是 `epoll + EventLoop + Channel`
* 协程只是把“回调继续执行”改造成“挂起/恢复”

---

## 1. coroutine 在 mini-trantor 里应该解决什么问题

不是为了炫技，而是为了让业务代码从：

```cpp
conn->setMessageCallback([](auto& conn, Buffer* buf){
    // 状态机式拆分
});
```

逐步演进成：

```cpp
Task<> session(TcpConnectionPtr conn) {
    while (true) {
        auto msg = co_await conn->asyncReadSome();
        if (msg.empty()) break;
        co_await conn->asyncWrite(msg);
    }
}
```

也就是：

* 把“分段回调状态机”变成“顺序式业务逻辑”
* 但底层调度仍由 IO 事件驱动

---

## 2. coroutine 接入的三种层次

我建议你按层次推进。

---

### 层次 A：最小 awaitable 封装

给 `TcpConnection` 提供：

* `asyncReadSome()`
* `asyncWrite(...)`
* `asyncClose()`

它们返回 awaitable / Task。

这是最合适的 v1 目标。

---

### 层次 B：协议级 await

例如：

* `co_await conn->readExact(n)`
* `co_await conn->readUntil("\r\n")`
* `co_await codec->readFrame(conn)`

这是 v1.1/v2 很适合加的。

---

### 层次 C：协程会话框架

例如：

* 新连接建立后自动启动一个 session coroutine
* coroutine 内部处理整个连接生命周期

这个也很强，但我建议建立在层次 A 稳定后。

---

## 3. 一个关键问题：什么事件让协程 resume？

答案非常明确：

### 对于读 await

* 当 `Channel` 收到可读事件
* `TcpConnection::handleRead()` 把数据放入 `inputBuffer`
* 如果有等待读的 coroutine，就 resume 它

### 对于写 await

* 当待发送数据全部发完
* 在 `handleWrite()` 中检测 outputBuffer 已空
* resume 等待写完成的 coroutine

### 对于 close await

* 在 `handleClose()` 里 resume 等待关闭的 coroutine

---

## 4. 建议的协程接入架构

在 `TcpConnection` 中增加“等待者槽位”。

---

### 读等待者

```cpp
struct ReadAwaiterState {
    std::coroutine_handle<> handle;
    size_t minBytes = 1;
    bool active = false;
};
```

### 写等待者

```cpp
struct WriteAwaiterState {
    std::coroutine_handle<> handle;
    bool active = false;
};
```

### 关闭等待者

```cpp
struct CloseAwaiterState {
    std::coroutine_handle<> handle;
    bool active = false;
};
```

然后 `TcpConnection` 持有：

```cpp
std::optional<ReadAwaiterState> readWaiter_;
std::optional<WriteAwaiterState> writeWaiter_;
std::optional<CloseAwaiterState> closeWaiter_;
```

---

## 5. 为什么 v1 建议“一类事件一个等待者”

因为简单、稳定、够用。

### 不建议 v1 一上来做：

* 多个协程同时 await 同一个连接的读事件
* 多个 await 挂成队列
* 复杂取消语义

因为那会立刻把你带进：

* await 冲突管理
* 调度公平性
* 超时 / cancel / close 联动
* 一对多 resume

v1 最佳方案是：

> **一个连接同一时刻只允许一个读 await、一个写 await、一个关闭 await。**

这和大多数 session 协程模型是匹配的。

---

## 6. asyncReadSome 的设计

---

### 语义

* 如果 inputBuffer 已有数据，立即 ready，不挂起
* 否则注册自己为 `readWaiter_`
* 当后续有数据到来时 resume
* resume 后从 inputBuffer 取数据返回

---

### 伪接口

```cpp
class ReadSomeAwaitable {
public:
    ReadSomeAwaitable(TcpConnection& conn, size_t minBytes = 1);

    bool await_ready() const noexcept;
    void await_suspend(std::coroutine_handle<> h);
    std::string await_resume();

private:
    TcpConnection& conn_;
    size_t minBytes_;
};
```

---

### await_ready

```cpp
return conn_.inputBuffer_.readableBytes() >= minBytes_
       || conn_.state() != kConnected;
```

---

### await_suspend

* 保存 coroutine handle 到 `readWaiter_`
* 标记等待最少字节数
* 不做额外轮询，因为 Channel 已经在读监听状态

---

### await_resume

* 如果连接已断开且无数据，返回空串 / expected<..., error>
* 否则从 inputBuffer 取出数据返回

---

## 7. asyncWrite 的设计

语义：

* 用户 `co_await conn->asyncWrite(data)`
* 实际调用 `send(...)`
* 如果数据立刻写完，则 `await_ready = true`
* 否则注册为 `writeWaiter_`
* 当 outputBuffer 清空时 resume

这个接口非常适合业务逻辑。

---

### 伪接口

```cpp
class WriteAwaitable {
public:
    WriteAwaitable(TcpConnection& conn, std::string data);

    bool await_ready() const noexcept;
    void await_suspend(std::coroutine_handle<> h);
    void await_resume();

private:
    TcpConnection& conn_;
    std::string data_;
};
```

注意：

* 这里 `data_` 最好自持有，避免用户传入的 buffer 生命周期不够
* 或者设计成 `std::shared_ptr<std::string>` / `BufferChain`

v1 直接持有 `std::string` 就行。

---

## 8. 在 handleRead / handleWrite / handleClose 中恢复协程

---

### handleRead

```cpp
void TcpConnection::handleRead() {
    int savedErrno = 0;
    ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);

    if (n > 0) {
        if (readWaiter_ && readWaiter_->active &&
            inputBuffer_.readableBytes() >= readWaiter_->minBytes) {
            auto h = readWaiter_->handle;
            readWaiter_.reset();
            loop_->queueInLoop([h]{ h.resume(); });
        }
        if (messageCallback_) {
            messageCallback_(shared_from_this(), &inputBuffer_);
        }
    } else if (n == 0) {
        handleClose();
    } else {
        handleError();
    }
}
```

### 为什么建议 `queueInLoop([h]{ h.resume(); })`

而不是直接 `h.resume()`？

因为直接在 `handleRead()` 栈帧内 resume，可能导致：

* 递归重入
* 业务协程立刻再次操作 connection，干扰当前事件处理流程
* 控制流过深

放到 loop 的 pending queue 更稳。

---

### handleWrite

当 `outputBuffer_` 清空时：

```cpp
if (outputBuffer_.readableBytes() == 0) {
    channel_->disableWriting();

    if (writeWaiter_ && writeWaiter_->active) {
        auto h = writeWaiter_->handle;
        writeWaiter_.reset();
        loop_->queueInLoop([h]{ h.resume(); });
    }

    if (state_ == kDisconnecting) {
        shutdownInLoop();
    }
}
```

---

### handleClose

```cpp
if (closeWaiter_ && closeWaiter_->active) {
    auto h = closeWaiter_->handle;
    closeWaiter_.reset();
    loop_->queueInLoop([h]{ h.resume(); });
}
```

并且如果有读/写等待者，也要在关闭时一并 resume，让它们在 `await_resume()` 中感知连接关闭并退出。

这点非常重要，否则协程会永远挂住。

---

## 9. coroutine 返回类型该怎么选

你已经在学 C++20 协程，所以这里建议你直接统一成一个轻量 `Task<T>`：

* `Task<void>`
* `Task<std::string>`
* 后续可扩展 `Task<Expected<T, NetError>>`

### v1 建议

先别追求花哨：

* 做一个最小 `Task<T>`
* 支持 continuation
* 支持 `co_await task`
* 暂时不做线程池调度器
* 恢复逻辑由 EventLoop 驱动

---

## 10. 推荐的 session 协程模型

这是未来最有价值的形态。

### 新连接建立后：

```cpp
void onConnection(const TcpConnectionPtr& conn) {
    if (conn->connected()) {
        startSession(conn);
    }
}
```

### startSession

```cpp
Task<void> echoSession(TcpConnectionPtr conn) {
    while (conn->connected()) {
        auto msg = co_await conn->asyncReadSome();
        if (msg.empty()) break;
        co_await conn->asyncWrite(msg);
    }
}
```

### 启动方式

可以先简单做：

* 创建 `Task<void>`
* 立即 `start()` / `detach()`
* 不要求用户保留 handle

---

## 11. coroutine 接入时最危险的问题

---

### 问题 1：对象生命周期

协程挂起期间，`TcpConnection` 可能被销毁。

解决：

* awaitable 内部持有 `TcpConnectionPtr`
* 或 session coroutine 入口直接持有 `TcpConnectionPtr`

强烈建议：
**所有网络 awaitable 都持有 `TcpConnectionPtr`，不要只持裸引用。**

---

### 问题 2：连接关闭导致永远不 resume

解决：

* close 时必须唤醒所有相关 waiter
* `await_resume()` 检查连接状态，返回 EOF/错误

---

### 问题 3：多协程并发读同一连接

v1 直接禁止：

* 若已有 `readWaiter_`，再次 read await 直接 assert / throw

---

### 问题 4：跨线程 resume

必须恢复到 owner loop。
好在你当前设计中，所有 IO 事件都在 owner loop 触发，所以 resume 也应通过该 loop 的 `queueInLoop()` 进行。

---

## 12. coroutine v1 最小落地接口建议

我建议你第一版就做这 4 个：

```cpp
Task<std::string> TcpConnection::readSome();
Task<void> TcpConnection::write(std::string data);
Task<void> TcpConnection::waitClosed();
Task<void> TcpConnection::yieldToLoop();   // 可选，调试很好用
```

之后再加：

```cpp
Task<std::string> readExact(size_t n);
Task<std::string> readUntil(std::string_view delim);
Task<bool> writeWithTimeout(...);
```

---

## 13. coroutine 模块 v1 验收标准

* 单连接 echo session 可用协程写出来
* `co_await readSome()` 在无数据时挂起，有数据时恢复
* `co_await write()` 在发送完成时恢复
* 连接关闭时，等待中的协程能退出，不悬挂
* 不允许同一连接多个协程同时读
* 恢复都发生在 owner loop 线程

---

# 八、推荐目录结构

你现在做的是 mini-trantor，不要一开始目录散乱。

```text
mini-trantor/
├── CMakeLists.txt
├── examples/
│   ├── echo_server/
│   ├── echo_client/
│   └── coroutine_echo_server/
├── mini/
│   ├── base/
│   │   ├── noncopyable.h
│   │   ├── Timestamp.h
│   │   ├── Logger.h
│   │   └── Types.h
│   ├── net/
│   │   ├── EventLoop.h/.cc
│   │   ├── Channel.h/.cc
│   │   ├── Poller.h/.cc
│   │   ├── EPollPoller.h/.cc
│   │   ├── Buffer.h/.cc
│   │   ├── Socket.h/.cc
│   │   ├── SocketsOps.h/.cc
│   │   ├── InetAddress.h/.cc
│   │   ├── Acceptor.h/.cc
│   │   ├── TcpConnection.h/.cc
│   │   ├── TcpServer.h/.cc
│   │   ├── EventLoopThread.h/.cc
│   │   └── EventLoopThreadPool.h/.cc
│   ├── coroutine/
│   │   ├── Task.h
│   │   ├── TaskPromise.h
│   │   ├── NetAwaiters.h
│   │   └── CoroutineUtils.h
│   └── util/
│       ├── CurrentThread.h/.cc
│       └── Endian.h
└── tests/
    ├── test_buffer.cc
    ├── test_poller.cc
    ├── test_eventloop.cc
    └── test_coroutine.cc
```

---

# 九、核心类关系图

---

## 1. 类关系简图

```text
EventLoop
 ├── owns Poller
 ├── owns wakeup Channel
 ├── manages activeChannels
 └── executes pendingFunctors

Channel
 ├── belongs to EventLoop
 ├── wraps fd + interested events
 └── dispatches read/write/close/error callbacks

Poller
 └── EPollPoller

TcpServer
 ├── owns Acceptor
 ├── owns EventLoopThreadPool
 └── manages TcpConnection map

TcpConnection
 ├── belongs to one EventLoop
 ├── owns Socket
 ├── owns Channel
 ├── owns input/output Buffer
 └── hosts coroutine waiters
```

---

## 2. 连接建立流程图

```text
listenfd readable
 -> Acceptor::handleRead
 -> accept()
 -> TcpServer::newConnection
 -> threadPool->getNextLoop()
 -> create TcpConnection
 -> ioLoop->runInLoop(connectEstablished)
 -> channel enableReading
 -> connectionCallback
```

---

## 3. 协程读流程图

```text
co_await conn->readSome()
 -> await_ready?
    -> yes: 直接返回
    -> no: 保存 coroutine_handle 到 readWaiter_
 -> 挂起

socket readable
 -> handleRead
 -> inputBuffer append
 -> 若满足 readWaiter_ 条件
 -> queueInLoop(resume coroutine)
 -> coroutine 恢复
 -> await_resume() 取数据返回
```

---

# 十、推荐的 v1 开发顺序

这个顺序非常关键。别上来就写 coroutine。

---

## Phase 1：最小 Reactor 跑通

实现：

* Channel
* Poller
* EPollPoller
* EventLoop
* wakeup(eventfd)

验收：

* 一个 timer/pipe/eventfd demo 能跑
* 可以注册 fd 并收到读事件

---

## Phase 2：Buffer + Socket 基础设施

实现：

* InetAddress
* Socket
* SocketsOps
* Buffer
* readv 优化

验收：

* Buffer 单测通过
* socket 非阻塞 accept/read/write 跑通

---

## Phase 3：TcpConnection 单线程版

实现：

* Acceptor
* TcpConnection
* TcpServer（单线程）
* echo server

验收：

* 单线程 echo server 稳定
* 多连接可收发
* 优雅关闭正常

---

## Phase 4：多线程 one loop per thread

实现：

* EventLoopThread
* EventLoopThreadPool
* TcpServer 连接分发到 subLoop

验收：

* 多线程 echo server
* 多客户端压测无崩溃
* 跨线程 send 正常

---

## Phase 5：coroutine 底座

实现：

* Task<T>
* continuation
* 基础启动方式

验收：

* 纯内存 await demo 跑通
* continuation 恢复正确

---

## Phase 6：网络 awaitable 接入

实现：

* asyncReadSome/read waiter
* asyncWrite/write waiter
* close waiter

验收：

* coroutine echo server 跑通
* close 不悬挂
* 同连接单读 await 约束生效

---

# 十一、我建议你的 v1 里定义的“硬规则”

这些规则先写进设计文档，后面代码就不容易乱。

---

## 规则 1

**一个 TcpConnection 永远只属于一个 EventLoop。**

---

## 规则 2

**所有 fd 事件处理都必须在 owner loop 线程中进行。**

---

## 规则 3

**跨线程调用网络操作，只能通过 runInLoop / queueInLoop 投递。**

---

## 规则 4

**同一连接同一时刻只允许一个读协程等待。**

---

## 规则 5

**连接关闭时，必须唤醒所有挂起等待者。**

---

## 规则 6

**协程恢复不要直接在 handleRead/handleWrite 栈内 resume，统一 queueInLoop。**

---

## 规则 7

**v1 不做复杂协议层，messageCallback 先交给上层自己拆包。**

---

# 十二、你这个项目最适合的“里程碑版本”

我建议你不是叫一版叫 mini-trantor v1，而是拆成下面 4 个可提交版本：

---

## mini-trantor v0.1

**单线程 Reactor 核心**

* EventLoop
* Channel
* EPollPoller
* wakeup

---

## mini-trantor v0.2

**单线程 TCP**

* Buffer
* Acceptor
* TcpConnection
* TcpServer
* echo server

---

## mini-trantor v0.3

**多线程 TCP**

* EventLoopThread
* EventLoopThreadPool
* one loop per thread

---

## mini-trantor v1.0

**coroutine 接入**

* Task
* asyncReadSome
* asyncWrite
* coroutine echo server

这样你每个阶段都能产出一个“能演示、能验证、能继续迭代”的版本。

---

# 十三、最后给你一个“工程级结论”

如果你想把这个项目做成未来你自己游戏服务器网络底座的雏形，那么：

### 你最该抓住的不是“把所有功能都做出来”

而是这三件事：

**第一，线程归属清晰**

* 一个连接一个 loop
* 所有 IO 在 owner loop

**第二，生命周期清晰**

* TcpConnection 的建立、活跃、发送、关闭、销毁路径必须明确

**第三，协程只是 Reactor 的上层接口**

* 底层不是 coroutine scheduler 驱动
* 底层是 epoll 事件驱动
* coroutine 只是把回调继续执行改成 resume

这三点一旦立住，你后面要接：

* RPC
* codec
* length-header 协议
* Lua 协程桥接
* 游戏 session actor

都会顺很多。

---

如果你要，我下一步可以直接继续给你写第二份配套文档：
**《mini-trantor v1 类清单 + 每个类的头文件接口草案》**，我会把每个 `.h` 该有哪些字段和方法，直接按可开工的程度列出来。
