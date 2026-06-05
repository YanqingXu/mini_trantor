# TimerQueue —— 工程级源码拆解

## 1. 类定位

* **角色**：为单个 EventLoop 提供**基于 timerfd 的定时任务管理**
* **层级**：Reactor 层（核心底层）
* 支持 one-shot 和 repeating 定时器，回调在 owner loop 线程执行

## 2. 解决的问题

**核心问题**：如何在 Reactor 框架中高效地管理多个定时器？

选择 timerfd 而非 poll timeout 的原因：
- timerfd 作为 fd 可以统一加入 epoll，不需要特殊逻辑
- 纳秒精度
- 多个 timer 但只需一个 timerfd（只设置最近的超时时间）

## 3. 对外接口

| 方法 | 用途 | 线程安全 |
|------|------|----------|
| `addTimer(cb, when, interval)` | 添加定时器 | 跨线程安全 |
| `cancel(timerId)` | 取消定时器 | 跨线程安全 |

## 4. 核心成员变量

```cpp
EventLoop* loop_;                              // 所属 EventLoop
const int timerfd_;                            // timerfd 文件描述符
std::unique_ptr<Channel> timerfdChannel_;      // timerfd 的 Channel
std::atomic<std::int64_t> nextSequence_;       // 定时器 ID 递增计数器（跨线程安全）
TimerMap timers_;                              // {Timestamp, sequence} → TimerPtr 有序映射
std::unordered_map<int64_t, TimerPtr> timersById_;  // sequence → TimerPtr 快速查找
```

### Timer 结构

```cpp
struct Timer {
    TimerCallback callback;          // 到期回调
    Timestamp expiration;            // 到期时间
    Duration interval;               // 重复间隔（0 = one-shot）
    int64_t sequence;                // 唯一 ID
    bool canceled{false};            // 是否已取消
    bool inQueue{true};              // 是否在 timers_ 中
};
```

## 5. 执行流程

### 5.1 添加定时器

```
addTimer(cb, when, interval):
  ├─ timer = make_shared<Timer>(cb, when, interval, nextSequence_++)
  ├─ timerId = TimerId(timer->sequence)
  ├─ isInLoopThread()? → addTimerInLoop(timer)
  │   else → runInLoop(addTimerInLoop(timer))
  └─ return timerId

addTimerInLoop(timer):
  ├─ timer->inQueue = true
  ├─ timersById_[seq] = timer
  ├─ timers_[{when, seq}] = timer
  └─ if 新 timer 是最早的:
      └─ resetTimerfd(when)  → timerfd_settime()
```

### 5.2 定时器触发

```
timerfd 到期 → EPOLLIN
  → Channel::handleEvent
  → TimerQueue::handleRead():
      ├─ readTimerfdOrDie(timerfd_)        // 消费 timerfd 数据
      ├─ expired = getExpired(now)          // 收集已到期 timer
      ├─ for timer in expired:
      │   if (!timer->canceled):
      │       timer->callback()             // 执行回调
      └─ reset(expired, now)                // 重新调度重复 timer
```

### 5.3 getExpired

```cpp
std::vector<TimerPtr> getExpired(Timestamp now) {
    TimerKey sentry{now, INT64_MAX};       // 上界哨兵
    auto end = timers_.upper_bound(sentry); // 第一个 > now 的位置
    // timers_.begin() → end 就是所有已到期的
    for (it = begin; it != end; ++it) {
        it->second->inQueue = false;
        expired.push_back(it->second);
    }
    timers_.erase(begin, end);
    return expired;
}
```

### 5.4 reset（重复定时器）

```cpp
void reset(expired, now) {
    for (timer : expired) {
        if (timer->repeat() && !timer->canceled && timersById_ 中还存在) {
            timer->expiration = now + timer->interval;
            insert(timer);           // 重新插入 timers_
        } else {
            timersById_.erase(seq);  // one-shot 或已取消，清理
        }
    }
    // 更新 timerfd 为下一个最早的
    if (!timers_.empty())
        resetTimerfd(timers_.begin()->expiration);
    else
        disarmTimerfd();             // 无定时器，解除 timerfd
}
```

### 5.5 取消定时器

```
cancelInLoop(timerId):
  ├─ 在 timersById_ 中查找
  ├─ timer->canceled = true
  ├─ if (timer->inQueue):
  │   ├─ timers_.erase({expiration, seq})
  │   └─ timer->inQueue = false
  │   └─ timersById_.erase(seq)
  │   └─ 更新 timerfd（如果影响了最早的 timer）
  └─ else:
      └─ timersById_.erase(seq)      // 已被 getExpired 取出
```

## 6. 关键交互关系

```
EventLoop
  │ owns
  ▼
TimerQueue
  │ owns
  ├─ timerfd (fd)
  ├─ timerfdChannel_ (Channel)
  └─ timers_ (map of Timer)
```

| 类 | 关系 |
|----|------|
| **EventLoop** | 拥有 TimerQueue，调用 addTimer/cancel |
| **Channel** | TimerQueue 拥有 timerfdChannel_ |
| **TcpServer** | 间接使用（IdleTimeout 通过 EventLoop::runAfter） |

## 7. 关键设计点

### 双索引结构

- `timers_`：`map<{Timestamp, seq}, TimerPtr>`，按到期时间排序，支持高效 getExpired
- `timersById_`：`unordered_map<seq, TimerPtr>`，支持 O(1) cancel

### TimerKey 包含 sequence

同一时刻可能有多个 timer 到期。`{Timestamp, sequence}` 保证唯一性和稳定顺序。

### canceled + inQueue 双标志

- `canceled`：标记已取消（即使已在 getExpired 返回列表中也能跳过）
- `inQueue`：标记是否在 timers_ 中（避免重复 erase）

### shared_ptr<Timer>

Timer 可能同时被 timers_、timersById_、getExpired 返回的 vector 持有，
shared_ptr 保证不会悬空。

## 8. 潜在问题

### 大量定时器的性能

`std::map` 的 insert/erase 是 O(log n)。对于数万个定时器，可考虑时间轮（timing wheel）。
当前 v1 追求正确性，map 足够。

### 系统时钟调整

使用 `CLOCK_MONOTONIC`，不受系统时间调整影响。

## 9. 极简实现

```cpp
class MinimalTimerQueue {
    int timerfd_;
    std::map<Timestamp, std::function<void()>> timers_;
public:
    void addTimer(std::function<void()> cb, Timestamp when) {
        timers_[when] = cb;
        // timerfd_settime to earliest
    }
    void handleRead() {
        auto now = Timestamp::now();
        auto end = timers_.upper_bound(now);
        for (auto it = timers_.begin(); it != end; ++it)
            it->second();
        timers_.erase(timers_.begin(), end);
    }
};
```

## 10. 面试角度

**Q: 为什么用 timerfd 而不是 epoll_wait 的 timeout？**
A: timerfd 统一在 epoll 事件框架中处理，不需要特殊逻辑。多个 timer 只需一个 timerfd（设置最近的超时）。

**Q: 如何处理同一时刻多个 timer 到期？**
A: TimerKey 是 `{Timestamp, sequence}`，sequence 保证唯一性。getExpired 用 upper_bound 一次取出所有。

**Q: 取消一个已经 fire 但还没执行回调的 timer？**
A: getExpired 后、callback 执行前检查 `timer->canceled`。cancelInLoop 中设置 canceled=true，如果 timer 已不在 map 中（inQueue=false），只清理 timersById_ 索引。
