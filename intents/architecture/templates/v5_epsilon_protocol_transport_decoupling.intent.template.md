# Intent Template: v5-epsilon Protocol and Transport Decoupling

## 1. Intent

Describe which protocol layer currently depends too much on transport internals
and what narrower abstraction is being introduced.

---

## 2. In Scope

- protocol modules affected:
- transport abstractions introduced:
- lifecycle boundaries clarified:

---

## 3. Non-Responsibilities

- does not invent a second scheduler
- does not hide ownership or teardown paths
- does not change user-visible protocol behavior without contract updates

---

## 4. Core Invariants

- protocol code depends on narrower transport-facing surface
- teardown still converges through existing lifecycle paths
- TCP and TLS remain compatible transport implementations

---

## 5. Test Contracts

- unit:
- contract:
- integration:

Suggested files:
- `tests/unit/net/test_protocol_codec_adapter.cpp`
- `tests/contract/http/test_http_transport_contract.cpp`
- existing HTTP / WS / RPC contract and integration tests

---

## 6. Review Questions

- Is this a real decoupling, or just an extra wrapper around `TcpConnection`?
- Does the new abstraction have clear owner-thread semantics?
- Are protocol behaviors still covered by existing tests?
