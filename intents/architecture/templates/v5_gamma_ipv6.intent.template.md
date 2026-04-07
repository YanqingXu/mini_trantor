# Intent Template: v5-gamma IPv6 and Address Model Completion

## 1. Intent

Describe which address-model gap is being closed and why dual-stack support is
needed for the current public API.

---

## 2. In Scope

- address types supported:
- modules touched:
- new constructors/helpers:
- compatibility plan for existing IPv4 APIs:

---

## 3. Non-Responsibilities

- does not introduce cross-platform poller support
- does not change EventLoop scheduling semantics
- does not weaken existing IPv4 tests

---

## 4. Core Invariants

- family is explicit
- sockaddr storage remains valid
- string formatting is unambiguous
- accept/connect preserve family correctly

---

## 5. Test Contracts

- unit:
- contract:
- integration:

Suggested files:
- `tests/unit/net/test_inet_address_ipv6.cpp`
- `tests/contract/tcp_client/test_tcp_client_ipv6.cpp`
- `tests/integration/tcp_server/test_tcp_server_ipv6.cpp`

---

## 6. Review Questions

- Are IPv4 and IPv6 paths both explicit and understandable?
- Does any API accidentally assume `sockaddr_in` only?
- Are DNS results consumed without family loss?
