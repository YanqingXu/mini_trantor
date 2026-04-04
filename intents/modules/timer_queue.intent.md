# Module Intent: TimerQueue

## 1. Intent
TimerQueue provides owner-loop timer scheduling for one EventLoop.
It translates timerfd readiness into ordered timer callback dispatch while preserving
reactor thread-affinity and lifecycle discipline.

---

## 2. Responsibilities
- own one timerfd and its Channel registration
- store one-shot and repeating timer metadata
- trigger ready timers on the owner EventLoop thread
- reschedule repeating timers explicitly
- support cross-thread add/cancel through EventLoop scheduling APIs

---

## 3. Non-Responsibilities
- does not invent a separate scheduler outside EventLoop
- does not own arbitrary user objects captured by timer callbacks
- does not provide business retry / timeout policy by itself
- does not bypass EventLoop quit and teardown semantics

---

## 4. Core Invariants
- one TimerQueue belongs to exactly one EventLoop
- timer container mutation happens only on the owner loop thread
- timerfd registration is removed before TimerQueue destruction completes
- ready timer callbacks run on the owner loop thread
- repeating timers are only reinserted if they were not canceled
- cancel must prevent future firing, even if requested cross-thread

---

## 5. Collaboration
- owned by EventLoop
- uses Channel for timerfd readability notifications
- relies on EventLoop runInLoop / queueInLoop for cross-thread add/cancel
- may later provide scheduling substrate for connection idle timeout or delayed shutdown features

---

## 6. Threading Rules
- addTimerInLoop / cancelInLoop are owner-thread-only operations
- public timer APIs on EventLoop may be called cross-thread, but must marshal into the owner loop
- timer callbacks execute in the owner loop thread only
- timerfd read/reset happens in the owner loop thread only

---

## 7. Ownership Rules
- EventLoop owns TimerQueue
- TimerQueue owns timerfd and its timer Channel
- TimerQueue owns timer metadata entries
- Timer callbacks borrow any captured objects; they must not imply hidden ownership transfer

---

## 8. Failure Semantics
- timerfd create/read/settime failures should fail explicitly rather than silently corrupt scheduling
- cancel of an already-fired or unknown timer is a safe no-op
- quit during timer callback processing must not abandon already-accepted owner-loop work
- TimerQueue destruction must tolerate an empty or partially canceled timer set

---

## 9. Public API Direction
Exposed through EventLoop:
- runAt(Timestamp, Functor)
- runAfter(Duration, Functor)
- runEvery(Duration, Functor)
- cancel(TimerId)

`TimerId` exists to make cancellation explicit and avoid exposing internal timer pointers.

---

## 10. Test Contracts
- one-shot timer fires on the owner loop thread
- repeating timer fires multiple times and can be canceled
- cross-thread runAfter marshals back and fires on the owner loop thread
- cancel before expiration prevents callback execution
- timer queue teardown removes backend registration safely

---

## 11. Review Checklist
- Is timer state still fully owner-loop-owned?
- Is timerfd registration removed before destruction?
- Can cancel race with callback execution without double-fire?
- Do timer callbacks still obey EventLoop thread-affinity rules?
- Does this design compose with future async timeout APIs without bypassing EventLoop?
