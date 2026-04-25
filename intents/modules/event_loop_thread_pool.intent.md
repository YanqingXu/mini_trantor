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

---

## 8. Review Checklist
- Is base-loop ownership still clear?
- Is loop selection deterministic?
- Does startup preserve one-loop-per-thread discipline?
