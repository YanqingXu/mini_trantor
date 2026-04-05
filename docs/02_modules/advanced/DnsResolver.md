# DnsResolver 源码拆解

## 1. 类定位

* **角色**: 高级功能模块（Advanced）
* **层级**: Net 层辅助设施，连接 EventLoop 和系统 DNS
* DnsResolver 是 **异步域名解析器**——用独立工作线程池执行阻塞的 `getaddrinfo`，通过 `runInLoop` 将结果投递回请求方线程

## 2. 解决的问题

* **核心问题**: `getaddrinfo` 是阻塞调用，不能在 EventLoop 线程直接执行
* 如果没有 DnsResolver:
  - 在 EventLoop 线程做 DNS 解析会阻塞 Reactor
  - 用户需要自己管理工作线程和结果投递
  - 无法复用 DNS 缓存，重复解析浪费时间

## 3. 对外接口（API）

| 方法 | 场景 | 调用者 |
|------|------|--------|
| `DnsResolver(numThreads)` | 创建解析器，启动工作线程池 | 用户 |
| `~DnsResolver()` | 停止工作线程，等待其退出 | 自动 |
| `resolve(hostname, port, loop, cb)` | 异步解析 | TcpClient / 用户 |
| `enableCache(ttl)` | 开启 DNS 缓存 | 用户 |
| `clearCache()` | 清空缓存 | 用户 |
| `cacheEnabled()` | 查询缓存状态 | 用户 |
| `getShared()` | 全局共享实例（static） | TcpClient |

## 4. 核心成员变量

```
DnsResolver
├── workers_: vector<thread>               // 工作线程池
├── requestQueue_: queue<ResolveRequest>   // 待解析请求队列
├── queueMutex_: mutex                     // 保护请求队列
├── queueCv_: condition_variable           // 唤醒工作线程
├── stopping_: bool                        // 停止标志
├── cacheEnabled_: bool                    // 缓存开关
├── cacheTtl_: seconds                     // 缓存 TTL
├── cacheMutex_: mutex                     // 保护缓存表
├── cache_: unordered_map<string, CacheEntry>  // hostname → 缓存条目
```

### ResolveRequest

```
ResolveRequest
├── hostname: string
├── port: uint16_t
├── callbackLoop: EventLoop*      // 结果投递的目标 loop
├── callback: ResolveCallback     // 用户回调
```

### CacheEntry

```
CacheEntry
├── addresses: vector<sockaddr_in>                // 缓存地址（port=0）
├── expiry: steady_clock::time_point              // 过期时间
```

## 5. 执行流程

### 5.1 解析流程

```
用户调用 resolve("example.com", 80, loop, cb)
  │
  ├─ 1. 检查缓存:
  │     if (cacheEnabled_) {
  │       lock cacheMutex_
  │       if (cache hit && not expired):
  │         → 构造 InetAddress vector（设置 port）
  │         → callbackLoop->runInLoop(cb(result))
  │         → return（缓存命中，不入队）
  │     }
  │
  ├─ 2. 入队:
  │     lock queueMutex_
  │     → requestQueue_.push({hostname, port, loop, cb})
  │     → queueCv_.notify_one()
  │
  ├─ 3. 工作线程取出请求:
  │     workerThread():
  │       unique_lock + queueCv_.wait()
  │       → req = queue.front(); queue.pop()
  │
  ├─ 4. 阻塞解析:
  │     → getaddrinfo(hostname, port, hints, &res)
  │     → 遍历 addrinfo 链表，收集 AF_INET 地址
  │     → freeaddrinfo(res)
  │
  ├─ 5. 更新缓存:
  │     if (cacheEnabled_ && addrs not empty):
  │       lock cacheMutex_
  │       → cache_[hostname] = {addrs_with_port0, now + ttl}
  │
  └─ 6. 投递结果:
        → callbackLoop->runInLoop(cb(addresses))
        // 回调在请求方 EventLoop 线程执行
```

### 5.2 停止流程

```
~DnsResolver()
  → lock queueMutex_ → stopping_ = true
  → queueCv_.notify_all()
  → for each worker: worker.join()
  // 等待所有工作线程退出
```

### 5.3 线程模型图

```
  ┌──────────────┐     resolve()     ┌─────────────────┐
  │ EventLoop    │────────────────▶  │ Request Queue   │
  │ 线程 (调用者)│                   │ (mutex + cv)    │
  └──────────────┘                   └────────┬────────┘
        ▲                                     │
        │ runInLoop(cb)                       │ 取出请求
        │                                     ▼
  ┌──────────────┐               ┌─────────────────────┐
  │ EventLoop    │◀──────────────│   Worker Thread     │
  │ 线程 (回调)  │               │   getaddrinfo()     │
  └──────────────┘               └─────────────────────┘
```

## 6. 关键交互关系

```
┌───────────┐  resolve()   ┌─────────────┐   getaddrinfo   ┌──────────┐
│ TcpClient │─────────────▶│ DnsResolver │────────────────▶│ 系统 DNS │
│           │◀─────────────│             │◀────────────────│          │
└───────────┘  cb(addrs)   └──────┬──────┘                 └──────────┘
                                  │ runInLoop
                                  ▼
                           ┌─────────────┐
                           │ EventLoop   │
                           └─────────────┘
```

* **上游**: TcpClient (通过 getShared())，ResolveAwaitable
* **下游**: EventLoop (runInLoop 投递)，系统 getaddrinfo
* **线程安全**: 请求队列用 mutex+cv 保护，缓存用独立 mutex 保护

## 7. 关键设计点

### 7.1 线程池 + 队列模式

* 经典的 producer-consumer 模式
* 工作线程用 `condition_variable::wait` 阻塞等待
* 解析请求被投递到队列，工作线程竞争取出

### 7.2 缓存设计

* 缓存 key: hostname（不含 port）
* 缓存 value: `vector<sockaddr_in>` 存储时 port=0，查询时按请求 port 填充
* TTL 检查: `expiry > now()`

### 7.3 全局共享实例

```cpp
static shared_ptr<DnsResolver> getShared() {
    static auto instance = make_shared<DnsResolver>(2);
    return instance;
}
```

* Meyer's singleton，线程安全
* 2 个工作线程，适合大部分场景

## 8. 潜在问题

| 问题 | 描述 | 严重程度 |
|------|------|----------|
| cacheEnabled_ 非 atomic | 主线程 enableCache + 工作线程 read → 数据竞争 | 中 |
| 缓存不区分 IPv4/IPv6 | hints.ai_family = AF_INET，只缓存 IPv4 | 低（设计选择） |
| 无解析超时 | getaddrinfo 可能阻塞很久（30s+） | 中 |
| 全局实例生命周期 | static 对象的销毁顺序与 EventLoop 可能冲突 | 低 |

## 9. 面试角度总结

| 问题 | 答案要点 |
|------|----------|
| 为什么 DNS 解析要用独立线程池？ | getaddrinfo 是阻塞调用，不能在 Reactor 线程执行 |
| 结果如何安全投递回 EventLoop？ | 通过 callbackLoop->runInLoop()，回调在请求者线程执行 |
| 缓存的键值设计？ | key=hostname, value=addrs(port=0)，查询时按请求 port 填充 |
| 和 c-ares 的区别？ | c-ares 是基于 fd 的非阻塞 DNS，mini-trantor 用线程池+getaddrinfo 更简单 |
