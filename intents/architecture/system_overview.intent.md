# System Intent: mini-trantor Overview

## 1. Intent
mini-trantor is a small reactor-style C++ network library inspired by trantor.
Its purpose is to provide a minimal but structurally correct networking foundation
for learning, extension, coroutine integration, and AI-assisted engineering.

This project is not intended to be a feature-complete production framework in v1.
Its primary value is architectural correctness and evolvability.

---

## 2. Core Architectural Style
- Reactor pattern
- Event-driven I/O
- One EventLoop per thread
- Poller-backed active event dispatch
- Channel as fd-event binding abstraction
- Explicit lifecycle and thread-affinity rules
- Coroutine-ready extension design, but not coroutine-first core

---

## 3. Core Modules
### Mandatory v1 foundation modules
- Channel
- Poller
- EPollPoller
- EventLoop
- Buffer
- Acceptor
- TcpConnection
- TcpServer
- EventLoopThread
- EventLoopThreadPool
- Coroutine adapters / awaitables
- Task abstraction

### Deferred until after v1-coro-preview
- TimerQueue
- Async timers
- Metrics/tracing hooks
- Backpressure policies

### Staged boundary
- `v1-alpha`: synchronous Reactor mainline is stable
- `v1-beta`: threading model is stable
- `v1-coro-preview`: coroutine bridge runs through on top of Reactor semantics

Detailed stage contracts are defined in `v1_stages.intent.md`.

---

## 4. Architectural Priorities
Priority order in v1:
1. lifecycle safety
2. thread-affinity correctness
3. API clarity
4. debuggability
5. extensibility
6. performance optimization

Performance matters, but not at the cost of unclear ownership or hidden thread behavior.

---

## 5. Core Invariants
- Each EventLoop is bound to exactly one thread
- Poller is only used by its owner EventLoop
- Channel belongs to exactly one EventLoop
- registration state must remain consistent between Poller and Channel lifecycle
- cross-thread mutation of core loop state is forbidden except via approved scheduling APIs
- lifecycle-sensitive callbacks must not run on already-destroyed upper-layer objects

---

## 6. Threading Model Summary
- single EventLoop owns mutable loop state
- cross-thread requests are marshaled back into loop thread
- wakeup mechanism is used to interrupt poll wait when necessary
- loop-thread discipline replaces widespread locking in core path

Detailed rules are defined in threading_model.intent.md and thread_affinity_rules.md.

---

## 7. Ownership Model Summary
- EventLoop owns Poller
- Poller does not own Channel
- Channel does not own fd by default
- upper-layer objects own business semantics
- lower-layer objects should not assume business object lifetime

Detailed rules are defined in lifetime_rules.intent.md and ownership_rules.md.

---

## 8. Public API Philosophy
Public APIs should:
- be narrow
- map clearly to reactor semantics
- preserve owner-thread execution guarantees
- be testable by contract
- avoid exposing backend-specific complexity

---

## 9. Failure Semantics
The core should distinguish:
- programming contract violations
- runtime I/O failures
- backend registration failures
- recoverable vs non-recoverable states

v1 should prefer explicit logging + safe shutdown/guard behavior
over overly abstract error modeling.

---

## 10. Non-Goals for v1
- full cross-platform backend support
- SSL/TLS
- full HTTP/WebSocket protocol layer
- high-level business RPC framework
- complex coroutine cancellation graph
- lock-free everywhere design

---

## 11. Expected Learning Outcome
After v1, a reader should be able to understand:
- reactor architecture
- fd event registration and dispatch
- thread-affinity discipline
- connection lifecycle management
- how coroutine integration can be layered on top safely

---

## 12. Review Questions
- Does this module fit reactor architecture cleanly?
- Does this change preserve v1 priorities?
- Does it introduce backend leakage?
- Does it create lifecycle ambiguity?
- Does it damage future coroutine integration points?
