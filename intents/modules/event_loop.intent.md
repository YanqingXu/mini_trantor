# Module Intent: EventLoop

## 1. Intent
EventLoop is the core event-dispatch engine bound to one thread.
It waits for active I/O, dispatches Channel callbacks, executes queued tasks,
and serves as the central scheduling point for future timer and coroutine extensions.

EventLoop is the heart of reactor execution in mini-trantor.

---

## 2. Responsibilities
- own and drive Poller
- perform poll wait loop
- dispatch active Channel events
- execute pending functors submitted from same thread or other threads
- maintain thread-affinity discipline
- provide runInLoop / queueInLoop API
- support wakeup mechanism for cross-thread responsiveness
- provide safe extension point for timers and coroutine resume

---

## 3. Non-Responsibilities
- does not parse business protocols
- does not own all connection objects by itself
- does not define application-level routing logic
- does not replace a thread pool abstraction
- does not directly implement high-level coroutine task graph in v1

---

## 4. Core Invariants
- one EventLoop binds to exactly one thread
- loop() runs only on owner thread
- Poller is used only by owner thread
- pending functors execute on owner thread
- quit must not abandon already-queued pending functors
- wakeup is used to interrupt blocking poll when needed
- channel update/remove must respect EventLoop ownership
- Poller lifetime does not exceed EventLoop lifetime

---

## 5. Collaboration
- owns one Poller
- coordinates with Channel objects for I/O event dispatch
- later coordinates with TimerQueue
- later provides scheduling point for coroutine awaiter resume
- interacts with EventLoopThread / thread-pool wrappers in scaled mode

---

## 6. Main Execution Model
Default v1 loop direction:
1. wait for active I/O via Poller
2. dispatch active channels
3. execute pending functors
4. repeat until quit requested

If quit is observed, EventLoop may stop polling new iterations,
but it should still drain already-queued pending functors before leaving loop().

If this order changes, docs/tests/contracts must be updated.

---

## 7. Public API Direction
Typical API direction:
- loop()
- quit()
- runInLoop(Functor)
- queueInLoop(Functor)
- updateChannel(Channel*)
- removeChannel(Channel*)
- assertInLoopThread()
- isInLoopThread()
- wakeup()

Additional APIs can be added later for timers/coroutines.

---

## 8. Threading Rules
- EventLoop has owner thread identity
- same-thread runInLoop executes immediately
- cross-thread runInLoop behaves like queued scheduling
- queueInLoop always enqueues
- cross-thread enqueue must ensure wakeup when loop may be blocked
- pending functor queue flush occurs on owner thread only

---

## 9. Wakeup Intent
Wakeup exists to solve a specific problem:
the loop may be blocked in poll while another thread submits new work.

Wakeup should:
- be explicit
- be lightweight
- not become a hidden data channel
- not replace normal callback/business delivery semantics

---

## 10. Failure Semantics
EventLoop should explicitly handle:
- backend poll errors/interruption
- wakeup read/write issues
- invalid non-owner-thread mutation attempts
- callback exceptions if exception policy exists
- quit request while processing current iteration
- queued functors that were already accepted before loop exit

v1 should prefer predictable behavior over over-complicated generic error models.

---

## 11. Future Extension Points
- TimerQueue integration
- coroutine awaiter scheduling
- metrics/tracing hooks
- idle strategy tuning
- task priority experimentation

These extensions must preserve EventLoop as the single-thread scheduling core.

---

## 12. Test Contracts
- same-thread runInLoop executes immediately
- cross-thread queueInLoop executes on loop thread
- cross-thread queueInLoop wakes blocked poll
- quit causes safe loop exit
- pending functors preserve expected execution semantics
- quit still drains already-queued nested functors before loop exit
- update/remove channel routes through correct Poller interaction path

---

## 13. Review Checklist
- Does this change violate single-owner-thread discipline?
- Is wakeup still correct and necessary?
- Are pending functors executed in a predictable place?
- Does this complicate future timer/coroutine integration?
- Is destruction/shutdown path still safe?
