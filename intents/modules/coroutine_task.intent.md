# Module Intent: coroutine::Task

> S1 progress: sleep and TCP waiters implement owner-loop unregistration.
> Start/detach/adoption ownership edges are covered. Combinator parent destruction
> and DNS remain open;
> see [the execution record](../../docs/s1_coroutine_lifecycle.md). No claim is
> made that arbitrary destruction concurrent with coroutine execution is safe.

## 1. Intent
Task is the minimal composable coroutine result object for mini-trantor.
It provides start/detach/co_await semantics so that coroutine-based connection
handlers can be expressed naturally, while **never** replacing or bypassing
EventLoop scheduling semantics.

Task is a bridge, not a scheduler.

---

## 2. Responsibilities
- represent a lazy coroutine that suspends on initial_suspend
- provide `start()` to resume the coroutine synchronously from the caller
- provide `detach()` to transfer ownership into the coroutine frame itself,
  allowing fire-and-forget usage
- provide `operator co_await` for Task-to-Task composition
- propagate exceptions through the promise and rethrow on result access
- ensure coroutine frame is destroyed exactly once on all paths

---

## 3. Non-Responsibilities
- does not own or reference an EventLoop
- does not schedule coroutine resume across threads
- does not implement cancellation trees or timeout policies
- does not manage connection lifecycle or buffers
- does not replace callback-based Reactor event dispatch

---

## 4. Core Invariants
- S1-01c: start only leaves initial_suspend. Calling start again on a pending
  Task is a logic error; readiness/cancellation resumes belong to the awaitable.
- detach on an already-started Task transfers ownership without an extra resume;
  detach on a completed Task destroys its frame without resuming final_suspend.
- Awaiting an already-started Task attaches the continuation without restarting it.
  Empty-task await reports logic_error, rather than dereferencing a null handle.
- Task::Awaiter is move-only because it owns the transferred frame.
- a Task is lazy: the coroutine body does not begin until `start()`, `detach()`,
  or `co_await` is invoked
- ownership of the coroutine frame is unambiguous at all times:
  - before start/detach: Task object owns the frame
  - after detach: the frame owns itself (destroyed via FinalAwaiter)
  - after co_await transfer: the Awaiter owns the frame
- the coroutine frame is destroyed exactly once, regardless of path
- exception state is captured and rethrown on `result()` or `await_resume()`
- FinalAwaiter resumes the continuation if present, or self-destroys if detached,
  or suspends at final_suspend otherwise

---

## 5. Collaboration
- TcpConnection awaitables (ReadAwaitable, WriteAwaitable, CloseAwaitable) are
  `co_await`-ed inside Task coroutines; they interact with EventLoop, not Task
- TcpServer connection callback typically calls `echoSession(conn).detach()`
- Task itself has no direct dependency on any mini::net type

---

## 6. Threading Rules
- Destruction must not race coroutine execution. A suspended network Task is
  destroyed on its active awaitable's owner loop; off-thread callers request
  cancellation and arrange owner-loop cleanup instead of directly destroying it.
- A completion promise set inside a coroutine does not prove final suspension.
  Transfer back to another thread only after an owner-loop completion barrier.
- Task itself is thread-agnostic: it has no internal synchronization
- the caller of `start()` or `detach()` determines the initial execution thread
- when a Task is used with TcpConnection awaitables, the awaitable's
  `await_suspend` registers the coroutine handle with EventLoop, and resume
  happens on the owner loop thread — this is the awaitable's responsibility,
  not Task's
- **coroutine resume must never bypass EventLoop scheduling**: this is enforced
  by the awaitables (e.g. `queueResume`), not by Task itself

---

## 7. Ownership Rules
- Task<T> is move-only (non-copyable)
- Task destructor destroys the coroutine frame if still owned
- `detach()` transfers ownership: exchanges coroutine_ to null, sets detached
  flag, and resumes — after detach, Task object is empty
- `operator co_await` transfers ownership to Awaiter via std::exchange
- FinalAwaiter is responsible for frame destruction in the detached case

---

## 8. Failure Semantics
- unhandled exceptions are captured via `unhandled_exception()` into
  `exception_ptr`
- `result()` and `await_resume()` rethrow captured exceptions
- accessing `result()` before completion throws `logic_error`
- double-destroy is prevented by the move-only + exchange discipline

---

## 9. Extension Points
- Task<T> supports arbitrary return types via TaskPromise<T>::return_value
- Task<void> specialization uses return_void
- FinalAwaiter's symmetric transfer enables efficient Task-to-Task chaining
- cancellation token propagation already exists in the promise;
  frame unregistration and safe completion are the next required contracts

---

## 10. Test Contracts
- `tests/contract/coroutine/test_task_start_contract.cpp`: repeated start,
  detach before/after completion, adopting an already-started child and empty await.
- lazy start: coroutine body does not execute until start() is called
- detach: coroutine runs to completion and frame is destroyed automatically
- co_await composition: parent Task resumes after child Task completes
- exception propagation: thrown exception is rethrown on result() access
- void specialization: Task<void> start/detach/co_await work correctly
- move semantics: moved-from Task is empty and safe to destroy
- result before completion: throws logic_error

---

## 11. Review Checklist
- Is Task still a pure coroutine wrapper with no scheduling logic?
- Is coroutine frame ownership unambiguous on every path?
- Does FinalAwaiter correctly handle continuation / detach / normal suspend?
- Can Task be misused to bypass EventLoop resume discipline?
- Are all exception paths covered (unhandled_exception, result, await_resume)?
- Does detach safely transfer ownership without double-destroy risk?
