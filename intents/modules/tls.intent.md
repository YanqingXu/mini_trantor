# Module Intent: TLS/SSL Integration

## 1. Intent
TLS integration adds a secure transport layer on top of existing TCP connections.
It wraps OpenSSL's `SSL_CTX` and `SSL` objects with reactor-friendly, non-blocking
handshake and I/O, preserving thread-affinity and lifecycle discipline.

TLS is an optional feature: connections without TLS continue to work unchanged.

---

## 2. Components

### TlsContext
- Wraps `SSL_CTX*` with RAII
- Factory methods for server (cert + key) and client contexts
- Configuration: CA path, verify mode, protocol version
- Shared across connections (SSL_CTX is reference-counted)
- Thread-safe for read-only access after construction

### TcpConnection TLS mode
- Optional `SSL*` per connection, enabled via `startTls()`
- TLS state machine: None → Handshaking → Established → ShuttingDown
- Non-blocking handshake integrated with Channel events
- SSL_read / SSL_write replace raw fd I/O when active
- SSL_shutdown integrates with existing close path
- Coroutine awaitables work transparently over TLS

---

## 3. Responsibilities
- provide RAII SSL_CTX management with safe configuration API
- perform non-blocking TLS handshake on owner EventLoop thread
- transparently route read/write through SSL when TLS is active
- integrate TLS shutdown with connection close semantics
- support SNI (Server Name Indication) for client connections
- preserve all existing non-TLS behavior when TLS is not enabled

---

## 4. Non-Responsibilities
- does not implement certificate revocation checking
- does not implement mTLS (mutual TLS) advanced policies
- does not implement session resumption / tickets in v3
- does not manage OpenSSL global state beyond one-time init
- does not replace or bypass EventLoop scheduling

---

## 5. Core Invariants
- TLS state machine transitions happen only on owner loop thread
- SSL object is per-connection, created from shared SSL_CTX
- handshake does not block EventLoop thread (non-blocking SSL_do_handshake)
- connectionCallback fires only after TLS handshake completes (when TLS is enabled)
- SSL_read/SSL_write handle WANT_READ/WANT_WRITE by adjusting Channel interest
- SSL_free is called before connection destruction completes
- no OpenSSL global state leakage across EventLoop threads

---

## 6. Threading Rules
- TlsContext construction and configuration happen on any thread before use
- TlsContext is read-only after construction; safe to share across threads
- startTls() must be called on the owner loop thread
- TLS handshake events are processed on the owner loop thread
- SSL_read / SSL_write happen on the owner loop thread only
- SSL_shutdown happens on the owner loop thread

---

## 7. Ownership Rules
- TlsContext is shared via shared_ptr (one per server/client, shared across connections)
- TcpConnection holds shared_ptr<TlsContext> to keep context alive
- TcpConnection owns its SSL* object (freed in destructor or connectDestroyed)
- SSL_CTX is owned by TlsContext; all SSL objects derived from it must not outlive it

---

## 8. Failure Semantics
- TlsContext creation failure (bad cert/key path) throws exception
- TLS handshake failure produces handleError → handleClose path
- SSL_read errors (non-WANT_READ/WANT_WRITE) trigger error → close path
- SSL_write errors trigger error → close path
- certificate verification failure is surfaced through OpenSSL's verify callback
- SSL_shutdown failure does not prevent connection cleanup

---

## 9. State Machine

```
                   startTls()
  kTlsNone ──────────────────── kTlsHandshaking
                                      │
                          SSL_do_handshake() == 1
                                      │
                                      ▼
                              kTlsEstablished
                                      │
                            shutdown / close
                                      │
                                      ▼
                              kTlsShuttingDown
                                      │
                                      ▼
                              connection closed
```

---

## 10. TcpServer Integration
- `TcpServer::enableSsl(shared_ptr<TlsContext>)` configures server-side TLS
- On new connection: TcpConnection::startTls(ctx, isServer=true) before connectEstablished()
- connectionCallback fires after TLS handshake succeeds

---

## 11. TcpClient Integration
- `TcpClient::enableSsl(shared_ptr<TlsContext>, hostname)` configures client-side TLS
- On new connection: TcpConnection::startTls(ctx, isServer=false, hostname) before connectEstablished()
- SNI hostname is set via SSL_set_tlsext_host_name
- connectionCallback fires after TLS handshake succeeds

---

## 12. Test Contracts
- TlsContext loads valid certificate and key successfully
- TlsContext rejects invalid certificate path
- TLS handshake completes on owner loop thread (server + client)
- TLS read/write deliver correct data through SSL layer
- TLS shutdown integrates with connection close path
- TLS handshake failure produces error → close callback
- certificate verification failure is detectable
- non-TLS connections are unaffected by TLS infrastructure
- coroutine awaitables work transparently over TLS connections

---

## 13. Review Checklist
- Is SSL object lifecycle tied to TcpConnection destruction?
- Does TLS handshake stay non-blocking?
- Are WANT_READ/WANT_WRITE handled correctly in all paths?
- Does connectionCallback fire only after handshake completes?
- Is the non-TLS path completely unchanged?
- Can SSL_CTX outlive all SSL objects derived from it?
- Is SNI set correctly for client connections?

---

## 14. Core Module Change Gate Answers
1. **Which loop/thread owns this module?** TlsContext is thread-agnostic after construction.
   SSL objects within TcpConnection are owned by the connection's EventLoop thread.
2. **Who owns it and who releases it?** TlsContext: shared_ptr ownership by TcpServer/TcpClient/TcpConnection.
   SSL: TcpConnection creates and frees it.
3. **Which callbacks may re-enter?** TLS handshake completion may trigger connectionCallback.
   SSL_read/SSL_write may require opposite event (WANT_WRITE during read).
4. **Which operations are allowed cross-thread?** TlsContext configuration before use.
   All SSL operations are owner-loop-thread only.
5. **Which test file verifies this change?** unit/tls/test_tls_context.cpp,
   contract/tls/test_tls_handshake.cpp, integration/tls/test_tls_echo.cpp
