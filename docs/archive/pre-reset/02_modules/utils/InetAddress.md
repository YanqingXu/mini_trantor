# InetAddress 源码拆解

## 1. 类定位

* **角色**: 工具类（Utils）
* **层级**: Net 层基础设施
* InetAddress 是 **IPv4 地址的轻量封装**——对 `sockaddr_in` 的构造、保存与文本转换

## 2. 解决的问题

* **核心问题**: 统一 IPv4 地址的表示，避免在调用处直接操作 `sockaddr_in` 结构
* 如果没有 InetAddress:
  - 每处创建地址都要手动 memset + htons + inet_pton
  - 地址到字符串转换代码散落各处
  - 接口签名混乱（有的用 sockaddr_in*，有的用 ip+port）

## 3. 对外接口（API）

| 方法 | 场景 | 调用者 |
|------|------|--------|
| `InetAddress(port, loopbackOnly)` | 按端口创建（INADDR_ANY 或 LOOPBACK） | TcpServer / Acceptor |
| `InetAddress(ip, port)` | 指定 IP + 端口 | TcpClient / 用户 |
| `InetAddress(sockaddr_in)` | 从系统结构构造 | Socket::accept |
| `getSockAddrInet()` | 获取底层 sockaddr_in（const ref） | Socket / sockets 函数 |
| `setSockAddrInet(addr)` | 设置底层 sockaddr_in | accept 后更新 |
| `toIp()` | 转为 "1.2.3.4" 字符串 | 日志 / 调试 |
| `toIpPort()` | 转为 "1.2.3.4:8080" 字符串 | 日志 / 连接名 |
| `port()` | 获取端口号（主机字节序） | 用户 |

## 4. 核心成员变量

```
InetAddress
├── addr_: sockaddr_in    // IPv4 地址结构（网络字节序）
```

* 值语义，可拷贝
* 无动态内存分配
* 创建时 memset 清零

## 5. 执行流程

### 5.1 构造

```
InetAddress(port=8080, loopbackOnly=false)
  → memset(&addr_, 0, sizeof(addr_))
  → addr_.sin_family = AF_INET
  → addr_.sin_addr = htonl(INADDR_ANY)   // 或 INADDR_LOOPBACK
  → addr_.sin_port = htons(8080)

InetAddress("192.168.1.1", 8080)
  → 先调用 InetAddress(8080, false)
  → inet_pton(AF_INET, "192.168.1.1", &addr_.sin_addr)
  → 无效 IP → throw runtime_error
```

### 5.2 使用场景

```
Acceptor::listen():
  → InetAddress addr(port)
  → socket_.bindAddress(addr)         // addr.getSockAddrInet()

Socket::accept():
  → sockaddr_in addr
  → accept4(fd, &addr, ...)
  → peerAddr->setSockAddrInet(addr)   // 更新传出参数

TcpConnection 构造:
  → name_ = localAddr.toIpPort() + "-" + peerAddr.toIpPort()
```

## 6. 关键交互关系

```
┌──────────┐    ┌──────────┐    ┌──────────────┐
│ Acceptor │    │ Socket   │    │ TcpConnection│
│          │───▶│          │───▶│              │
└──────────┘    └──────────┘    └──────────────┘
     │               │               │
     └───────────────┴───────────────┘
                     │
                     ▼
              ┌──────────────┐
              │ InetAddress  │
              └──────────────┘
```

* 几乎所有 Net 层类都使用 InetAddress
* 纯值类型，无反向依赖

## 7. 关键设计点

* **值语义**: 可自由拷贝、移动，无需 shared_ptr
* **仅 IPv4**: 当前不支持 IPv6（sockaddr_in 而非 sockaddr_in6）
* **异常安全**: 无效 IP 字符串直接 throw，不会产生半初始化对象
* **零开销**: 只是 sockaddr_in 的薄包装，sizeof(InetAddress) == sizeof(sockaddr_in)

## 8. 潜在问题

| 问题 | 描述 | 严重程度 |
|------|------|----------|
| 不支持 IPv6 | 仅 AF_INET | 中（v1 设计选择） |
| toIp 每次分配 string | 高频日志场景可能有开销 | 低 |

## 9. 面试角度总结

| 问题 | 答案要点 |
|------|----------|
| InetAddress 解决什么问题？ | 统一 IPv4 地址表示，避免散落的 sockaddr_in 操作 |
| 为什么是值语义？ | 地址只是 16 字节结构体，拷贝比间接引用更快 |
| 如何扩展支持 IPv6？ | 改用 sockaddr_storage 或 union { sockaddr_in, sockaddr_in6 } |
