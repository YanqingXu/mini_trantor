# Buffer —— 工程级源码拆解

## 1. 类定位

* **角色**：非阻塞 I/O 的**字节缓冲容器**，解决读写不完整的问题
* **层级**：Net 层（基础设施）
* 每个 TcpConnection 持有两个 Buffer：inputBuffer_（接收）和 outputBuffer_（发送）

```
内核:
  recv(fd, ...) → 可能只读到部分数据
  write(fd, ...) → 可能只写了部分数据

Buffer 解决:
  ┌──────────────────────────────────────┐
  │ prependable │  readable  │ writable  │
  │   (8 B)     │  (已收数据) │ (待填空间) │
  └──────────────────────────────────────┘
  0          readerIndex_  writerIndex_  buffer_.size()
```

## 2. 解决的问题

**核心问题**：非阻塞 I/O 中，一次 `read()`/`write()` 不保证完整读/写所有数据，如何高效地缓冲和管理？

如果没有 Buffer：
- 读：用户需要自己拼接多次 read 的结果，管理临时缓冲区
- 写：用户需要记录已写位置，处理 EAGAIN 后的重试
- 解析：协议解析需要可靠的"查看但不消费"(peek) 操作

Buffer 提供：
1. **自动扩容**：数据超出当前容量时自动增长
2. **空间回收**：前方已读空间可以被重用（compact）
3. **readv 优化**：一次 syscall 尽可能多读数据（scatter/gather I/O）
4. **prepend 支持**：在已读数据前面插入数据（如包头长度字段）

## 3. 对外接口（API）

### 查看状态

| 方法 | 用途 |
|------|------|
| `readableBytes()` | 已接收但未消费的数据量 |
| `writableBytes()` | 当前可直接写入的空间 |
| `prependableBytes()` | 前方可用于 prepend 的空间 |
| `peek()` | 查看可读数据的起始指针 |
| `beginWrite()` | 可写区域的起始指针 |

### 消费数据

| 方法 | 用途 |
|------|------|
| `retrieve(len)` | 移动读指针，"消费"len 字节 |
| `retrieveUntil(end)` | 消费到指定指针位置 |
| `retrieveAll()` | 消费所有数据（重置读写指针） |
| `retrieveAsString(len)` | 取出 len 字节为 string |
| `retrieveAllAsString()` | 取出所有可读数据为 string |

### 写入数据

| 方法 | 用途 |
|------|------|
| `append(data, len)` | 追加数据（自动扩容） |
| `append(string_view)` | 追加 string_view |
| `ensureWritableBytes(len)` | 确保有 len 字节可写空间 |
| `hasWritten(len)` | 手动推进写指针 |

### I/O 操作

| 方法 | 用途 |
|------|------|
| `readFd(fd, savedErrno)` | 从 fd 读数据到 Buffer (readv) |
| `writeFd(fd, savedErrno)` | 从 Buffer 写数据到 fd |

## 4. 核心成员变量

```cpp
std::vector<char> buffer_;     // 底层连续内存
std::size_t readerIndex_;      // 读指针（下一个可读位置）
std::size_t writerIndex_;      // 写指针（下一个可写位置）
```

### 初始状态

```
kCheapPrepend = 8
kInitialSize = 1024

buffer_.size() = 8 + 1024 = 1032
readerIndex_ = 8
writerIndex_ = 8
readable = 0, writable = 1024, prependable = 8
```

### 内存布局

```
┌──────────┬───────────────────┬──────────────────────┐
│ prepend  │     readable      │      writable        │
│ (≥8 B)   │  (有数据未消费)    │  (可直接写入)         │
└──────────┴───────────────────┴──────────────────────┘
↑          ↑                   ↑                       ↑
0      readerIndex_        writerIndex_           buffer_.size()
```

## 5. 执行流程（最重要）

### 5.1 readFd —— 核心读取方法

