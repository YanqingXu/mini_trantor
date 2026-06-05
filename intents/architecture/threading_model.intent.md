# Architecture Intent: Threading Model

## 1. Intent
The threading model defines how concurrency is structured in mini-trantor.
The goal is to reduce synchronization complexity by concentrating mutable reactor state
inside a single EventLoop thread, while still allowing safe cross-thread task submission.

---

## 2. Core Principle
One EventLoop owns one thread.
That thread is the only place where:
- Poller wait/dispatch occurs
- Channel registration changes occur
- pending functors are flushed
- already-queued pending functors are drained before loop exit
- lifecycle-sensitive state transitions occur

This is the primary concurrency discipline of the system.

---

## 3. Why This Model Exists
This model is chosen to:
- reduce lock complexity
- keep event ordering understandable
- keep callback execution context predictable
- simplify future coroutine resume semantics
- align with mature reactor framework design

---

## 4. Owner Thread Rule
Each EventLoop records its owner thread identity.
The following operations are owner-thread only:
- loop()
- update/remove channel registration
- processing active channels
- flushing pending functors
- loop-owned timer dispatch
- connection state mutation under that loop

Violations should be guarded, asserted, or explicitly rejected depending on severity.

---

## 5. Cross-Thread Interaction Model
Cross-thread callers must not directly mutate loop-owned state.

Approved paths:
- runInLoop(fn)
- queueInLoop(fn)
- wakeup()

### runInLoop(fn)
- executes immediately if caller is already in owner thread
- otherwise enqueues fn and ensures wakeup if necessary

### queueInLoop(fn)
- always enqueues
- loop thread executes queued functions later
- if fn was accepted before quit completes, it should still run on the owner loop thread

### wakeup()
- interrupts blocked poll wait
- causes loop to observe pending work sooner

---

## 6. Dispatch Ordering
Within one loop iteration, the system should have a clear order for:
- poll wait
- active channel dispatch
- queued functor flush
- timer-related work if present

Exact micro-order can evolve, but must remain documented and stable enough for reasoning.

v1 default direction:
1. poll for active events
2. dispatch active channels
3. execute pending functors
4. repeat

If quit is requested during an iteration, already-queued functors should still be drained
before the loop fully exits.

If this changes, related contracts/tests/docs must be updated.

---

## 7. Callback Thread Context
Unless explicitly documented otherwise:
- Channel callbacks run in EventLoop owner thread
- queued tasks run in EventLoop owner thread
- connection callbacks run in owning EventLoop thread
- write/read/error/close handling runs in owning EventLoop thread

The system should avoid ambiguous execution contexts.

---

## 8. Wakeup Design Intent
Wakeup is not business messaging.
Wakeup is a scheduling interrupt used to:
- break blocking poll
- accelerate queued task handling
- maintain responsiveness for cross-thread submissions

Wakeup must remain lightweight and explicit.

---

## 9. Scaling Model
The threading model supports:
- single-loop single-thread v1 deployment
- one-loop-per-thread scaling in thread pool model
- accept loop handing off connections to worker loops

The model does not assume shared mutable connection state across loops.

---

## 10. Coroutine Compatibility
Future coroutine integration must respect this model.
Await suspension/resume should not invent a new concurrency model.
Resume should return to the appropriate EventLoop thread unless explicitly designed otherwise.

---

## 11. Failure / Risk Areas
High-risk threading mistakes:
- direct Poller mutation from other thread
- callback execution on wrong thread
- forgetting wakeup after cross-thread queue
- hidden shared state between loops
- destruction path racing with queued callbacks

These must be explicitly guarded in implementation and tests.

---

## 12. Test Contracts
- same-thread runInLoop is immediate
- cross-thread runInLoop is marshaled correctly
- queueInLoop from another thread wakes loop
- queued work executes on loop thread
- channel registration change is loop-thread enforced

---

## 13. Review Questions
- Does this change create a non-owner-thread mutation path?
- Is callback execution context still predictable?
- Is wakeup still correctly used?
- Does this make future coroutine scheduling harder or easier?
