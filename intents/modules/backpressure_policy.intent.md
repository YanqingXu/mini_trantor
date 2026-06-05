# Module Intent: Backpressure Policy

## 1. Intent
Backpressure policy defines how one TcpConnection reacts when outbound bytes
accumulate faster than the peer can consume them.

The initial goal is not a global traffic-control framework.
It is a minimal per-connection policy that protects owner-loop memory growth
and event churn while preserving normal Reactor scheduling semantics.

---

## 2. Responsibilities
- define per-connection high-water and low-water thresholds
- pause read interest when outbound buffered bytes cross the high-water threshold
- resume read interest only after buffered bytes drain to the low-water threshold
- keep all policy transitions on the owning EventLoop thread
- compose with existing close/error/high-water-mark notification paths

---

## 3. Non-Responsibilities
- does not parse protocol-level flow-control frames
- does not provide global fairness across multiple connections
- does not replace application-specific overload policy
- does not bypass TcpConnection close/remove lifecycle

---

## 4. Core Invariants
- policy state belongs to exactly one TcpConnection owner loop
- read suspend/resume must happen through Channel registration changes on the owner loop
- policy uses hysteresis: pause at high-water, resume at low-water
- close/error/destruction clears any temporary read throttling state
- policy must not invent a separate scheduler or background worker

---

## 5. Collaboration
- TcpServer may install policy thresholds on newly created connections
- TcpConnection enforces the policy against its output buffer and read interest
- Channel carries the actual EPOLLIN enable/disable state change
- user callbacks still observe normal connection/message/close ordering

---

## 6. Threading Rules
- threshold evaluation runs on the owning connection loop
- cross-thread configuration must not directly mutate Channel registration
- any suspend/resume caused by the policy must converge on EventLoop scheduling APIs

---

## 7. Failure Semantics
- invalid threshold pairs should be rejected explicitly
- temporary backpressure must not strand a connection in permanently paused read state
- timeout/error/force-close paths should clear policy state by reusing normal teardown

---

## 8. Test Contracts
- crossing high-water pauses read processing for that connection
- draining to low-water resumes read processing on the owner loop
- policy does not bypass close/remove lifecycle
- TcpServer-installed policy affects newly accepted connections end-to-end

---

## 9. Review Checklist
- Is the policy per-connection rather than hidden global mutable state?
- Are Channel registration changes still owner-loop only?
- Can reads resume after drain without requiring ad-hoc external nudges?
- Does teardown still converge on the existing close/remove path?
