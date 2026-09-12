# Module Intent: ConnectionAwaiterRegistry

> S1-01b implements owner-loop unregistration and invalidation of queued resumes.
> The remaining Task/combinator/DNS lifecycle work is tracked in
> [the execution record](../../docs/s1_coroutine_lifecycle.md).

## 1. Intent
ConnectionAwaiterRegistry coordinates coroutine waiters that suspend on one
TcpConnection for read readiness, write completion, or close completion.

Its purpose is to keep coroutine bookkeeping out of TcpConnection's core
transport/lifecycle code while preserving the project's rule that suspension
and resume semantics still flow through EventLoop scheduling.

---

## 2. Responsibilities
- store at most the allowed waiter state for read, write, and close operations
- arm waiters on the owner loop thread
- decide when buffered data, output drain, or close state is sufficient to resume
- resume coroutines through EventLoop queueing rather than arbitrary direct resume
- clear waiter state safely during connection close/error teardown
- report contract violations such as duplicate active waiter registration

---

## 3. Non-Responsibilities
- does not own TcpConnection lifecycle
- does not perform socket or SSL I/O
- does not invent a separate coroutine scheduler
- does not expose transport state directly to user code

---

## 4. Core Invariants
- S1-01b: a waiter has an explicit Unarmed/Pending/Queued/Completed/Abandoned
  lifecycle. Queued callbacks share the operation state, not ownership of the frame.
- Unregistering an abandoned waiter removes its slot without resuming it;
  queued completions must check the state before touching the borrowed handle.
- A queued waiter still reserves its read/write/close slot until await_resume
  consumes completion or destruction unregisters it. Readiness is not consumption.
- Cancellation is an explicit outcome; it cannot overwrite a completion that
  has already won. A pre-cancelled token wins before I/O submission.
- one registry belongs to exactly one TcpConnection and one owner loop
- waiter registration and waiter state mutation happen on the owner loop thread
- coroutine resumption returns through EventLoop scheduling
- close/error teardown resumes and clears relevant waiters exactly once
- duplicate waiter registration is rejected explicitly rather than silently overwritten

---

## 5. Collaboration
- TcpConnection owns ConnectionAwaiterRegistry
- TcpConnection notifies the registry after read progress, write drain, and close
- awaitable facade objects may remain nested under TcpConnection or move later,
  but their storage and resume policy live in the registry
- registry queries connection-visible state such as inputBuffer, outputBuffer,
  and public disconnected state through narrow helper methods

---

## 6. Threading Rules
- await_ready fast paths must not inspect loop-owned mutable state off-thread
- await_suspend marshals to the owner loop when needed
- registry-triggered resume is always queued onto the owner loop
- cross-thread coroutine callers interact only through TcpConnection awaitables

---

## 7. Ownership Rules
- TcpConnection owns the registry
- awaitable facade objects may hold shared_ptr<TcpConnection> to preserve safe lifetime
- the registry never owns the coroutine task object beyond the stored handle needed
  for resume/cancel-on-close behavior

---

## 8. Failure Semantics
- disconnected-without-data is surfaced explicitly to read waiters
- duplicate waiter arming throws a logic error
- close path resumes pending waiters and clears their state even if the close was
  triggered by error or force-close

---

## 9. Extension Points
- cancellation and owner-loop destruction have distinct completion/unregistration paths
- future richer read/write completion result types
- future observability for waiter counts and resume reasons

---

## 10. Test Contracts
- `tests/contract/coroutine/test_tcp_awaitable_lifetime.cpp`: destruction before
  readiness, after completion/cancel was queued, write already drained vs blocked,
  and replacement read after an earlier waiter was destroyed.
- read waiter resumes on owner loop when enough bytes arrive
- read waiter observes explicit peer-closed result when connection closes empty
- write waiter resumes on owner loop after output drain
- close waiter resumes on owner loop after connection teardown
- duplicate read or write waiter registration is rejected
- waiters are resumed exactly once on close/error path

---

## 11. Review Checklist
- Is waiter state isolated from core transport code?
- Are resumes still queued through EventLoop?
- Can close/error clear all waiters without leaking stale handles?
- Are duplicate waiter violations still explicit?
