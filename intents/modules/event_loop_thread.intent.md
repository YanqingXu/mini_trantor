# Module Intent: EventLoopThread

## 1. Intent
EventLoopThread owns one background thread whose main job is to construct,
run, and stop exactly one EventLoop.

---

## 2. Responsibilities
- create one EventLoop inside the worker thread
- publish the created loop pointer once ready
- stop the loop and join the thread during explicit stop() or teardown

---

## 3. Non-Responsibilities
- does not schedule business tasks itself
- does not own connection objects directly
- does not implement thread-pool policy

---

## 4. Core Invariants
- one EventLoopThread produces at most one live EventLoop at a time
- returned EventLoop pointer is owned by the worker thread stack lifetime
- stop() and teardown wait for the worker thread to exit before returning

---

## 5. Threading Rules
- startLoop() is the publication point from creator thread to worker thread
- loop quit/join coordination must remain explicit
- stop() is the synchronization point after which the previously returned
  EventLoop pointer must be treated as expired

---

## 6. Failure Semantics
- thread startup must not expose a null loop after successful wait
- destruction should tolerate already-stopped loop/thread states
- explicit stop should tolerate already-stopped loop/thread states

---

## 7. Test Contracts
- startLoop returns a usable EventLoop pointer
- queued work executes on the worker loop thread
- destruction joins the thread cleanly after quit
- explicit stop drains accepted work, quits the loop, and joins the thread

## S1-03: Startup and stop contract

- Internal states: Idle -> Starting -> Running -> Exited -> Stopping -> Stopped;
  initialization failure reaches Failed after owner-thread cleanup. Failed and Stopped
  can start a fresh worker. Running start is idempotent.
- startLoop returns only after initialization. An init callback exception is rethrown
  on the starter after joining; init callback quit/stop is a startup failure, not a
  publication that can disappear before the starter wakes.
- After normal loop exit, the worker retains its stack EventLoop until stop, restart,
  or wrapper destruction releases it. Borrowed pointers remain alive until that
  boundary, but a closed LoopHandle rejects work: object lifetime is not dispatch life.
- External start/stop operations serialize; callers must synchronize use of a borrowed
  pointer against stop/restart. Init callbacks must return without waiting for an
  external control operation on the same wrapper.
- A worker may request its own stop (including in init); it cannot join itself.
  startLoop from init is rejected; from a running worker it returns the current loop.
  Destroying the wrapper on its managed thread is a fatal ownership violation.
- The state mutex protects pointer publication, quit/wakeup and unpublication before
  EventLoop destruction. The control mutex protects thread creation and join. User
  callbacks run under neither mutex. The worker never acquires the control mutex.
- Pool coordination can request all worker stops before joining any of them. No cached
  raw pointer is used to stop a worker whose stack might be destroyed.
- This contract handles initialization failures. Runtime callback exception policy is
  the separate S1 shutdown task; it is not silently converted into startup success.
- Extension: ThreadInitCallback runs on the newly constructed loop's owner thread.
  It must clean up any externally owned loop resources if initialization throws.
- Regression: `tests/contract/event_loop_thread/test_start_stop_failure.cpp` covers
  init-quit/throw/retry, self-stop, early exit, concurrent controls and pool rollback.

---

## 8. Review Checklist
- Is the loop publication race-free?
- Is shutdown explicit and join-safe?
- Does this still enforce one-loop-per-thread semantics?
