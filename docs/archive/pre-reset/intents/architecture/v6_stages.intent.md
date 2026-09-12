# Architecture Intent: v6 Stage Boundary

## 1. Intent

mini-trantor v6 extends the hardened v5 foundation into a more reusable
client-side and ecosystem-facing networking toolkit.

The purpose of v6 is not to bypass the Reactor model with higher-level magic.
The purpose is to make more real-world client workflows reusable while keeping:

- thread-affinity correctness
- explicit ownership
- EventLoop scheduling semantics
- contract-first evolution

---

## 2. v6-alpha — Client Ecosystem Foundation

### Goal
Client-side building blocks become reusable enough for higher-level services.

### In Scope
- HTTP client main path
- richer RPC client support such as connection pooling
- optional service discovery integration
- reuse of existing DNS / TLS / EventLoop / TcpClient foundations

### Required Guarantees
- new client abstractions do not invent a parallel scheduler
- retry / pool / discovery behavior remains explicit and testable
- ownership of pooled connections remains documented
- shutdown and reconnect semantics remain compatible with v5 contracts

### Exit Signals
- contract tests for HTTP client request/response lifecycle
- contract tests for RPC client pool ownership and teardown
- integration tests for real client round-trip paths

---

## 3. Out of Scope Before v6-alpha Closes

- QUIC / HTTP/3
- full service mesh integration
- transparent load balancing that hides thread ownership
- implicit global runtime

---

## 4. Dependency Analysis

```text
v6-alpha
  └── depends on v5-alpha for unified async semantics
  └── depends on v5-beta for stable shutdown behavior
  └── benefits from v5-gamma for dual-stack client usage
  └── benefits from v5-epsilon for cleaner protocol/transport boundaries
```

---

## 5. Review Questions

- Is this capability really a client-ecosystem addition, or should it first
  be solved at the v5 foundation level?
- Who owns pooled/discovered connections, and who releases them?
- Which retries are user-visible policy, and which are hidden implementation detail?
- Which exact contract and integration tests prove the new behavior?
