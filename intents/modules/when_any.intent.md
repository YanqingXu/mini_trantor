# Module Intent: coroutine::WhenAny

## 1. Intent
WhenAny is a coroutine combinator that awaits multiple `Task<T>` sub-tasks
concurrently and resumes the caller as soon as the **first** sub-task completes.
It returns the index of the completed task along with its result, and
**cancels** remaining sub-tasks to prevent resource leakage.

WhenAny enables the timeout pattern:
`co_await whenAny(asyncReadSome(), asyncSleep(5s))` — whichever completes
first wins, and the other is cancelled.

WhenAny is a composition utility, not a scheduler.

---

## 2. Responsibilities
- accept a variadic pack of `Task<T>` sub-tasks (all must share the same
  return type `T`, or all be `Task<void>`)
- start all sub-tasks eagerly when the WhenAny awaitable is suspended on
- resume the parent coroutine when the first sub-task completes
- return the index of the winning sub-task and its result
- request cancellation of remaining sub-tasks after the first completes
- ensure every sub-task coroutine frame is destroyed exactly once,
  including cancelled ones

---

## 3. Non-Responsibilities
- does not own or reference an EventLoop
- does not schedule sub-task resumes across threads
- does not implement complex cancellation trees beyond simple stop-request
- does not guarantee that cancelled sub-tasks stop immediately
  (cancellation is cooperative)
- does not implement priority or weighted selection among sub-tasks

---

## 4. Core Invariants
- WhenAny is lazy: sub-tasks are not started until co_await
- the parent coroutine resumes exactly once, triggered by the first
  sub-task to complete
- only the first completing sub-task's result is delivered to the parent;
  results from later-completing sub-tasks are discarded
- remaining sub-tasks are notified of cancellation after the first completes
- every sub-task coroutine frame is destroyed exactly once on all paths
- WhenAny itself is a transient awaitable; it does not outlive the co_await
  expression
- the winning index is stable: it reflects the original positional index
  in the input parameter list

### Cancellation Semantics (Critical)
- cancellation is cooperative: WhenAny sets a cancellation flag, but sub-tasks
  must check it at their own suspension points
- for built-in awaitables (SleepAwaitable), cancellation calls the awaitable's
  `cancel()` method which safely resumes the sub-task coroutine
- for TcpConnection awaitables, cancellation triggers `forceClose()` on the
  connection, which causes the awaitable to resume with an error/empty result
- if a cancelled sub-task has already completed by the time cancellation is
  requested, the cancellation is a safe no-op
- double-resume prevention: only the first sub-task to complete triggers
  parent resume; subsequent completions (including from cancel-triggered
  resumes) are silently discarded

---

## 5. Collaboration
- consumes `Task<T>` objects by move
- composes inside any `Task<T>` coroutine via `co_await whenAny(t1, t2, ...)`
- interacts with SleepAwaitable's `cancel()` for timeout cancellation
- interacts with TcpConnection close path for connection-awaitable cancellation
- sub-tasks may be bound to different EventLoops

---

## 6. Threading Rules
- WhenAny itself has no thread affinity
- the parent coroutine resumes on the thread of the first completing sub-task
  (via symmetric transfer from that sub-task's FinalAwaiter)
- if the parent must resume on a specific EventLoop thread, the caller is
  responsible for ensuring this
- WhenAny's shared state (first-completion flag) must use atomic operations
  because sub-tasks may complete on different threads concurrently
- cancellation of remaining sub-tasks is dispatched from the winning
  sub-task's completion context; for cross-loop sub-tasks, the cancel
  operation must be safe to call cross-thread (SleepAwaitable::cancel
  and TcpConnection::forceClose both support this)

---

## 7. Ownership Rules
- WhenAny takes ownership of all input `Task<T>` objects via move
- shared state (first-completion flag + winner index + result + parent handle)
  is reference-counted or embedded in a shared control block
- cancelled sub-task wrapper coroutines must complete and be destroyed
  even after the parent has been resumed; their shared_ptr to the control
  block keeps it alive until all wrappers finish
- the parent coroutine handle is borrowed; it is resumed exactly once
  by the first completing wrapper

---

## 8. Failure Semantics
- if the first completing sub-task throws, the exception is propagated
  to the parent (remaining sub-tasks are still cancelled)
- if a cancelled sub-task throws during its cancellation/cleanup path,
  that exception is silently discarded (the parent has already been resumed)
- double-resume prevention is critical: the atomic first-completion flag
  ensures only one sub-task triggers parent resume
- if the parent coroutine is destroyed while sub-tasks are still running,
  the cancelled sub-tasks will complete but the parent handle will not be
  resumed; this is avoided by design (the parent should co_await WhenAny,
  which means it is suspended until WhenAny resumes it)

---

## 9. Extension Points
- future: cancellation token / stop_token integration for richer cancellation
- future: WhenAny for dynamic task vectors (`std::vector<Task<T>>`)
- future: WhenAny with heterogeneous return types via `std::variant`
- future: priority-based selection (prefer certain sub-tasks)

---

## 10. Test Contracts
- first completion wins: correct index and result returned
- remaining tasks cancelled: verify cancellation is requested
- all void tasks: WhenAny<void, void> returns winning index
- all value tasks: WhenAny<T, T> returns index + value
- single task: WhenAny with one task degenerates correctly
- exception from winner: exception propagated to parent
- exception from cancelled task: silently discarded
- timeout pattern: WhenAny(slowTask, asyncSleep(short)) returns sleep index
- double-resume prevention: concurrent completions do not crash
- move semantics: input tasks are consumed
- cancellation safety: no coroutine handle leak after cancel

---

## 11. Review Checklist
- Is WhenAny still a pure coroutine combinator with no EventLoop dependency?
- Is the first-completion flag correctly synchronized (atomic)?
- Is every sub-task coroutine frame destroyed exactly once?
- Is the parent coroutine resumed exactly once (no double-resume)?
- Is cancellation cooperative and safe for cross-thread sub-tasks?
- Are cancelled sub-tasks guaranteed to complete and clean up?
- Can WhenAny be misused to bypass EventLoop scheduling?
- Is the timeout pattern (WhenAny + SleepAwaitable) safe and correct?
