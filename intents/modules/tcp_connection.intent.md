# Module Intent: TcpConnection

## 1. Intent
TcpConnection models one TCP connection bound to one EventLoop.
It owns per-connection buffers, socket wrapper, channel registration,
and callback/coroutine handoff points while preserving reactor semantics.

---

## 2. Responsibilities
- own connection-local socket/channel/buffer state
- translate read/write/close/error events into unified connection state changes
- expose send/shutdown APIs that preserve owner-thread execution
- expose force-close style teardown entry that still converges on the same close path
- enforce optional per-connection read-throttling when outbound bytes exceed configured thresholds
- coordinate coroutine waiters without bypassing EventLoop scheduling

---

## 3. Non-Responsibilities
- does not own EventLoop
- does not own TcpServer
- does not invent a separate coroutine scheduler
- does not perform application protocol parsing

---

## 4. Core Invariants
- one TcpConnection belongs to exactly one EventLoop
- connection state mutation happens on owner loop thread
- close and error teardown converge on one safe path
- channel registration is removed before effective destruction
- backpressure-driven read suspend/resume only changes Channel interest on owner loop thread
- coroutine resume returns through EventLoop scheduling

---

## 5. Threading Rules
- handleRead/handleWrite/handleClose/handleError run on owner loop thread
- cross-thread send/shutdown must marshal back into the loop
- backpressure threshold evaluation and read-interest toggling happen on the owner loop
- await readiness checks must not inspect loop-owned mutable state off-thread

---

## 6. Failure Semantics
- fatal read/write errors should produce explicit teardown
- repeated close/error handling should be guarded or idempotent
- timeout-driven close should reuse the normal close path rather than inventing a side channel
- backpressure throttling should resume automatically after the output buffer drains below the low-water threshold
- disconnected state should block unsafe user-visible actions

---

## 7. Extension Points
- coroutine read/write/close awaiters
- optional high-water-mark notification callback
- future backpressure metrics

---

## 8. Test Contracts
- cross-thread send executes on owner loop thread
- read/write error path converges on safe close handling
- cross-thread force-close marshals back to the owner loop and converges on safe close handling
- high-water to low-water drain path pauses and resumes read processing on the owner loop
- coroutine awaiters resume through EventLoop rather than arbitrary caller thread
- repeated teardown does not leave stale registration behind

---

## 9. Review Checklist
- Is all mutable connection state still loop-owned?
- Do close and error share one teardown model?
- Can callbacks or coroutine resumes outlive safe ownership?
- Is registration removed before destruction?
