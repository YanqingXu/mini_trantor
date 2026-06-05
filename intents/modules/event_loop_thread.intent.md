# Module Intent: EventLoopThread

## 1. Intent
EventLoopThread owns one background thread whose main job is to construct,
run, and stop exactly one EventLoop.

---

## 2. Responsibilities
- create one EventLoop inside the worker thread
- publish the created loop pointer once ready
- stop the loop and join the thread during teardown

---

## 3. Non-Responsibilities
- does not schedule business tasks itself
- does not own connection objects directly
- does not implement thread-pool policy

---

## 4. Core Invariants
- one EventLoopThread produces at most one live EventLoop at a time
- returned EventLoop pointer is owned by the worker thread stack lifetime
- teardown waits for the worker thread to exit before destruction completes

---

## 5. Threading Rules
- startLoop() is the publication point from creator thread to worker thread
- loop quit/join coordination must remain explicit

---

## 6. Failure Semantics
- thread startup must not expose a null loop after successful wait
- destruction should tolerate already-stopped loop/thread states

---

## 7. Test Contracts
- startLoop returns a usable EventLoop pointer
- queued work executes on the worker loop thread
- destruction joins the thread cleanly after quit

---

## 8. Review Checklist
- Is the loop publication race-free?
- Is shutdown explicit and join-safe?
- Does this still enforce one-loop-per-thread semantics?
