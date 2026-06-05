# Architecture Intent: v1 Stage Boundary

## 1. Intent
mini-trantor v1 is intentionally staged.
The goal is to freeze correctness boundaries in order:
1. synchronous Reactor mainline
2. threading model
3. coroutine bridge

This intent exists so roadmap discussions, implementation scope, review scope,
and test scope all refer to the same boundary.

---

## 2. v1-alpha
### Goal
Synchronous Reactor mainline is stable.

### In Scope
- Channel
- Poller / EPollPoller
- EventLoop
- wakeup
- queueInLoop / runInLoop
- Buffer
- Acceptor
- TcpConnection
- TcpServer
- callback-based echo server main path

### Required Guarantees
- fd registration/dispatch is stable
- same-thread and cross-thread scheduling semantics are explicit
- connection read/write/close path converges safely
- synchronous echo server main path runs through end-to-end

### Exit Signals
- unit tests for local module invariants
- contract tests for EventLoop / Poller / TcpConnection
- integration test for synchronous TcpServer echo path

---

## 3. v1-beta
### Goal
Thread model is stable.

### In Scope
- EventLoopThread
- EventLoopThreadPool
- one-loop-per-thread expansion
- cross-thread task submission discipline
- base-loop to io-loop handoff

### Required Guarantees
- loop-owned mutable state stays on owner thread
- allowed cross-thread operations use explicit marshaling
- worker loop publication and teardown are stable
- no hidden shared mutable connection state is introduced

### Exit Signals
- contract tests for cross-thread queue/wakeup behavior
- contract tests for EventLoopThreadPool startup and loop selection
- integration tests still pass with threaded deployment where applicable

---

## 4. v1-coro-preview
### Goal
Coroutine bridge runs through without changing Reactor semantics.

### In Scope
- `mini::coroutine::Task`
- `TcpConnection::asyncReadSome`
- `TcpConnection::asyncWrite`
- `TcpConnection::waitClosed`
- coroutine echo main path

### Required Guarantees
- coroutine awaiters do not bypass EventLoop scheduling
- resume happens on the appropriate owner loop
- close/error paths still wake/resume waiters safely
- coroutine mode remains a bridge on top of Reactor, not a separate scheduler

### Exit Signals
- unit tests for `Task`
- contract tests for connection lifecycle still pass
- integration test for coroutine echo path runs through successfully

---

## 5. Out of Scope Before v1-coro-preview Closes
- TimerQueue
- async timer APIs
- full backpressure policy
- protocol stacks above TCP
- standalone coroutine scheduler
- complex cancellation tree

---

## 6. Review Questions
- Which stage boundary does this change belong to?
- Does it strengthen the current stage, or leak work from a later stage?
- Which test layer proves the intended stage contract?
