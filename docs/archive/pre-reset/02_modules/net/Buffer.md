# Buffer —— 工程级源码拆解

## 1. 类定位

* **角色**：连接读写路径上的**字节容器**，负责维护可读/可写/可预留区域
* **层级**：Net 工具层（被 TcpConnection 拥有，不属于 Reactor 核心层）
* 每个 TcpConnection 拥有一个 `inputBuffer_` 和一个 `outputBuffer_`

```
                   Kernel Socket Buffer
                          │
                     readv (scatter-read)
                          │
                          ▼
              ┌───────────────────────┐
              │ Buffer (inputBuffer_) │
              │ [prepend][readable][writable]
              └───────────┬───────────┘
                          │ messageCallback_(conn, &inputBuffer_)
                          ▼
                   业务代码 (retrieve)
              
              ┌───────────────────────┐
              │ Buffer (outputBuffer_)│
              │ [prepend][readable][writable]
              └───────────┬───────────┘
                          │ writable event → writeFd
                          ▼
                   Kernel Socket Buffer
```

Buffer **不拥有 socket**，也**不解析协议**，它只是字节的暂存区。

---

## 2. 解决的问题

**核心问题**：TCP 是字节流，一次 `read()` 可能收到半个消息，也可能收到多个消息。如何优雅地管理这些字节？

如果没有 Buffer：
- 每次 `read()` 只能用固定大小的栈上数组 → 可能截断数据
- 写操作必须一次性完成 → 无法处理 "写一半" 的情况
- 业务代码需要自己维护偏移量 → 容易出错
- scatter-read（readv）优化需要手动实现 → 代码分散

Buffer 的设计解决了：
1. **动态扩容**：写满时自动扩大
2. **空间复用**：已读数据区 (`prependableBytes`) 在写满时可被回收
3. **scatter-read**：`readFd` 用栈上 64KB 辅助缓冲作为第二 iov，避免过度分配
4. **prepend 区域**：预留 8 字节用于在头部写入消息长度，无需移动已有数据

---

## 3. 对外接口（API）

### 状态查询

| 方法 | 用途 |
|------|------|
| `readableBytes()` | 可读字节数（待消费） |
| `writableBytes()` | 可写字节数（可追加） |
| `prependableBytes()` | prepend 区大小（已读区 + kCheapPrepend） |
| `peek()` | 可读区起始指针（只读） |
| `beginWrite()` | 可写区起始指针 |

### 数据追加

| 方法 | 用途 |
|------|------|
| `append(data, len)` | 追加原始字节 |
| `append(string_view)` | 追加字符串视图 |
| `ensureWritableBytes(len)` | 确保有 `len` 字节可写（必要时扩容） |
| `hasWritten(len)` | 手动推进 writerIndex（配合 `beginWrite()` 零拷贝写入） |

### 数据消费

| 方法 | 用途 |
|------|------|
| `retrieve(len)` | 推进 readerIndex（消费 `len` 字节） |
| `retrieveUntil(ptr)` | 消费到指针位置 |
| `retrieveAll()` | 重置所有索引（复用缓冲） |
| `retrieveAllAsString()` | 读取全部数据为 string |
| `retrieveAsString(len)` | 读取 `len` 字节为 string |

### fd I/O 桥接

| 方法 | 用途 |
|------|------|
| `readFd(fd, &errno)` | 从 fd 读数据（scatter-read） |
| `writeFd(fd, &errno)` | 把可读数据写入 fd |

---

## 4. 核心成员变量

```cpp
std::vector<char> buffer_;    // 底层存储，动态扩容
std::size_t readerIndex_;     // 可读区的起始偏移（通常 >= kCheapPrepend）
std::size_t writerIndex_;     // 可写区的起始偏移（即可读区末尾）
```

### 内存布局（初始状态）

```
buffer_:
┌────────────────────────────────────────────────────────────────┐
│ kCheapPrepend (8) │     kInitialSize (1024)                    │
│   [prepend区]     │    [      writable区      ]                 │
└────────────────────────────────────────────────────────────────┘
                    ↑ readerIndex_                               ↑ writerIndex_ = readerIndex_
                    (readableBytes = 0)
```

### 数据追加后

```
buffer_:
┌───────────────┬─────────────┬──────────────────────────────────┐
│  prepend (8)  │  readable   │          writable                │
│               │   (N bytes) │         (1024-N bytes)           │
└───────────────┴─────────────┴──────────────────────────────────┘
                ↑ readerIndex_           ↑ writerIndex_
```

### 线程安全

Buffer **不做任何同步**，由调用者（TcpConnection 的 owner loop）保证单线程访问。

---

## 5. 执行流程（关键路径）

### 5.1 从 socket 读取数据（readFd）

