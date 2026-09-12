# Intent Template: v5-beta Graceful Shutdown and Signal Integration

## 1. Intent

Describe which shutdown path is being formalized:
- process shutdown
- server shutdown
- client shutdown
- worker loop shutdown

---

## 2. In Scope

- signals handled:
- modules touched:
- shutdown ordering defined:
- drain policy:

---

## 3. Non-Responsibilities

- does not create hidden global runtime
- does not mutate loop-owned state off-thread
- does not rely on destructor accidents for cleanup

---

## 4. Core Invariants

- accept stop precedes final loop teardown
- connection shutdown ordering is explicit
- pending callbacks do not outlive safe owners
- worker loop exit remains coordinated

---

## 5. Threading Rules

- which loop receives signal wakeup:
- how shutdown requests cross threads:
- where final teardown executes:

---

## 6. Failure Semantics

- repeated shutdown request:
- signal during pending close:
- forced close fallback:

---

## 7. Test Contracts

- unit:
- contract:
- integration:

Suggested files:
- `tests/contract/signal/test_signal_handling.cpp`
- `tests/integration/tcp_server/test_graceful_shutdown.cpp`

---

## 8. Review Questions

- Is shutdown ordering documented and testable?
- Can callbacks still target destroyed upper-layer objects?
- Does threaded shutdown converge cleanly?
