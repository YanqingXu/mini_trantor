# Intent Template: v5-alpha Unified Cancellation and Error Semantics

## 1. Intent

Describe which async APIs are being aligned and why shared cancellation/error
semantics are needed now.

---

## 2. In Scope

- awaitables affected:
- token/source primitives introduced:
- error/result carriers introduced or revised:
- modules touched:

---

## 3. Non-Responsibilities

- does not invent a new scheduler
- does not bypass EventLoop owner-thread discipline
- does not hide timeout/cancel/peer-close distinctions

---

## 4. Core Invariants

- resume occurs exactly once
- cancellation converges on owner loop semantics
- error categories are explicit enough for callers
- duplicate waiter and close paths remain guarded

---

## 5. Threading Rules

- who owns cancellation state:
- where await_suspend / await_resume may run:
- how cross-thread cancel is marshaled:

---

## 6. Ownership Rules

- who owns token/source:
- who only observes cancellation:
- how pending coroutine handles are protected:

---

## 7. Failure Semantics

- peer close:
- timeout:
- active cancel:
- transport error:

---

## 8. Test Contracts

- unit:
- contract:
- integration:

Suggested files:
- `tests/unit/coroutine/test_cancellation_token.cpp`
- `tests/contract/coroutine/test_cancellation_contract.cpp`
- `tests/integration/coroutine/test_timeout_race.cpp`

---

## 9. Review Questions

- Can any path double-resume a coroutine handle?
- Can callers distinguish timeout from peer close?
- Does cancellation still flow through EventLoop semantics?