```cpp
ssize_t Buffer::readFd(int fd, int* savedErrno) {
    char extraBuffer[65536];             // 64KB 栈上缓冲
    struct iovec vec[2];

    const std::size_t writable = writableBytes();

    vec[0].iov_base = beginWrite();      // Buffer 内部可写空间
    vec[0].iov_len = writable;
    vec[1].iov_base = extraBuffer;       // 栈上额外空间
    vec[1].iov_len = sizeof(extraBuffer);

    // 如果 Buffer 可写空间 < 64KB，用 2 个 iovec
    // 如果 Buffer 可写空间 ≥ 64KB，只用 1 个 iovec（已经够大）
    const int iovcnt = writable < sizeof(extraBuffer) ? 2 : 1;
    const ssize_t n = ::readv(fd, vec, iovcnt);

    if (n < 0) {
        *savedErrno = errno;
    } else if (static_cast<std::size_t>(n) <= writable) {
        // 数据全部放入 Buffer 内部空间
        writerIndex_ += n;
    } else {
        // 数据溢出到 extraBuffer
        writerIndex_ = buffer_.size();
        append(extraBuffer, n - writable);  // 触发扩容
    }
    return n;
}
```

**为什么用 readv + 栈缓冲？**

```
方案 A: 先 ioctl(FIONREAD) 获取可读量 → 扩容 → read()
  缺点: 两次系统调用

方案 B: 预分配大 Buffer
  缺点: 每个连接浪费大量内存

方案 C: readv + 64KB 栈缓冲 ✅
  优点: 一次 syscall，不浪费堆内存，溢出部分再 append
```

**readv 的分散读取**：

```
内核数据: [ABCDEFGHIJ] (10 字节)

readv(fd, [{buf1, 3}, {buf2, 7}])

buf1: [ABC]        → Buffer 内部空间
buf2: [DEFGHIJ]    → 栈上 extraBuffer → append 到 Buffer
```

### 5.2 writeFd

```cpp
ssize_t Buffer::writeFd(int fd, int* savedErrno) {
    const ssize_t n = ::write(fd, peek(), readableBytes());
    if (n < 0) {
        *savedErrno = errno;
    }
    return n;
}
```

注意：`writeFd` 后**不自动 retrieve**。调用者需要手动 `retrieve(n)`。
这是因为 TcpConnection::handleWrite 中 TLS 路径有特殊处理。

### 5.3 makeSpace —— 空间管理

```cpp
void Buffer::makeSpace(std::size_t len) {
    if (writableBytes() + prependableBytes() - kCheapPrepend < len) {
        // 回收前方空间也不够 → 直接扩容
        buffer_.resize(writerIndex_ + len);
    } else {
        // 回收前方空间够用 → compact（整理）
        const std::size_t readable = readableBytes();
        std::copy(begin() + readerIndex_, begin() + writerIndex_,
                  begin() + kCheapPrepend);
        readerIndex_ = kCheapPrepend;
        writerIndex_ = readerIndex_ + readable;
    }
}
```

**Compact 示例**：

```
Before compact:
┌────────────────────────┬───────┬──────┐
│ prepend (已读，浪费)     │ readable │ writable │
│ ████████████████████    │ ABCD  │ ##   │
└────────────────────────┴───────┴──────┘
                          ↑ reader   ↑ writer

After compact:
┌──────────┬──────────┬──────────────────────────┐
│ prepend  │ readable │ writable (回收了前方空间)    │
│ 8B       │ ABCD     │ ########################  │
└──────────┴──────────┴──────────────────────────┘
           ↑ reader   ↑ writer
```

### 5.4 append

```cpp
void Buffer::append(const char* data, std::size_t len) {
    ensureWritableBytes(len);                    // 自动扩容
    std::memcpy(beginWrite(), data, len);        // 拷贝数据
    hasWritten(len);                             // 推进写指针
}
```

### 5.5 retrieve

```cpp
void Buffer::retrieve(std::size_t len) {
    if (len < readableBytes()) {
        readerIndex_ += len;                     // 部分消费
    } else {
        retrieveAll();                           // 全部消费 → 重置指针
    }
}

void Buffer::retrieveAll() {
    readerIndex_ = kCheapPrepend;
    writerIndex_ = kCheapPrepend;               // 回到初始状态
}
```

## 6. 关键交互关系

```
TcpConnection
    │
    ├── inputBuffer_
    │   ├── readFd() ← handleRead()
    │   ├── peek() → messageCallback (用户读数据)
    │   └── retrieve() → 用户消费数据
    │
    └── outputBuffer_
        ├── append() ← sendInLoop()
        ├── writeFd() ← handleWrite()
        └── retrieve() ← handleWrite() 成功后
```

| 类 | 关系 |
|----|------|
| **TcpConnection** | 拥有 2 个 Buffer 实例 |
| **用户代码** | 通过 messageCallback 的 Buffer* 参数访问 |
| **Channel** | 间接（Channel 触发 handleRead/handleWrite → 操作 Buffer） |

