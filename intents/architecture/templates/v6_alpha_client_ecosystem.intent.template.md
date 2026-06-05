# Intent Template: v6-alpha Client Ecosystem Foundation

## 1. Intent

Describe which client-facing workflow is being elevated into a reusable library
capability and why it belongs at the v6 layer rather than earlier core stages.

---

## 2. In Scope

- client abstraction added:
- underlying modules reused:
- pooling / retry / discovery policy:
- ownership model:

---

## 3. Non-Responsibilities

- does not bypass Reactor scheduling semantics
- does not hide retry/discovery policy that users need to reason about
- does not introduce a hidden global client runtime

---

## 4. Core Invariants

- client lifecycle is explicit
- pooled/shared connections have clear ownership
- shutdown and reconnect behavior remain documented

---

## 5. Test Contracts

- unit:
- contract:
- integration:

Suggested files:
- `tests/contract/http/test_http_client.cpp`
- `tests/integration/http/test_http_client.cpp`
- `tests/contract/rpc/test_rpc_client_pool.cpp`

---

## 6. Review Questions

- Why is this a reusable client capability rather than app-specific glue?
- Who owns and releases pooled/discovered connections?
- Which failure and retry behaviors remain visible to users?
