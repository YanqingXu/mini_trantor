# Module Intent: Channel

## 1. Intent
Channel is the binding object between:
- a file descriptor
- interested events
- returned active events
- callback handlers

It represents event semantics for one fd inside one EventLoop.

Channel is not the socket itself.
Channel is not the business connection object.
Channel is not the Poller.

---

## 2. Responsibilities
- store interested event mask
- receive returned active event mask
- dispatch corresponding callbacks
- coordinate update/remove through EventLoop
- provide tie-based callback safety support for upper-layer owner objects

---

## 3. Non-Responsibilities
- does not own fd by default
- does not own EventLoop
- does not perform poll wait
- does not define business protocol logic
- does not manage thread pool policy

---

## 4. Core Invariants
- a Channel belongs to exactly one EventLoop
- update/remove operations must follow loop-thread rules
- revents reflects active events returned by Poller/backend
- events reflects current interest mask requested by owner/module logic
- dangerous callback dispatch should be guarded if tie is enabled and owner expired

---

## 5. Collaboration
- EventLoop coordinates Channel update/remove
- Poller tracks registration state for Channel
- upper-layer owner (e.g. TcpConnection) may tie itself for callback safety
- Channel callbacks may invoke read/write/error/close handling paths

---

## 6. Event Semantics
Typical event types include:
- readable
- writable
- error
- close / hangup

Exact mapping depends on backend, but Channel should expose a stable semantic interface
rather than leaking raw backend complexity into upper layers.

---

## 7. Threading Rules
- Channel belongs to one EventLoop
- registration changes occur on owning EventLoop thread
- handleEvent is expected to run in EventLoop thread
- external callers must not mutate registration state from arbitrary threads

---

## 8. Lifetime Rules
- Channel does not own fd by default
- Channel may observe an upper-layer owner via weak tie
- if tied owner has expired, dangerous callback path should not proceed
- a Channel must not remain registered in Poller after effective teardown begins

---

## 9. Failure / Risk Areas
High-risk mistakes:
- confusing events vs revents
- dispatching callbacks after owner expiration
- removing/destroying channel without unregistering
- backend registration inconsistent with channel state
- invoking callbacks in wrong thread context

---

## 10. Public API Direction
Typical API direction:
- fd()
- events()
- set_revents(...)
- isNoneEvent()
- enableReading()
- disableReading()
- enableWriting()
- disableWriting()
- disableAll()
- handleEvent(...)
- tie(...)
- remove()
- update()

API naming can evolve, but semantics should remain stable.

---

## 11. Test Contracts
- event enable/disable updates interest mask correctly
- handleEvent dispatches correct callbacks
- tie blocks unsafe callback path when owner expired
- remove/update obey loop-thread discipline
- backend-facing state remains consistent after repeated enable/disable

---

## 12. Review Checklist
- Is Channel incorrectly owning fd?
- Are events and revents clearly separated?
- Is tie logic safe and understandable?
- Are update/remove paths respecting EventLoop ownership?
- Is callback dispatch order/documentation clear?