```cpp
ssize_t Buffer::readFd(int fd, int* savedErrno) {
    char extraBuffer[65536];  // 64KB 栈上辅助缓冲
    struct iovec vec[2];
    const std::size_t writable = writableBytes();

    vec[0].iov_base = beginWrite();       // 先写到 buffer_ 的可写区
    vec[0].iov_len = writable;
    vec[1].iov_base = extraBuffer;        // 溢出部分写到栈上临时缓冲
    vec[1].iov_len = sizeof(extraBuffer);

    const int iovcnt = writable < sizeof(extraBuffer) ? 2 : 1;
    const ssize_t n = ::readv(fd, vec, iovcnt);
    // ...
    if (static_cast<std::size_t>(n) <= writable) {
        writerIndex_ += n;               // 数据全在 buffer_ 内
    } else {
        writerIndex_ = buffer_.size();   // buffer_ 满了
        append(extraBuffer, n - writable); // 把溢出部分 append 进来（触发扩容）
    }
    return n;
}
```

**为什么用 readv + 栈上辅助缓冲？**
- 避免预先分配过大内存（不知道一次 read 会来多少数据）
- 正常情况下数据写入 `buffer_`，不触发额外分配
- 数据量大时溢到栈上，再 `append` 合并 → 一次系统调用收完全部数据

### 5.2 makeSpace —— 扩容策略

```
ensureWritableBytes(len) 触发 makeSpace(len)：

策略1: 空间复用（优先）
  如果 writableBytes + prependableBytes >= len + kCheapPrepend：
    → 把 readable 区数据 memmove 到 kCheapPrepend 位置
    → 回收已消费的 prepend 区
    → 不分配新内存

策略2: 扩容（必要时）
  如果策略1仍然不够：
    → buffer_.resize(writerIndex_ + len)
    → vector 自动分配更大内存
```

```
复用前:
┌──────────────────────────┬──────────┬───────────┐
│   大 prepend 区（已读）    │ readable │  writable │
└──────────────────────────┴──────────┴───────────┘

复用后:
┌────────────┬──────────┬───────────────────────────┐
│ prepend(8) │ readable │     更大的 writable        │
└────────────┴──────────┴───────────────────────────┘
```

**核心不变式**：`makeSpace` 后，原有的 readable 数据内容完全保留，只是偏移量发生了变化。

### 5.3 消费数据（retrieve）

```
业务代码处理完消息后：
  buffer.retrieveUntil(end_of_message)
    → retrieve(end_of_message - peek())
        → 如果 len < readableBytes：
            readerIndex_ += len        // 只移动读指针
        → 否则：
            retrieveAll()              // 复位到初始状态（快速路径）
```

---

## 6. 协作关系

```
      TcpConnection
           │
     拥有两个 Buffer:
     ├── inputBuffer_  ← handleRead() 中 readFd() 填充
     │                  ↓
     │             messageCallback_(conn, &inputBuffer_)
     │                  ↓
     │             业务代码读取 + retrieve
     │
     └── outputBuffer_ ← send() → sendInLoop() → append()
                         ↓
                    handleWrite() → writeFd() → retrieve
```

| 协作方 | 行为 |
|--------|------|
| `TcpConnection::handleRead` | 调用 `inputBuffer_.readFd()` 填充入站数据 |
| `messageCallback_` | 接收 `Buffer*` 指针，读取后调用 `retrieve` |
| `TcpConnection::sendInLoop` | 调用 `outputBuffer_.append()` 追加出站数据 |
| `TcpConnection::handleWrite` | 调用 `outputBuffer_.writeFd()` 向 socket 写出 |

---

## 7. 关键设计点

### 7.1 kCheapPrepend 的意义

```cpp
static constexpr std::size_t kCheapPrepend = 8;
```

预留 8 字节 prepend 区，允许在**不移动数据**的情况下在消息头部写入长度字段：

```cpp
// 发送 "len + body" 格式的消息（无复制）
uint32_t len = htonl(body.size());
buffer.prepend(&len, sizeof(len));   // 写到 prepend 区
buffer.writeFd(fd, &errno);          // 一次发送 header + body
```

如果没有 prepend 区，需要先申请一个临时 vector 拼接 header + body，多一次内存拷贝。

### 7.2 scatter-read 的三段式

```
状态1: writable >= 64KB
  → iovcnt=1, 只写进 buffer_
  → 一次 readv，无栈内存使用

状态2: writable < 64KB（通常情况）
  → iovcnt=2, buffer_ + extraBuffer
  → 如果 n <= writable：writerIndex_ += n（快速路径）
  → 如果 n > writable：writerIndex_ = buffer_.size()，再 append 溢出部分
```

关键权衡：**64KB 栈内存换取不预分配 buffer_ 的灵活性**。

### 7.3 所有权与生命周期

