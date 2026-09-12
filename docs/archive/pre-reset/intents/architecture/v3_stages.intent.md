# Architecture Intent: v3 Stage Boundary

## 1. Intent
mini-trantor v3 extends the stable v1 Reactor + v2 Client/Timer foundation with
production-relevant capabilities: structured concurrency, DNS resolution, and TLS.
The same staging discipline applies: freeze correctness boundaries in order,
with explicit exit signals before advancing.

v3 assumes v2-beta is closed and all v2 exit signals are met (28/28 tests passing).

---

## 2. v3-alpha — Structured Concurrency Primitives
### Goal
Composable awaitable combinators are stable.

### In Scope
- `mini::coroutine::WhenAll` — await multiple `Task<T>` concurrently, resume when all complete
- `mini::coroutine::WhenAny` — await multiple `Task<T>` concurrently, resume when first completes
- cancellation of remaining tasks in `WhenAny` after first completion
- composition with existing `Task<T>` and TcpConnection awaitables
- composition with `SleepAwaitable` (e.g. timeout pattern via `WhenAny`)

### Required Guarantees
- `WhenAll` / `WhenAny` do not bypass EventLoop scheduling semantics
- all sub-task resumes happen on the appropriate owner loop thread
- cancellation path does not leak coroutine handles or double-resume
- `WhenAny` cancellation of remaining tasks is safe even if they are mid-suspension
- result collection is type-safe (tuple for `WhenAll`, variant/index for `WhenAny`)
- pure coroutine-layer extension — no changes to Reactor core required

### Exit Signals
- unit tests for `WhenAll` with void and value-returning tasks
- unit tests for `WhenAny` with early completion and cancellation
- contract tests for resume-on-correct-loop-thread guarantee
- contract tests for exception propagation through combinators
- integration test for timeout pattern: `WhenAny(asyncReadSome(), asyncSleep(timeout))`

---

## 3. v3-beta — DNS Resolver
### Goal
Asynchronous domain name resolution integrated with EventLoop scheduling.

### In Scope
- `mini::net::DnsResolver` — async DNS resolution with EventLoop callback delivery
- integration with `Connector` / `TcpClient` for hostname-based connect
- caching strategy (optional, with explicit TTL policy)
- coroutine awaitable wrapper: `co_await resolver.asyncResolve("example.com")`

### Required Guarantees
- DNS resolution does not block EventLoop thread
- resolution result callback is delivered on the requesting EventLoop thread
- resolver lifecycle is tied to EventLoop or explicit owner
- thread pool or async I/O backend for actual resolution (not blocking the loop)
- failed resolution produces explicit error, not silent hang
- resolver does not introduce hidden shared mutable state across loops

### Exit Signals
- unit tests for resolver result parsing and cache logic
- contract tests for callback delivery on owner loop thread
- contract tests for resolution failure and timeout handling
- integration test for TcpClient connecting by hostname through resolver
- integration test for coroutine-based resolve + connect chain

---

## 4. v3-gamma — TLS/SSL Integration
### Goal
Secure transport layer on top of existing TCP connections.

### In Scope
- `mini::net::TlsContext` — SSL_CTX wrapper with certificate/key configuration
- `mini::net::TlsConnection` — TLS-aware connection adapter layered on TcpConnection
- TLS handshake integrated with EventLoop (non-blocking SSL_do_handshake)
- TLS read/write path through existing Buffer infrastructure
- TcpServer TLS mode (accept + handshake)
- TcpClient TLS mode (connect + handshake)
- coroutine awaitables for TLS connections (asyncReadSome / asyncWrite over TLS)
- SNI (Server Name Indication) support

### Required Guarantees
- TLS handshake does not block EventLoop thread
- TLS state machine is explicit (handshaking / established / shutting_down / closed)
- TLS operations respect owner-loop-thread discipline
- SSL_CTX is shared across connections but SSL objects are per-connection
- TLS shutdown path integrates with existing connection close semantics
- certificate verification errors produce explicit callbacks, not silent failures
- no OpenSSL global state leakage across EventLoop threads

### Exit Signals
- unit tests for TlsContext configuration (cert loading, protocol selection)
- contract tests for TLS handshake state machine on owner loop thread
- contract tests for TLS read/write through Buffer path
- contract tests for TLS shutdown and error handling
- integration test for TLS echo server + client round-trip
- integration test for coroutine TLS echo path
- integration test for certificate verification failure handling

---

## 5. Out of Scope Before v3-gamma Closes
- HTTP/HTTPS protocol stack
- WebSocket protocol
- QUIC / HTTP/3
- mTLS (mutual TLS) advanced policies
- DNS-over-HTTPS / DNS-over-TLS
- connection migration
- full structured concurrency cancellation tree (beyond WhenAll/WhenAny)

---

## 6. Dependency Analysis
```
v3-alpha (Structured Concurrency)
  └── no dependency on v3-beta or v3-gamma
      (pure coroutine layer, builds on existing Task<T>)

v3-beta (DNS Resolver)
  └── benefits from v3-alpha (WhenAny for resolve timeout pattern)
      but not strictly required

v3-gamma (TLS/SSL)
  └── benefits from v3-beta (hostname-based connect needs DNS)
  └── benefits from v3-alpha (WhenAny for handshake timeout)
  └── most invasive — staged last to minimize risk to stable core
```

---

## 7. Review Questions
- Which stage boundary does this change belong to?
- Does it strengthen the current stage, or leak work from a later stage?
- Which test layer proves the intended stage contract?
- Does this change introduce any dependency on v3 infrastructure
  that would break v2 guarantees if reverted?
- For TLS changes: does this modify TcpConnection internals,
  or layer cleanly on top?