## 7. 关键设计点

### 7.1 kCheapPrepend

```
预留 8 字节 prepend 空间
```

用途：协议解析完毕后，可以在消息前面插入长度字段：
```cpp
int32_t len = htonl(readableBytes());
// 在读指针前面写入 4 字节长度
memcpy(peek() - 4, &len, 4);
```

8 字节足够放一个 int64_t 或两个 int32_t。

### 7.2 vector<char> 而不是自定义分配器

- `std::vector<char>` 提供连续内存 + 自动管理 + 拷贝/移动
- 不需要 `std::string`（因为需要精确控制 size vs capacity）
- 不需要自定义分配器（v1 追求简单正确）

### 7.3 readv 的 iovec 数量决策

```cpp
const int iovcnt = writable < sizeof(extraBuffer) ? 2 : 1;
```

如果 Buffer 可写空间已经 ≥ 64KB，就不需要 extraBuffer，减少 copy。

### 7.4 不加锁

Buffer 由"所属连接的 loop 线程"独占使用，不需要锁。
跨线程 send 通过 `runInLoop` 回到 loop 线程后才操作 Buffer。

## 8. 潜在问题

### 8.1 无限增长

Buffer 只增不缩。如果短时间内收到大量数据，Buffer 可能扩容到很大，
之后即使数据减少，内存也不会释放。

**缓解**：配合背压策略（disableReading）限制输入速率。

### 8.2 readFd 的 65536 字节栈缓冲

每次 readFd 调用使用 64KB 栈空间。如果调用栈很深（如协程帧上 readFd），
可能接近栈溢出。

**风险**：低。64KB 相对标准 8MB 栈空间很小，协程帧也有独立的栈。

### 8.3 writeFd 不自动 retrieve

调用者必须记得 `retrieve(n)`，否则数据会被重复发送。
当前只有 TcpConnection::handleWrite 调用 writeFd，且紧跟 retrieve。

### 8.4 copy vs move

`retrieveAsString` 创建 `std::string` 后 retrieve，会触发一次 memcpy。
对于大数据量可能有性能影响，但语义清晰。

## 9. 极简实现

```cpp
class MinimalBuffer {
public:
    MinimalBuffer() : buf_(1024), rIdx_(0), wIdx_(0) {}

    void append(const char* data, size_t len) {
        if (wIdx_ + len > buf_.size())
            buf_.resize(wIdx_ + len);
        memcpy(&buf_[wIdx_], data, len);
        wIdx_ += len;
    }

    const char* peek() const { return &buf_[rIdx_]; }
    size_t readableBytes() const { return wIdx_ - rIdx_; }

    void retrieve(size_t len) {
        rIdx_ += len;
        if (rIdx_ == wIdx_) { rIdx_ = wIdx_ = 0; }
    }

private:
    std::vector<char> buf_;
    size_t rIdx_, wIdx_;
};
```

完整版增加：
1. **kCheapPrepend** → 前方预留空间
2. **readv + extraBuffer** → 高效读取
3. **makeSpace compact** → 空间回收
4. **writeFd** → 写到 fd
5. **string_view/string 重载** → 接口便利

## 10. 面试角度总结

**Q1: Buffer 的内存布局是什么？**
A: `[prependable | readable | writable]`。prependable 是已消费的空间（初始 8 字节），readable 是待读数据，writable 是可写空间。

**Q2: readFd 为什么用 readv 而不是 read？**
A: readv 用 scatter/gather I/O 一次读入多个缓冲区。Buffer 内部空间 + 64KB 栈缓冲，一次 syscall 最多读 64KB+，避免多次 read。

**Q3: 为什么用栈上的 extraBuffer？**
A: 避免为每个连接预分配大 Buffer 浪费内存。栈上分配零开销，读完后 append 到 Buffer（按需扩容）。

**Q4: makeSpace 什么时候 compact，什么时候 resize？**
A: 如果 `writable + prependable - 8 >= len`，compact（把 readable 移到前面）；否则 resize 扩容。

**Q5: 为什么 retrieveAll 不 shrink_to_fit？**
A: 避免频繁分配/释放。Buffer 大小通常稳定在某个水位，shrink 后可能很快又需要扩容。

**Q6: Buffer 的线程安全怎么保证？**
A: 不加锁。由 TcpConnection 的 loop 线程独占使用。跨线程 send 通过 runInLoop 回到正确线程。
