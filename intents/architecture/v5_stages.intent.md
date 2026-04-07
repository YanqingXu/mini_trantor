# Architecture Intent: v5 Stage Boundary

## 1. Intent

mini-trantor v5 is the consolidation release line.
Its goal is not to add many new top-level protocols, but to harden the
library as a long-term networking foundation.

v5 focuses on:

1. unified async semantics
2. graceful lifecycle closure
3. address model completeness
4. observability and configuration
5. cleaner protocol/transport boundaries
6. engineering guardrails

v5 assumes v4 protocol-layer features are available, but treats them as
consumers of the core runtime rather than excuses to weaken its contracts.

---

## 2. v5-alpha — Unified Cancellation and Error Semantics

### Goal
Coroutine-facing async APIs expose a shared cancellation model and a more
consistent error surface.

### In Scope
- cancellation token / source primitives
- integration with `SleepAwaitable`
- integration with `TcpConnection` read/write/close awaitables
- integration with `WhenAny` loser cancellation
- integration with async DNS resolve paths
- clearer distinction between peer-close, timeout, active cancel, and I/O failure

### Required Guarantees
- cancellation does not bypass EventLoop scheduling semantics
- cancellation and close paths do not double-resume coroutine handles
- errors are explicit enough for upper layers to distinguish major failure classes
- cancellation ownership and propagation are documented

### Exit Signals
- unit tests for cancellation token state transitions
- contract tests for cancel-on-owner-loop semantics
- integration test for timeout race patterns

---

## 3. v5-beta — Graceful Shutdown and Signal Integration

### Goal
Process shutdown and server shutdown become explicit library contracts.

### In Scope
- signal integration for `SIGINT` / `SIGTERM`
- stop-accepting path for servers
- orderly connection drain / forced teardown policy
- worker loop exit sequencing
- shutdown-safe pending functor handling

### Required Guarantees
- shutdown does not mutate loop-owned state off-thread
- shutdown ordering is explicit and documented
- signal delivery converges through EventLoop-friendly paths
- close callbacks do not fire on already-destroyed upper layers during shutdown

### Exit Signals
- contract tests for signal wakeup and shutdown ordering
- integration tests for graceful server shutdown in threaded mode

---

## 4. v5-gamma — IPv6 and Address Model Completion

### Goal
Address abstractions support dual-stack usage without damaging current APIs.

### In Scope
- IPv6-aware `InetAddress`
- socket helpers that accept IPv4 / IPv6
- accept/connect paths that preserve address family correctly
- DNS result consumption for dual-stack targets

### Required Guarantees
- existing IPv4 behavior remains stable
- address stringification remains clear and unambiguous
- no hidden family mismatch in accept/connect flows

### Exit Signals
- unit tests for address construction and formatting
- contract tests for IPv6 client/server usage
- integration test for IPv6 echo path

---

## 5. v5-delta — Configuration and Observability

### Goal
Runtime behavior becomes easier to configure, inspect, and diagnose.

### In Scope
- explicit options for connector / resolver / server tuning
- logging and metrics hook points
- connection / retry / timeout / handshake observability
- backpressure-related observability

### Required Guarantees
- observability hooks do not violate owner-thread rules
- configuration boundaries stay narrow and understandable
- default behavior remains stable for existing users

### Exit Signals
- contract tests for option propagation
- tests for metrics/log hook invocation on correct thread

---

## 6. v5-epsilon — Protocol / Transport Decoupling

### Goal
Upper protocol layers depend on narrower transport-facing abstractions.

### In Scope
- codec or stream-facing abstraction cleanup
- reducing HTTP / WS / RPC dependence on `TcpConnection` internals
- preserving TCP and TLS as transport implementations beneath the same direction

### Required Guarantees
- protocol-layer behavior remains unchanged from user perspective
- transport teardown and callback ordering stay explicit
- decoupling does not create a hidden second scheduler or ownership model

### Exit Signals
- contract tests that verify protocol behavior through narrowed interfaces
- integration tests for unchanged HTTP / WS / RPC main paths

---

## 7. v5-zeta — Engineering Guardrails

### Goal
The project is continuously verifiable and safer to evolve.

### In Scope
- CI workflow
- sanitizer-enabled test jobs
- install-package verification
- parser fuzz entry points
- benchmark baseline for key paths

### Required Guarantees
- guardrails remain additive and do not change runtime contracts
- failures are visible early in the change pipeline

### Exit Signals
- CI covers build, test, and install
- sanitizer jobs run the lifecycle-sensitive test set
- at least one fuzz target exists for text/frame parsers

---

## 8. Dependency Analysis

```text
v5-alpha (Unified async semantics)
  └── should land before richer shutdown and client ecosystem work

v5-beta (Graceful shutdown)
  └── benefits from v5-alpha because close/cancel semantics become clearer

v5-gamma (IPv6)
  └── should land before major new client APIs to avoid freezing IPv4-only assumptions

v5-delta (Config + observability)
  └── can partially overlap with v5-gamma

v5-epsilon (Protocol/transport decoupling)
  └── safer after shutdown/error semantics are better defined

v5-zeta (Engineering guardrails)
  └── can progress alongside most other v5 stages
```

---

## 9. Review Questions

- Which v5 stage does this change strengthen?
- Does it reduce or increase lifecycle ambiguity?
- Does it preserve owner-thread discipline?
- Which exact test file proves the new contract?
- Which intent/docs/diagrams must be updated together?
