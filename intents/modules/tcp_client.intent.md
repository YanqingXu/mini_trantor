# Module Intent: TcpClient

## 1. Intent
TcpClient is the client-side counterpart to TcpServer.
It coordinates active connection establishment through Connector,
manages the resulting TcpConnection, and provides reconnect capability —
all while preserving the same owner-loop discipline as the server side.

---

## 2. Responsibilities
- own one Connector for active connection initiation
- own one TcpConnection (shared_ptr) after connect succeeds
- expose connect / disconnect APIs that respect owner-loop threading
- deliver connection / message / close / write-complete callbacks to the user
- support configurable retry-on-failure and reconnect-on-disconnect policies
- coordinate safe teardown of Connector and TcpConnection on destruction

---

## 3. Non-Responsibilities
- does not own EventLoop
- does not perform per-connection I/O directly (delegated to TcpConnection)
- does not implement application protocol logic
- does not manage multiple simultaneous connections (one client = one connection)
- does not implement DNS resolution

---

## 4. Core Invariants
- one TcpClient belongs to exactly one EventLoop
- connection state mutation happens on the owner loop thread only
- at most one active Connector attempt or one established TcpConnection
  exists at any time
- Connector Channel and TcpConnection Channel are removed before
  effective destruction
- reconnect policy is explicit and configurable, never silently hardcoded
- Hostname results retain resolver order and are tried sequentially. A failed
  candidate cannot monopolize the client through an implicit retry loop.
- Repeated connect while resolving, connecting or already connected is idempotent.
- A manual connect from a Disconnected notification is deferred until the old
  connection bookkeeping is removed; it works without automatic reconnect enabled.
- A new connect round has a generation; stop/disconnect/destruction invalidate
  delayed DNS completion and queued candidate advancement from earlier rounds.

---

## 5. Collaboration
- uses Connector to perform non-blocking connect and detect success/failure
- creates TcpConnection on the owner loop after Connector delivers a connected fd
- TcpConnection uses the same owner loop as TcpClient
- may use TimerQueue (through EventLoop) for reconnect backoff delays
- user interacts with TcpClient through callbacks set before connect()

---

## 6. Threading Rules
- connect() and disconnect() may be called cross-thread;
  they must marshal into the owner loop via runInLoop
- newConnection / removeConnection run on the owner loop thread
- user callbacks (connection / message / close) fire on the owner loop thread
- Connector state machine transitions happen on the owner loop thread only

---

## 7. Ownership Rules
- TcpClient owns the current Connector via shared_ptr; deferred owner-loop cleanup
  may retain an old attempt until its Channel has finished dispatching
- TcpClient holds TcpConnection via shared_ptr after connect succeeds
- TcpConnection close callback must prevent use-after-free of TcpClient
  (e.g. via weak capture or explicit guard, same discipline as TcpServer)
- destruction must run on the owner loop thread to safely clean up
  Channel registrations

---

## 8. Failure Semantics
- connect failure (refused, timeout, network unreachable) is reported through
  ConnectorEvent; no TcpConnection is fabricated for a failed candidate
- retry after failure uses the configured backoff strategy via EventLoop timer
- disconnect during an active connect attempt cancels Connector cleanly
- destruction during a pending connect or reconnect timer must not leak
  fds or leave stale Channel registrations
- repeated disconnect calls are idempotent
- Each hostname candidate uses one Connector with automatic retry disabled. On
  ConnectFailed/ConnectTimeout/SelfConnectDetected, advancement is always queued
  after old-attempt cleanup, guarded by both client generation and attempt identity.
- Exhausting candidates publishes Idle before the final failure hook. Explicit
  connect, including from that hook, starts a new resolution round; its generation
  rejects the old queued advance. Intermediate failure hooks retain the current
  round, so repeated connect remains idempotent. No infinite DNS retry is hidden.
- InetAddress clients honor ConnectorOptions.enableRetry for failed attempts.
  TcpClient::enableRetry controls reconnection after an established connection
  closes; it does not enable a hidden retry cycle after DNS candidate exhaustion.
- stop cancels pending establishment and future reconnection but leaves an
  established connection intact. disconnect additionally requests its shutdown.
- Connector event hooks may stop/disconnect, replace themselves or destroy the
  client; internal advancement is installed separately from the user hook slot.
- Connected/Disconnected lifecycle hooks fire once per connection transition;
  the bookkeeping close callback does not emit a second Disconnected hook.
- DNS OS work already running may finish after stop; its obsolete result is ignored.

---

## 9. Extension Points
- pluggable retry/backoff policy (e.g. exponential, fixed, no-retry)
- future coroutine-based connect awaitable
  (e.g. `co_await client.asyncConnect()`)
- future TLS handshake integration after TCP connect succeeds

---

## 10. Test Contracts
- connect to a listening server establishes TcpConnection on owner loop thread
- connect to a refused port reports failure through ConnectorEvent
- disconnect cancels pending connect and cleans up Channel registration
- enabled reconnect after an established connection closes starts a fresh round;
  failed single-address attempts use their configured backoff
- cross-thread connect marshals to owner loop before Connector starts
- cross-thread disconnect marshals to owner loop before teardown begins
- destruction during pending connect does not leak fd or crash
- destruction with an active TcpConnection cleans up safely
- controlled DNS candidates verify ordered fallback, finite exhaustion, repeated
  connect, stop/disconnect/destroy from failure hooks and obsolete DNS delivery
- connect inside the final failure hook starts a new round; the same call inside
  an intermediate failure hook keeps the existing candidate pass
- localhost integration must not require IPv4 to be the first resolver result

---

## 11. Review Checklist
- Is all mutable client state still owner-loop-owned?
- Can callbacks outlive TcpClient safely?
- Is Connector Channel removed before TcpClient destruction completes?
- Is TcpConnection Channel removed before TcpClient destruction completes?
- Is reconnect policy explicit and not buried in implementation details?
- Does cross-thread connect/disconnect use runInLoop consistently?
