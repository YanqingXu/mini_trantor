# Module Intent: EventLoopThreadPool

## 1. Intent
EventLoopThreadPool scales one-loop-per-thread execution by managing a set of
EventLoopThread workers and returning loops for connection assignment.

---

## 2. Responsibilities
- start configured worker EventLoopThread instances
- expose base loop when zero worker threads are configured
- hand out worker loops in a predictable round-robin manner
- stop all worker loops on explicit request (stop())

---

## 3. Non-Responsibilities
- does not own connection lifecycle directly
- does not share mutable connection state across loops
- does not replace EventLoop scheduling semantics

---

## 4. Core Invariants
- base loop remains the fallback loop when thread count is zero
- loop selection is deterministic and bounded by started workers
- thread-pool control remains on the base loop thread

---

## 5. Threading Rules
- start() and getNextLoop() are base-loop-thread operations
- worker loops are only used through EventLoop scheduling APIs after publication

---

## 6. Failure Semantics
- repeated selection must not step outside the worker loop array
- startup should remain explicit about zero-thread and multi-thread behavior

---

## 7. Test Contracts
- zero-thread start keeps work on base loop
- multi-thread start publishes the configured worker loops
- getNextLoop rotates through workers predictably
- stop() quits all worker loops and clears thread/loop containers

## S1-03: Atomic publication and owner-mediated stop

- States are Stopped, Starting, Running, Stopping. Publish the worker set only after every
  initialization succeeds; failures request stop on every partial worker, join all,
  return to Stopped and rethrow the original exception. Retry is allowed.
- start while Running is idempotent. Reentrant control or selection while Starting
  is rejected. This also applies to the zero-worker init callback on the base loop.
- start/stop/getNextLoop/getAllLoops require the base-loop thread. setThreadNum is
  stopped-only configuration under exclusive access, permitted before synchronized
  handoff to the base loop. It must not race control/selection. Null base loop and
  negative counts are rejected.
- Destruction may follow an externally synchronized ownership handoff; it uses worker
  owners directly and does not dereference the base loop. Never destroy from a worker.
- Stop requests go to all EventLoopThread owners before any join; their state mutex
  keeps quit/wakeup inside the EventLoop lifetime. Raw cached loop pointers are borrows.
- Independent worker quit does not destroy the cached object until pool cleanup, but
  selecting a worker already known to be quitting/exited is an error. Applications
  must coordinate producer shutdown; selection cannot promise future dispatch.
- Zero workers use the base loop; stopping the pool never quits that borrowed loop.
- ThreadInitCallback is the extension point; it runs once per worker, or once on the
  base loop for zero workers. Worker callbacks cannot synchronously control the pool.
- Regression: `tests/contract/event_loop_thread/test_start_stop_failure.cpp` and existing
  pool contract/stop tests cover rollback, reentry, early exit, repeat start/stop, restart.

---

## 8. Review Checklist
- Is base-loop ownership still clear?
- Is loop selection deterministic?
- Does startup preserve one-loop-per-thread discipline?
