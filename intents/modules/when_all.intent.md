# Module Intent: coroutine::WhenAll

## 1. Intent
WhenAll is a coroutine combinator that awaits multiple `Task<T>` sub-tasks
concurrently and resumes the caller when **all** sub-tasks have completed.
It collects results into a `std::tuple` and propagates the first captured
exception if any sub-task fails.

WhenAll is a composition utility, not a scheduler. It starts sub-tasks
eagerly upon `co_await` and relies on each sub-task's own awaitables
(TcpConnection, SleepAwaitable, etc.) for EventLoop integration.

---

## 2. Responsibilities
- accept a variadic pack of `Task<T>` objects
- start all sub-tasks eagerly when the WhenAll awaitable is suspended on
- track completion count; resume the parent coroutine when all complete
- collect results: `std::tuple<T0, T1, ...>` for value-returning tasks
- propagate exceptions: capture the first exception from any sub-task and
  rethrow it when the parent resumes
- ensure every sub-task coroutine frame is destroyed exactly once

---

## 3. Non-Responsibilities
- does not own or reference an EventLoop
- does not schedule sub-task resumes across threads (that is the
  responsibility of each sub-task's internal awaitables)
- does not implement cancellation of remaining sub-tasks on failure
  (v3-alpha: fail-collect-all semantics; cancellation is a future extension)
- does not implement timeout logic (use WhenAny + SleepAwaitable for that)
- does not limit concurrency or implement backpressure

---

## 4. Core Invariants
- WhenAll is lazy: sub-tasks are not started until the WhenAll awaitable
  is `co_await`-ed
- once started, all sub-tasks run to completion (no early cancellation)
- the parent coroutine resumes exactly once, after the last sub-task completes
- result tuple ordering matches the input task ordering (positional guarantee)
- if multiple sub-tasks throw, only the first exception (in completion order)
  is captured and rethrown; remaining exceptions are silently discarded
- every sub-task coroutine frame is destroyed, whether it succeeded or failed
- WhenAll itself is a transient awaitable; it does not outlive the co_await
  expression in the parent coroutine frame

---

## 5. Collaboration
- consumes `Task<T>` objects by move (takes ownership of coroutine handles)
- composes inside any `Task<T>` coroutine via `co_await whenAll(t1, t2, ...)`
- sub-tasks may internally use TcpConnection awaitables, SleepAwaitable,
  or other awaitable types — WhenAll is agnostic to these
- sub-tasks may be bound to different EventLoops; WhenAll does not enforce
  same-loop affinity

---

## 6. Threading Rules
- WhenAll itself has no thread affinity; it is a pure coroutine-layer construct
- the parent coroutine's resume thread is determined by whichever sub-task
  completes last — its FinalAwaiter will symmetric-transfer back to the
  WhenAll completion handler, which then resumes the parent
- if the parent must resume on a specific EventLoop thread, the caller is
  responsible for ensuring this (e.g. by wrapping the co_await in a
  loop-bound coroutine)
- sub-task resume threads are determined by their internal awaitables,
  not by WhenAll
- WhenAll's shared state (completion counter) must use atomic operations
  because sub-tasks may complete on different threads concurrently

---

## 7. Ownership Rules
- WhenAll takes ownership of all input `Task<T>` objects via move
- internally, WhenAll creates wrapper coroutines that own the sub-task
  handles and store results into shared state
- shared state (counter + result storage + exception) is reference-counted
  or embedded in a shared control block
- the parent coroutine handle is borrowed (not owned) by the shared state;
  it is resumed exactly once when the counter reaches zero

---

## 8. Failure Semantics
- if a sub-task throws, the exception is captured in the shared state
- remaining sub-tasks continue to run (no early abort in v3-alpha)
- when all sub-tasks complete, the captured exception is rethrown to the parent
- if the parent coroutine is destroyed before sub-tasks complete
  (e.g. due to Task destruction), sub-task wrapper coroutines will complete
  but the parent handle will not be resumed (it is already invalid);
  this scenario should be avoided by design
- double-resume of the parent is prevented by atomic counter: only the
  thread that decrements to zero performs the resume

---

## 9. Extension Points
- future: cancellation token propagation — if one sub-task fails, cancel
  the rest (structured concurrency with fail-fast semantics)
- future: WhenAll for dynamic task vectors (`std::vector<Task<T>>`)
- future: result accessor that preserves per-task exception information
  instead of only the first

---

## 10. Test Contracts
- all void tasks: WhenAll with multiple Task<void> completes successfully
- all value tasks: WhenAll returns std::tuple with correct values in order
- mixed types: WhenAll<int, std::string, void> returns correctly typed tuple
- single task: WhenAll with one task degenerates correctly
- exception propagation: if one sub-task throws, parent receives the exception
- multiple exceptions: first exception (in completion order) is propagated
- empty case: WhenAll with zero tasks completes immediately
- composition: WhenAll inside another WhenAll works correctly
- move semantics: input tasks are consumed; originals are empty after call

---

## 11. Review Checklist
- Is WhenAll still a pure coroutine combinator with no EventLoop dependency?
- Is the completion counter correctly synchronized for cross-thread sub-tasks?
- Is every sub-task coroutine frame destroyed exactly once on all paths?
- Is the parent coroutine resumed exactly once (no double-resume)?
- Does result ordering match input ordering?
- Can WhenAll be misused to bypass EventLoop scheduling? (it should not)
- Is exception capture safe when multiple sub-tasks fail concurrently?
