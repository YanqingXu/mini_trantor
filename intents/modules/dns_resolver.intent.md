# Module Intent: DnsResolver

## 1. Intent
DnsResolver provides asynchronous domain name resolution integrated with
EventLoop scheduling semantics. It runs blocking `getaddrinfo` calls on a
dedicated worker thread pool and delivers results back to the requesting
EventLoop thread via `runInLoop`. It supports optional caching with TTL.

DnsResolver is a standalone utility, not part of the Reactor core. It
bridges the gap between hostname strings and `InetAddress` values without
ever blocking an EventLoop thread.

---

## 2. Responsibilities
- accept a hostname + port + callback + target EventLoop
- perform blocking `getaddrinfo` on a worker thread (never on an EventLoop thread)
- deliver resolved addresses (or empty on failure) to the requesting
  EventLoop via `runInLoop`
- optionally cache resolved addresses with configurable TTL
- provide a global shared instance for convenience
- provide a coroutine awaitable wrapper (`ResolveAwaitable`) for coroutine use

---

## 3. Non-Responsibilities
- does not own or reference any EventLoop
- does not implement DNS protocol directly (delegates to OS `getaddrinfo`)
- does not perform connection establishment (that is Connector's job)
- does not implement retry or fallback logic for resolution failures
- does not integrate DNSSEC or other advanced DNS features
- does not perform reverse DNS lookups

---

## 4. Core Invariants
- DNS resolution never blocks an EventLoop thread
- the resolve callback is delivered on the requesting EventLoop thread
  (guaranteed by `runInLoop`)
- empty result vector indicates resolution failure
- cache entries expire after TTL; stale entries are never returned
- the worker thread pool is properly joined on DnsResolver destruction
- DnsResolver is thread-safe: `resolve()` may be called from any thread
- each resolve request is processed exactly once
- cache is keyed by hostname only; port is applied at lookup time

---

## 5. Collaboration
- uses `EventLoop::runInLoop` to deliver results on the requesting thread
- produces `InetAddress` values consumable by `Connector` and `TcpClient`
- `TcpClient` uses DnsResolver for hostname-based connect
- `ResolveAwaitable` wraps the async resolve for coroutine composition
- composes with `Task<T>` via `co_await asyncResolve(...)`

---

## 6. Threading Rules
- `resolve()` is thread-safe: callable from any thread (including EventLoop threads)
- worker threads perform blocking `getaddrinfo`; they never touch EventLoop state
- result delivery happens exclusively on the target EventLoop thread via `runInLoop`
- cache access is protected by a mutex (read/write from any thread)
- request queue is protected by a mutex + condition variable
- DnsResolver destruction joins all worker threads (must not be called
  from a worker thread)

---

## 7. Ownership Rules
- DnsResolver owns its worker thread pool
- DnsResolver owns its cache
- DnsResolver borrows EventLoop pointers from callers (does not own them)
- the global shared instance is reference-counted via `shared_ptr`
- `ResolveAwaitable` holds a `shared_ptr<DnsResolver>` to keep it alive
  during the resolve operation

---

## 8. Failure Semantics
- unresolvable hostname: callback receives empty vector, no crash or hang
- `getaddrinfo` error: logged to stderr, callback receives empty vector
- EventLoop destroyed before callback delivery: same as any pending
  `runInLoop` — undefined if loop is gone; caller must ensure loop outlives
  pending resolve
- DnsResolver destroyed while requests are pending: worker threads finish
  current request, remaining queued requests are dropped
  (callbacks will not fire)
- cache miss does not block; request is queued for worker thread

---

## 9. Extension Points
- pluggable resolution backend (e.g., c-ares for true async DNS)
- DNS-over-HTTPS / DNS-over-TLS
- IPv6 support (AF_INET6)
- per-hostname TTL based on DNS record TTL
- negative caching (cache failures for a short period)

---

## 10. Test Contracts
- resolve "localhost" returns non-empty result with 127.0.0.1
- resolve invalid hostname returns empty vector
- callback is delivered on the specified EventLoop thread
- cache hit returns result without blocking on worker thread
- cache TTL expiry causes re-resolution
- clearCache removes all cached entries
- multiple concurrent resolutions complete correctly
- TcpClient with hostname connects to local server
- coroutine asyncResolve returns valid addresses

---

## 11. Review Checklist
- Does DNS resolution ever block an EventLoop thread? (must not)
- Is the callback always delivered on the target EventLoop thread?
- Is the cache properly synchronized?
- Can the worker thread pool leak threads on destruction?
- Are all `getaddrinfo` results properly freed with `freeaddrinfo`?
- Does empty result consistently indicate failure?
- Is the global shared instance properly initialized and torn down?
