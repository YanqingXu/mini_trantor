# Module Intent: DnsResolver

> S1-02a fixes cache-hit re-entry and cancellation registration publication.
> S1-02b replaces late raw-loop access with rejecting LoopHandle posts.
> See [the audit](../../docs/audit_2026-09-12.md). IPv4/IPv6 resolution is already
> implemented; resolver ecosystem work is frozen pending these contracts.
> S1-01d: ResolveAwaitable borrows a guarded Task resume handle. This prevents
> late frame resumption but does not close request cancellation or loop shutdown.

## S1-02a implementation contract
- Copy request input before publishing cancellation/completion callbacks.
- Cache lookup copies the result under cacheMutex; user callbacks run only after
  unlocking. Cache TTL changes use the same mutex as cache reads/writes.
- A request mutex serializes registration installation against terminal delivery.
  Registration reset and the user callback run outside that mutex.
- Cancellation callbacks capture weak operation state, avoiding registration cycles.
- A token cancelled before resolve begins delivers Cancelled even on a cache hit.
- ResolveAwaitable keeps a local resolver reference across publication, because
  synchronous completion may destroy the awaitable during resolve().
- test_dns_lifetime covers cache re-entry, pre-cancelled cache hits, concurrent
  registration/cancellation and destroyed Task frames. The loop must still outlive
  pending operations until the subsequent shutdown contract is implemented.

## S1-02b shutdown contract
- Null callbackLoop and empty callback arguments fail with invalid_argument before
  registering or queueing a request.
- resolve snapshots callbackLoop->handle() before any callback publication; the
  raw loop must be valid for that initial snapshot only.
- Worker and cancellation completions always use LoopHandle::queue, including
  cache hits. No resolve callback executes inline after this change.
- A closed target rejects late delivery. The request then unregisters cancellation
  and releases its callback without executing it on an arbitrary thread.
- Callback execution is owner-loop-only; callback capture destruction can occur
  on the thread abandoning the final request reference. Captured resources must
  have compatible ownership, or callers must drain requests before closing loops.
- Task/frame owners still perform owner-loop cleanup before destroying a loop.
  A posting handle does not own a loop, resume an abandoned detached task, or
  substitute for application-level cancellation and join.

## 1. Intent
DnsResolver provides asynchronous domain name resolution integrated with
EventLoop scheduling semantics. It runs blocking `getaddrinfo` calls on a
dedicated worker thread pool and delivers results back to the requesting
EventLoop thread via `LoopHandle::queue`. It supports optional caching with TTL.

DnsResolver is a standalone utility, not part of the Reactor core. It
bridges the gap between hostname strings and `InetAddress` values without
ever blocking an EventLoop thread.

---

## 2. Responsibilities
- accept a hostname + port + callback + target EventLoop
- perform blocking `getaddrinfo` on a worker thread (never on an EventLoop thread)
- deliver an explicit resolve result to the requesting EventLoop via `runInLoop`
- optionally cache resolved addresses with configurable TTL
- provide a global shared instance for convenience
- provide a coroutine awaitable wrapper (`ResolveAwaitable`) for coroutine use

---

## 3. Non-Responsibilities
- does not own any EventLoop (the callback loop is borrowed)
- does not implement DNS protocol directly (delegates to OS `getaddrinfo`)
- does not perform connection establishment (that is Connector's job)
- does not implement retry or fallback logic for resolution failures
- does not integrate DNSSEC or other advanced DNS features
- does not perform reverse DNS lookups

---

## 4. Core Invariants
- DNS resolution never blocks an EventLoop thread
- the resolve callback is queued on the requesting EventLoop thread, including
  cache hits; a target closed before delivery abandons the callback
- resolution failure is explicit, not encoded as an empty result vector
- cache entries expire after TTL; stale entries are never returned
- the worker thread pool is properly joined on DnsResolver destruction
- DnsResolver is thread-safe: `resolve()` may be called from any thread
- each resolve request reaches at most one callback completion; an abandoned
  closed target executes no callback
- cache is keyed by hostname only; port is applied at lookup time

---

## 5. Collaboration
- uses `LoopHandle::queue` to deliver results on the requesting thread
- produces `InetAddress` values consumable by `Connector` and `TcpClient`
- `TcpClient` uses DnsResolver for hostname-based connect
- `ResolveAwaitable` wraps the async resolve for coroutine composition
- composes with `Task<T>` via `co_await asyncResolve(...)`

---

## 6. Threading Rules
- `resolve()` is thread-safe: callable from any thread (including EventLoop threads)
- worker threads perform blocking `getaddrinfo`; they never touch EventLoop state
- result delivery happens exclusively on the target EventLoop thread via queued posting
- cache access is protected by a mutex (read/write from any thread)
- request queue is protected by a mutex + condition variable
- DnsResolver destruction joins all worker threads (must not be called
  from a worker thread)

---

## 7. Ownership Rules
- DnsResolver owns its worker thread pool
- DnsResolver owns its cache
- DnsResolver snapshots a LoopHandle from the live EventLoop at request entry;
  worker/cancellation state never retains a raw pointer to the EventLoop
- the global shared instance is reference-counted via `shared_ptr`
- `ResolveAwaitable` holds a `shared_ptr<DnsResolver>` to keep it alive
  during the resolve operation

---

## 8. Failure Semantics
- unresolvable hostname: callback receives explicit `ResolveFailed`, no crash or hang
- `getaddrinfo` error: logged to stderr, callback receives explicit `ResolveFailed`
- EventLoop closed before delivery: late posts reject and the request abandons
  its callback. Capture destruction may occur on the abandoning thread; callers
  must separately arrange owner-loop cleanup for network resources and frames.
- DnsResolver destroyed while requests are pending: workers drain the request
  queue and are joined; callbacks already queued on a loop remain pending there.
  Joining workers does not prove that those callbacks have executed.
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
- resolve invalid hostname returns explicit error
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
- Does callback-layer failure remain explicit rather than sentinel-based?
- Is the global shared instance properly initialized and torn down?
