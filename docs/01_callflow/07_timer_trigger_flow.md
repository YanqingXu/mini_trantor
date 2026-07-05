# 定时器触发流程

## 用户接口

```cpp
// 延迟执行
auto timerId = loop->runAfter(3.0, [] { LOG_INFO << "timeout!"; });

// 定期执行
auto timerId = loop->runEvery(1.0, [] { heartbeat(); });

// 取消定时器
loop->cancelTimer(timerId);
```

## 添加定时器流程

```
loop->runAfter(delay, cb)
  → timerQueue_->addTimer(cb, when, interval)
      → runInLoop(addTimerInLoop)          // 确保在 loop 线程执行

addTimerInLoop():
  → auto timer = make_unique<Timer>(cb, when, interval)
  → timers_.insert({TimerKey{when, sequence++}, move(timer)})
  → return TimerId(timer.get(), sequence)
```

## 定时器触发流程

```
EventLoop::loop()
  → timerQueue_->pollTimeoutMs(defaultTimeout)
  → poller_->poll(timeoutMs, activeChannels)
  → dispatch active Channel callbacks
  → timerQueue_->handleExpired(now)
```

```cpp
void TimerQueue::handleExpired(Timestamp now) {
    Timestamp now = Timestamp::now();

    // 1. 收集所有已到期的定时器
    auto expired = getExpired(now);

    // 2. 逐个执行回调
    for (const auto& [key, timer] : expired) {
        timer->run();               // 执行用户回调
    }

    // 3. 重新调度重复定时器
    reset(expired, now);
}
```

## getExpired —— 收集已到期定时器

```cpp
std::vector<Entry> TimerQueue::getExpired(Timestamp now) {
    // TimerKey = {Timestamp, int64_t sequence}
    // 用 INT64_MAX 作为 sequence 上界，确保取出所有 <= now 的定时器
    TimerKey sentry{now, INT64_MAX};

    auto end = timers_.lower_bound(sentry);  // 第一个 > now 的位置
    // timers_ begin..end 就是所有已到期的

    std::vector<Entry> expired(
        std::make_move_iterator(timers_.begin()),
        std::make_move_iterator(end));
    timers_.erase(timers_.begin(), end);

    return expired;
}
```

## reset —— 重新调度重复定时器

```cpp
void TimerQueue::reset(const std::vector<Entry>& expired, Timestamp now) {
    for (const auto& [key, timer] : expired) {
        if (timer->repeat() && !timer->cancelled()) {
            timer->restart(now);       // 更新到期时间 = now + interval
            insert(std::move(timer));  // 重新插入 timers_
        }
        // 非重复定时器自动销毁（unique_ptr）
    }

    // 下一轮 EventLoop::poll 前会重新计算最近超时
}
```

## 取消定时器

```
loop->cancelTimer(timerId)
  → timerQueue_->cancelInLoop(timerId)
      → 找到对应 Timer → timer->cancel()  // 标记 cancelled
      → 从 timers_ 中 erase
```

取消操作在 loop 线程执行，避免竞争。已进入 `getExpired` 的定时器在 reset 阶段通过 `cancelled()` 检查跳过。

## 时间线示意

```
时间轴 ──────────────────────────────────────────►

  t=0     t=1     t=2     t=3     t=4     t=5
   │       │       │       │       │       │
   ├─ addTimer(cb1, t=2)                   │
   ├─ addTimer(cb2, t=3, repeat=1s)        │
   │       │       │       │       │       │
   │       │    poll timeout        │       │
   │       │    returns ─►          │       │
   │       │    cb1()      │       │       │
   │       │       │    poll timeout        │
   │       │       │    returns ─►          │
   │       │       │    cb2()     │       │
   │       │       │    reset cb2 │       │
   │       │       │    to t=4  poll timeout
   │       │       │       │   returns ─► │
   │       │       │       │   cb2()      │
   │       │       │       │   reset to t=5
```

## TimerQueue 与 EventLoop wakeup 的对比

| 特性 | TimerQueue poll timeout | EventLoop wakeup |
|------|--------------------------|------------------|
| 用途 | 定时触发 | 跨线程任务唤醒 |
| 触发方式 | Poller 等待到最近 timer 到期 | 其他线程写入 wakeup fd/socket |
| 执行线程 | owner EventLoop | owner EventLoop |
| 本项目使用 | `runAt/runAfter/runEvery/cancel` | `runInLoop/queueInLoop/quit` |
