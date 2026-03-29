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
- coroutine resume returns through EventLoop scheduling

---

## 5. Threading Rules
- handleRead/handleWrite/handleClose/handleError run on owner loop thread
- cross-thread send/shutdown must marshal back into the loop
- await readiness checks must not inspect loop-owned mutable state off-thread

---

## 6. Failure Semantics
- fatal read/write errors should produce explicit teardown
- repeated close/error handling should be guarded or idempotent
- disconnected state should block unsafe user-visible actions

---

## 7. Extension Points
- coroutine read/write/close awaiters
- future high-water-mark policy
- future backpressure metrics

---

## 8. Test Contracts
- cross-thread send executes on owner loop thread
- read/write error path converges on safe close handling
- coroutine awaiters resume through EventLoop rather than arbitrary caller thread
- repeated teardown does not leave stale registration behind

---

## 9. Review Checklist
- Is all mutable connection state still loop-owned?
- Do close and error share one teardown model?
- Can callbacks or coroutine resumes outlive safe ownership?
- Is registration removed before destruction?