- Buffer 的生命周期与 TcpConnection 绑定
- Buffer **不拥有 fd**，fd 属于 `Socket` 对象
- Buffer 所在的 TcpConnection 由 `shared_ptr` 管理
- `messageCallback_` 中不应持有 Buffer 的引用超过回调范围

---

## 8. 核心模块改动闸门（5 问）

1. **哪个 loop/线程拥有此模块？**
   Owner TcpConnection 的 loop 线程。Buffer 自身不做同步。

2. **谁拥有它，谁释放它？**
   TcpConnection 以成员变量形式拥有（非指针），随 TcpConnection 析构。

3. **哪些回调可能重入？**
   Buffer 内无回调，不存在重入风险。

4. **哪些操作允许跨线程，如何 marshal？**
   Buffer 不允许跨线程操作。send() 的跨线程 marshal 在 TcpConnection 层完成（copy payload 后 runInLoop），Buffer 本身只在 loop 线程被操作。

5. **哪个测试文件验证此改动？**
   `tests/unit/test_buffer.cc`（字节语义）、`tests/contract/test_tcp_connection.cc`（readFd/writeFd 集成路径）

---

## 9. 从极简到完整的演进路径

```cpp
// 极简版本
class MinimalBuffer {
    std::vector<char> buf_;
    size_t rIdx_{0}, wIdx_{0};
public:
    void append(const char* d, size_t n) {
        buf_.resize(wIdx_ + n);
        memcpy(buf_.data() + wIdx_, d, n);
        wIdx_ += n;
    }
    const char* peek() { return buf_.data() + rIdx_; }
    void retrieve(size_t n) { rIdx_ += n; }
    size_t readableBytes() { return wIdx_ - rIdx_; }
};
```

从极简 → 完整，需要加：

1. **kCheapPrepend 区** → 支持 O(1) 头部写入
2. **makeSpace 复用策略** → 避免不必要的内存分配
3. **scatter-read（readFd + iovec）** → 一次 readv 读完内核缓冲区
4. **writeFd** → 直接向 fd 写（无中间拷贝）
5. **retrieveUntil / retrieveAllAsString** → 便利 API

---

## 10. 易错点与最佳实践

### 易错点

| 错误 | 后果 |
|------|------|
| 在回调外持有 `peek()` 返回的指针 | `makeSpace` 扩容后指针失效 |
| 忘记 `retrieve` | 下次回调时 readableBytes 累积，逻辑乱掉 |
| 多线程访问同一个 Buffer | 数据竞争（Buffer 无锁） |
| `hasWritten(len)` 前未调用 `ensureWritableBytes` | 写越界 |

### 最佳实践

```cpp
// ✓ 消费完整消息的正确姿势：
void onMessage(const TcpConnectionPtr& conn, Buffer* buf) {
    while (buf->readableBytes() >= kHeaderLen) {
        uint32_t len = ntohl(*reinterpret_cast<const uint32_t*>(buf->peek()));
        if (buf->readableBytes() < kHeaderLen + len) break;  // 不够，等下次
        buf->retrieve(kHeaderLen);
        std::string body = buf->retrieveAsString(len);
        process(body);
    }
}

// ✓ 高效零拷贝读取后自定义处理：
char* dst = static_cast<char*>(myBuffer);
size_t n = std::min(buf->readableBytes(), myCapacity);
memcpy(dst, buf->peek(), n);
buf->retrieve(n);
```

---

## 11. 面试角度总结

**Q1: Buffer 的内存布局是什么样的？**
A: `[kCheapPrepend=8 字节][readable 区][writable 区]`，三个区通过两个索引 `readerIndex_` 和 `writerIndex_` 划分。初始时 `readerIndex_ == writerIndex_ == 8`，readable 为 0。

**Q2: 为什么用 readv + 栈上辅助缓冲？**
A: 避免 `buffer_` 预分配过大内存。先尝试把数据塞进已有 writable 区，不够了才用栈上 64KB。`readv` 一次系统调用读完所有数据，比多次 `read` 更高效。

**Q3: makeSpace 的两种策略？**
A: 策略1 是内部复用：如果 `prependableBytes + writableBytes >= len + kCheapPrepend`，把 readable 数据 memmove 到头部，不分配新内存；策略2 是真正扩容：`vector::resize`。

**Q4: Buffer 是线程安全的吗？**
A: 不是。Buffer 无任何同步机制，由 TcpConnection 的 owner loop 单线程持有和操作。

**Q5: kCheapPrepend 有什么用？**
A: 预留 8 字节，允许在消息头部写入长度字段而无需移动 body 数据，避免了一次内存拷贝（prepend 优化）。

**Q6: peek() 返回的指针什么时候会失效？**
A: 调用 `makeSpace`（即 `ensureWritableBytes`）时，如果触发了 `vector::resize`，内部会重新分配内存，所有旧指针失效。因此不要在 `append` 之后继续使用之前缓存的 `peek()` 指针。
