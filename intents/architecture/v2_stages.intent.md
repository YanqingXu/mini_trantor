# Architecture Intent: v2 Stage Boundary

## 1. Intent
mini-trantor v2 extends the stable v1 Reactor foundation with client-side
networking and coroutine-friendly timer APIs.
The same staging discipline applies: freeze correctness boundaries in order,
with explicit exit signals before advancing.

v2 assumes v1-coro-preview is closed and all v1 exit signals are met.

---

## 2. v2-alpha
### Goal
TcpClient main path is stable.

### In Scope
- Connector (active-connect adapter, symmetric counterpart to Acceptor)
- TcpClient
- non-blocking connect with EINPROGRESS handling
- configurable reconnect backoff strategy
- TcpClient + TcpConnection collaboration after connect succeeds
- client-side connection callback / message callback / close callback
- cross-thread connect / disconnect / send through EventLoop scheduling

### Required Guarantees
- connect / fail / retry paths converge safely without fd leak or stale Channel
- Connector obeys owner-loop-only Channel mutation discipline
- TcpClient owns Connector and (post-connect) TcpConnection shared_ptr
  with clear lifecycle boundaries
- cross-thread connect and disconnect marshal through runInLoop
- reconnect backoff is explicitly configurable, not hardcoded
- TcpClient destruction safely tears down Connector and TcpConnection
  Channel registrations on the owner loop thread
- client-side echo round-trip works end-to-end against a TcpServer

### Exit Signals
- unit tests for Connector local invariants (state machine, backoff calculation)
- contract tests for Connector connect / fail / retry on owner loop thread
- contract tests for TcpClient lifecycle (connect, disconnect, reconnect, destroy)
- contract tests for cross-thread connect and disconnect marshaling
- integration test for TcpClient ↔ TcpServer echo round-trip
- integration test for TcpClient reconnect after server restart

---

## 3. v2-beta
### Goal
Async timer API is stable.

### In Scope
- coroutine-friendly timer awaitable on EventLoop
  (e.g. `co_await loop->asyncSleep(duration)`)
- timer awaitable implementation built on existing TimerQueue infrastructure
- integration with TcpConnection timeout scenarios
  (e.g. coroutine-based per-connection idle timeout)
- async delay utility for coroutine-based connection handlers

### Required Guarantees
- async timer awaitable does not bypass EventLoop scheduling semantics
- timer awaitable resume happens on the owner loop thread
- cancel / teardown of a pending timer awaitable is safe
  (coroutine handle is not leaked or double-resumed)
- async timer composes naturally with existing TcpConnection awaitables
  inside a Task coroutine
- TimerQueue ownership and lifecycle rules remain unchanged

### Exit Signals
- unit tests for timer awaitable (ready / suspend / resume / cancel paths)
- contract tests for timer awaitable resume on owner loop thread
- contract tests for timer awaitable cancel during pending wait
- integration test for coroutine echo handler with per-connection idle timeout
  using async timer

---

## 4. Out of Scope Before v2-beta Closes
- TLS / SSL integration
- HTTP or any protocol stack above TCP
- standalone coroutine scheduler independent of EventLoop
- complex cancellation trees or structured concurrency primitives
- DNS resolver
- UDP support
- full backpressure policy framework beyond the existing per-connection model
- signal handling infrastructure

---

## 5. Review Questions
- Which stage boundary does this change belong to?
- Does it strengthen the current stage, or leak work from a later stage?
- Which test layer proves the intended stage contract?
- Does this change introduce any dependency on v2 infrastructure
  that would break v1 guarantees if reverted?
