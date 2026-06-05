# Buffer 设计分析

## 1. 设计意图

Buffer 是 mini-trantor 的核心数据容器，服务于 non-blocking I/O 场景中的数据缓冲需求。
设计目标：**在 non-blocking + level-triggered 模式下，高效管理输入输出数据**。

## 2. 核心数据结构

```
                       prependable      readable      writable
                     ┌──────────────┬──────────────┬──────────────┐
 vector<char> buffer │  8 bytes     │   data...    │   free...    │
                     └──────────────┴──────────────┴──────────────┘
                     ↑              ↑              ↑              ↑
                     0         readerIndex_   writerIndex_    buffer.size()

 kCheapPrepend = 8    允许在消息前方追加长度头等元数据
```

### 三区域

| 区域 | 范围 | 用途 |
|------|------|------|
| prependable | [0, readerIndex_) | 前置区，追加消息头（初始 8 字节） |
| readable | [readerIndex_, writerIndex_) | 可读数据 |
| writable | [writerIndex_, size()) | 可写空间 |

## 3. 关键操作

### 3.1 readFd — 从 fd 读入

```cpp
ssize_t Buffer::readFd(int fd, int* savedErrno) {
    char extrabuf[65536];  // 栈上 64KB
    struct iovec vec[2];
    vec[0].iov_base = begin() + writerIndex_;
    vec[0].iov_len = writableBytes();
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof(extrabuf);

    const int iovcnt = (writableBytes() < sizeof(extrabuf)) ? 2 : 1;
    const ssize_t n = ::readv(fd, vec, iovcnt);

    if (n > 0) {
        if (n <= writableBytes()) {
            writerIndex_ += n;
        } else {
            writerIndex_ = buffer_.size();
            append(extrabuf, n - writableBytes_before);
        }
    }
    return n;
}
```

**设计精髓**:
- 使用 `readv` scatter read，先填满 buffer writable 区域
- 溢出部分读入栈上 64KB extrabuf
- 之后追加到 buffer（可能触发 resize）
- **好处**: 一次系统调用读取尽可能多数据，减少 read 次数

### 3.2 makeSpace — 空间整理

```
场景: writable 不够，但 prependable + writable 够
  Before: [8 prepend][  已读的空间  ][readable][writable不够]
  After:  [8 prepend][readable][       writable 够了        ]
  操作: memmove readable 到前面

场景: 总空间不够
  操作: buffer_.resize(writerIndex_ + len)
```

### 3.3 prepend — 前置追加

```cpp
void prepend(const void* data, size_t len) {
    assert(len <= prependableBytes());
    readerIndex_ -= len;
    memcpy(begin() + readerIndex_, data, len);
}
```

* 在 readable 前方插入数据
* 典型用途：追加 4 字节长度头
* O(1) 操作（不需要 memmove）

## 4. 设计决策分析

### 4.1 为什么用 vector<char> 而不是 fixed buffer？

| 方案 | 优点 | 缺点 |
|------|------|------|
| `vector<char>` | 自动 resize，无上限 | resize 有拷贝开销 |
| `fixed char[N]` | 零拷贝 | 大消息截断 |
| `deque<Block>` | 分段存储 | readv 不连续 |

mini-trantor 选择 vector，因为：
- level-triggered 模式下一次 read 必须读完
- 大消息需要 buffer 能增长
- readv + extrabuf 避免频繁 resize

### 4.2 为什么要 prependable 区域？

应用层协议通常在消息前加长度头。如果没有 prepend：
1. 先序列化 body → 计算长度 → 分配新 buffer → 拷贝 header + body
2. 有了 prepend → 先序列化 body → prepend(len) → 零拷贝

### 4.3 为什么 readFd 用 readv + extrabuf？

- 如果 writable 只有 100 字节，但 fd 有 128KB 数据
- 方案 A: 循环 read 直到 EAGAIN → 多次系统调用
- 方案 B: 先 resize 到很大 → 浪费内存
- **方案 C (采用)**: readv 一次读取，溢出到栈上 extrabuf，再 append → 一次系统调用 + 一次 memcpy

### 4.4 为什么 extrabuf 是 64KB？

- TCP 接收缓冲区默认 ~128KB
- 64KB 在栈上是安全的（默认栈 8MB，一个函数帧 64KB 还行）
- 加上 buffer writable 空间，大概率能一次 read 完

## 5. 线程安全

* Buffer 不是线程安全的
* 由 TcpConnection 的 owner loop 线程独占访问
* 输入 buffer (inputBuffer_) 只在 handleRead 中写入
* 输出 buffer (outputBuffer_) 只在 sendInLoop/handleWrite 中操作

## 6. 性能特征

| 操作 | 复杂度 | 说明 |
|------|--------|------|
| append | 均摊 O(1) | vector push_back 语义 |
| retrieve | O(1) | 只移动 readerIndex_ |
| readFd | O(n) | readv 一次系统调用 |
| prepend | O(1) | 指针前移 + memcpy |
| makeSpace (compact) | O(n) | memmove readable 数据 |
| makeSpace (resize) | O(n) | vector resize + copy |

## 7. 与其他实现的比较

| 特性 | mini-trantor Buffer | muduo Buffer | Netty ByteBuf |
|------|--------------------|--------------|--------------------|
| 底层 | vector<char> | vector<char> | byte[] 池化 |
| readv optimize | ✅ 64KB extrabuf | ✅ 64KB extrabuf | N/A (Java NIO) |
| prepend | ✅ 8 bytes | ✅ 8 bytes | writerIndex 概念 |
| zero-copy | ❌ | ❌ | ✅ composite buffer |
| pool | ❌ | ❌ | ✅ pooled allocator |
