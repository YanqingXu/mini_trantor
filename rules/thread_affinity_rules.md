# thread_affinity_rules.md

## 1. Core Principle
mini-trantor follows a reactor-style thread-affinity model.
Mutable reactor state belongs to a specific EventLoop thread.

## 2. EventLoop
- Each EventLoop has exactly one owner thread
- loop() must run only on the owner thread
- Poller operations must run on the owner thread
- pending functors are executed on the owner thread

## 3. Allowed Cross-Thread Interaction
Cross-thread interaction must go through:
- runInLoop(...)
- queueInLoop(...)
- wakeup mechanism

No other direct mutation path is allowed for core loop state.

## 4. runInLoop
- If current thread == owner thread: execute immediately
- Else: enqueue and wakeup loop if needed

## 5. queueInLoop
- Always enqueue
- Must ensure wakeup when loop may be blocked in poll

## 6. Channel
- Channel update/remove must occur on its owning EventLoop thread
- handleEvent executes in EventLoop thread unless explicitly documented otherwise

## 7. TcpConnection
- State transitions occur on owning EventLoop thread
- Actual socket write/read handling occurs on owning EventLoop thread
- Cross-thread send request must be marshaled back into loop thread

## 8. Wakeup
- Wakeup write can be invoked cross-thread
- Wakeup read/clear is handled in loop thread
- Wakeup is a scheduling signal, not business event delivery

## 9. Forbidden
- Direct Poller mutation from non-owner thread
- Direct Channel mutation that changes registration from non-owner thread
- User callback execution in ambiguous thread context
- “Occasionally safe” thread behavior without rule support