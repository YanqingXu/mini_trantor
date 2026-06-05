# Module Intent: Poller

## 1. Intent
Poller is the I/O multiplexing abstraction used by EventLoop.
Its role is to wait for I/O readiness from the backend and translate that
into a stable list of active Channel objects for EventLoop dispatch.

Poller is not business logic.
Poller is not callback dispatch logic.
Poller is not channel owner.

---

## 2. Responsibilities
- maintain backend registration for Channel objects
- wait for active I/O events
- fill active channel collection for EventLoop
- keep registration bookkeeping consistent
- hide backend-specific details behind stable interface

---

## 3. Non-Responsibilities
- does not own Channel
- does not own EventLoop
- does not invoke business callbacks directly
- does not define connection lifecycle
- does not decide thread-pool scheduling

---

## 4. Core Invariants
- one Poller belongs to one EventLoop
- Poller is only used by owner EventLoop thread
- backend registration state must be consistent with internal channel bookkeeping
- removed channels must not continue appearing as valid active channels
- backend errors must not silently corrupt Poller internal state

---

## 5. Collaboration
- EventLoop owns Poller and calls poll/update/remove
- Channel provides fd and event masks
- concrete backend implementation (e.g. epoll) performs actual OS interaction

---

## 6. Interface Direction
Typical interface direction:
- poll(timeoutMs, activeChannels)
- updateChannel(Channel*)
- removeChannel(Channel*)
- hasChannel(Channel*) optional in debug path

Concrete backend implementations may extend internal helpers but should preserve public semantic contract.

---

## 7. Threading Rules
- Poller methods are owner-thread only
- no direct cross-thread backend mutation
- backend wakeup integration is coordinated via EventLoop mechanisms

---

## 8. Backend Abstraction Intent
v1 backend target is epoll.
But Poller abstraction should preserve the possibility of:
- poll/select style backend for learning
- kqueue backend
- io_uring-related future experiment
without forcing upper layers to change semantic assumptions.

---

## 9. Failure Semantics
Poller should distinguish:
- backend wait interruption
- backend registration failure
- invalid removal/update path
- stale channel state bug

Handling can be assert/log/fail-fast depending on severity, but must remain explicit.

---

## 10. Risk Areas
High-risk mistakes:
- internal channel map diverges from backend registration
- remove path leaves stale backend state
- active event fill produces invalid Channel pointers
- backend event translation leaks too much backend-specific complexity upward
- non-owner-thread access slips in unnoticed

---

## 11. Test Contracts
- updateChannel registers or updates backend state correctly
- removeChannel unregisters correctly
- poll returns active channels accurately
- invalid path is guarded or diagnosed
- internal bookkeeping remains consistent after repeated updates

---

## 12. Review Checklist
- Is Poller accidentally owning Channel?
- Is backend state synchronized with bookkeeping?
- Are owner-thread assumptions enforced?
- Are error paths explicit enough?
- Is EventLoop-facing interface stable and backend-neutral enough?