# Poller —— 工程级源码拆解

## 1. 类定位

* **角色**：I/O 多路复用的**抽象基类**，定义 poll/updateChannel/removeChannel 接口
* **层级**：Reactor 层（核心底层）
* EventLoop 通过 Poller 接口与具体后端（EPollPoller）交互

## 2. 解决的问题

将具体的多路复用实现（epoll/poll/kqueue）与 EventLoop 解耦。
当前只有 EPollPoller 一个实现，但接口预留了扩展能力。

## 3. 对外接口

| 方法 | 用途 | 调用者 |
|------|------|--------|
| `poll(timeoutMs, activeChannels)` | 等待 I/O 事件，填充活跃 Channel | EventLoop::loop() |
| `updateChannel(channel)` | 注册/更新 Channel 的事件监听 | EventLoop::updateChannel() |
| `removeChannel(channel)` | 从多路复用器中移除 Channel | EventLoop::removeChannel() |
| `hasChannel(channel)` | 检查 Channel 是否已注册 | EventLoop::hasChannel() |
| `newDefaultPoller(loop)` | 静态工厂，创建默认 Poller 实例 | EventLoop 构造函数 |

## 4. 核心成员变量

```cpp
ChannelMap channels_;       // fd → Channel* 映射（子类使用）
EventLoop* ownerLoop_;      // 所属 EventLoop（不拥有）
```

## 5. 执行流程

Poller 是纯接口，具体流程见 EPollPoller。

## 6. 关键设计点

* **策略模式**：EventLoop 持有 `unique_ptr<Poller>`，通过虚函数调用具体实现
* **channels_ 在基类**：避免每个子类重复维护 fd→Channel 映射

## 7. 面试角度

**Q: 为什么 Poller 是抽象类而不是直接用 EPollPoller？**
A: 面向接口编程，预留跨平台扩展（如 macOS 上用 kqueue）。

---

# EPollPoller —— 工程级源码拆解

## 1. 类定位

* **角色**：Poller 的 Linux epoll 后端实现
* **层级**：Reactor 层（核心底层）
* 直接调用 `epoll_create1`、`epoll_ctl`、`epoll_wait`

## 2. 解决的问题

把 Linux epoll 的系统调用封装为 Poller 接口。处理：
- Channel 状态三态管理（kNew/kAdded/kDeleted）
- events 数组自动扩容
- 错误处理（abort on fatal）

## 3. 对外接口

继承 Poller 的 poll/updateChannel/removeChannel。

## 4. 核心成员变量

```cpp
int epollfd_;                        // epoll 文件描述符
std::vector<epoll_event> events_;    // epoll_wait 结果数组（初始 16 个）
```

状态常量：
```cpp
static constexpr int kNew = -1;      // Channel 从未注册
static constexpr int kAdded = 1;     // Channel 在 epoll 中
static constexpr int kDeleted = 2;   // Channel 已从 epoll 删除，但仍在 channels_ map 中
```

## 5. 执行流程

### poll

```cpp
int numEvents = epoll_wait(epollfd_, events_.data(), events_.size(), timeoutMs);
if (numEvents > 0) {
    fillActiveChannels(numEvents, activeChannels);
    if (numEvents == events_.size()) {
        events_.resize(events_.size() * 2);   // 自动扩容
    }
}
```

### updateChannel 状态机

```
Channel::index 当前值  │  Channel 事件状态  │  操作
──────────────────────┼──────────────────┼───────────────
kNew (-1)             │  有事件          │  EPOLL_CTL_ADD → kAdded
kDeleted (2)          │  有事件          │  EPOLL_CTL_ADD → kAdded
kAdded (1)            │  有事件          │  EPOLL_CTL_MOD
kAdded (1)            │  无事件          │  EPOLL_CTL_DEL → kDeleted
```

### removeChannel

```
removeChannel(channel):
  ├─ channels_.erase(fd)
  ├─ if (kAdded) → epoll_ctl(DEL)
  └─ setIndex(kNew)
```

## 6. 关键设计点

### events 数组自动扩容

```cpp
if (static_cast<std::size_t>(numEvents) == events_.size()) {
    events_.resize(events_.size() * 2);
}
```

避免预分配过大数组，连接数增长时自动适应。

### kDeleted 状态的意义

当 Channel `disableAll()` 后，从 epoll 中删除（DEL），但保留在 `channels_` map 中。
如果 Channel 之后重新 `enableReading()`，可以直接 `EPOLL_CTL_ADD` 而不需要重新加入 map。

### abort 式错误处理

epoll_create/epoll_ctl/epoll_wait 失败直接 abort。
这些都是不可恢复的系统错误。

## 7. 面试角度

**Q: epoll 的 LT 和 ET 模式，mini-trantor 用哪个？**
A: LT（水平触发）。注册 EPOLLIN/EPOLLOUT 不带 EPOLLET。LT 更简单安全，不会遗漏事件。

**Q: events 数组为什么要自动扩容？**
A: 初始 16 个。如果某次 epoll_wait 返回的事件数等于数组大小，说明可能有更多事件等待，翻倍扩容。

**Q: kNew/kAdded/kDeleted 三态有什么用？**
A: 避免重复 EPOLL_CTL_ADD/DEL。kDeleted 允许 Channel 被重新激活而不需要重建 channels_ map 条目。
