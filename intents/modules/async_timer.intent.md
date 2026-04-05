# Module Intent: SleepAwaitable / asyncSleep

## 1. Intent
SleepAwaitable is a coroutine awaitable that suspends the current coroutine
for a specified duration, then resumes it on the owner EventLoop thread.
It is built on top of the existing TimerQueue infrastructure via
`EventLoop::runAfter`, and lives in `mini/coroutine/` as a bridge utility —
not a scheduler.

---

## 2. Responsibilities
- suspend the calling coroutine for a given duration
- register a one-shot timer via EventLoop::runAfter
- resume the coroutine on the owner loop thread when the timer fires
- support cancellation that safely resumes the coroutine (no handle leak)

---

## 3. Non-Responsibilities
- does not own or manage EventLoop
- does not own or modify TimerQueue internals
- does not implement repeating timers (use runEvery for that)
- does not implement a standalone coroutine scheduler
- does not implement structured concurrency or cancellation trees
- does not provide select/race semantics across multiple awaitables

---

## 4. Core Invariants
- SleepAwaitable is a transient stack object; it does not outlive the
  co_await expression
- await_ready() always returns false: a timer must be registered to expire
- resume always happens on the owner EventLoop thread
  (guaranteed by EventLoop::runAfter callback semantics)
- the coroutine handle is resumed exactly once on all paths:
  either by timer expiry or by explicit cancellation
- after cancel, the handle is resumed immediately on the owner loop thread
  with a "cancelled" indication (await_resume returns false)
- TimerQueue ownership and lifecycle rules are not changed

---

## 5. Collaboration
- uses EventLoop::runAfter to register the timer
- uses EventLoop::cancel to cancel a pending timer
- uses EventLoop::queueInLoop to safely resume the handle after cancel
- composes with Task<T> via co_await inside any Task coroutine
- composes alongside TcpConnection awaitables in the same coroutine body

---

## 6. Threading Rules
- asyncSleep must be called from a coroutine running on the target
  EventLoop's owner thread (or the await_suspend will marshal correctly
  via runAfter which handles cross-thread)
- timer callback (which resumes the coroutine) executes on the owner
  loop thread — this is guaranteed by TimerQueue
- cancel may be called cross-thread; it uses EventLoop::cancel which
  marshals via runInLoop internally

---

## 7. Ownership Rules
- SleepAwaitable borrows an EventLoop pointer; it does not own it
- SleepAwaitable stores a TimerId for cancellation; this is a value handle
- the coroutine handle stored during await_suspend is borrowed and must
  be resumed exactly once
- SleepAwaitable itself is a temporary — its lifetime is bounded by the
  co_await expression in the coroutine frame

---

## 8. Failure Semantics
- if the EventLoop quits before the timer fires, the timer callback
  will not execute and the coroutine handle will not be resumed;
  this is the same behavior as any pending functor when the loop exits
- cancel of an already-fired timer is a safe no-op (TimerQueue guarantees this)
- double-resume is prevented: cancel removes the timer before queueing
  a resume, and the timer callback checks cancellation state

---

## 9. Extension Points
- future: await_resume could return a status enum (expired vs cancelled)
  instead of bool, if richer cancellation semantics are needed
- future: a deadline-based variant (asyncSleepUntil) could be added
  using EventLoop::runAt

---

## 10. Test Contracts
- await_ready returns false unconditionally
- asyncSleep resumes coroutine after specified duration on owner loop thread
- asyncSleep composes with co_await in a Task coroutine
- cancel during pending sleep resumes coroutine immediately (no handle leak)
- cancel of already-expired sleep is a safe no-op
- multiple sequential asyncSleep calls work correctly in one coroutine
- asyncSleep does not bypass EventLoop scheduling semantics

---

## 11. Review Checklist
- Is SleepAwaitable still a thin bridge on TimerQueue, not a scheduler?
- Is the coroutine handle resumed exactly once on all paths?
- Does resume happen on the owner loop thread?
- Can SleepAwaitable outlive the co_await expression? (it should not)
- Does cancel leave the system in a consistent state?
- Are TimerQueue ownership and lifecycle rules still unchanged?
