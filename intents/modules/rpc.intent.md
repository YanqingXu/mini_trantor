# Module Intent: RPC (Remote Procedure Call) Layer

## 1. Intent
The RPC layer provides a minimal request-response protocol on top of the existing
TcpServer / TcpClient infrastructure. It consists of four components:

- **RpcCodec**: stateless codec that encodes/decodes length-prefixed RPC messages
  with `[totalLen | requestId | msgType | serviceMethod | payload]` framing
- **RpcChannel**: per-connection RPC state that maps in-flight requests to pending
  continuations (callback or coroutine), dispatches responses, and handles timeouts
- **RpcServer**: thin wrapper around TcpServer that registers service methods,
  decodes incoming requests, invokes handlers, and sends back responses
- **RpcClient**: thin wrapper around TcpClient that provides `call()` (callback)
  and coroutine `co_await asyncCall()` interfaces with per-request timeout

The RPC layer is a protocol adapter — it does not own or modify the Reactor core.

---

## 2. Responsibilities
- define a minimal binary framing protocol for request/response messages
- support request, response, and error message types
- correlate request ↔ response via unique request IDs
- support per-request timeout via TimerQueue integration
- support callback-based RPC calls (`call(method, payload, callback)`)
- support coroutine-based RPC calls (`co_await asyncCall(method, payload, timeout)`)
- dispatch incoming requests to registered method handlers on the server side
- handle malformed frames gracefully (close connection)
- resume coroutines / invoke callbacks on the connection's owner loop thread

---

## 3. Non-Responsibilities
- does not implement protobuf / flatbuffers serialization (payload is opaque bytes)
- does not implement service discovery or load balancing
- does not implement retry / circuit breaker / rate limiting
- does not implement streaming RPC or bidirectional streaming
- does not implement connection pooling (one RpcClient = one TcpClient)
- does not own EventLoop or TcpConnection lifecycle
- does not implement authentication or encryption (use TLS at transport level)

---

## 4. Core Invariants
- RpcCodec is stateless: encode/decode are pure utility functions
- RpcChannel is per-connection, accessed only on the connection's owner loop thread
- each request gets a monotonically increasing uint64 request ID per connection
- pending map (requestId → callback/continuation) is single-thread accessed only
- timeout cancels the pending entry and invokes callback with error (never leaks)
- response without matching pending request is silently discarded
- frame exceeding max size (default 64KB) causes connection close
- server handler receives (request payload, response callback) — must call response exactly once

---

## 5. Collaboration
- **RpcServer** wraps **TcpServer** and sets message/connection callbacks
- **RpcChannel** is created per-connection and stored via `TcpConnection::setContext`
- on message arrival, RpcCodec incrementally decodes frames from Buffer
- for request frames: RpcServer dispatches to registered handlers
- for response frames: RpcChannel matches requestId and invokes pending callback
- **RpcClient** wraps **TcpClient** and provides call/asyncCall API
- timeout is managed via `EventLoop::runAfter()` → TimerQueue

---

## 6. Threading Rules
- RpcCodec encode/decode are stateless utility functions — no thread affinity
- RpcChannel (pending map, nextRequestId) is per-connection, owner-loop-thread only
- RpcServer handler callbacks fire on the connection's owner loop thread
- RpcClient `call()` can be invoked from any thread (marshaled to loop thread)
- coroutine resumption always happens on the connection's owner loop thread
- RpcServer/RpcClient configuration (register methods, set timeout) before start()

---

## 7. Ownership Rules
- RpcServer owns TcpServer
- RpcClient owns TcpClient
- RpcChannel is owned by the connection (via `std::any` context storage)
- RpcCodec is a stateless utility namespace — no ownership
- pending callbacks are owned by RpcChannel; cleared on timeout or response
- user callback borrows request payload (string_view) and response sender (function)

---

## 8. Failure Semantics
- malformed frame (too short, invalid msgType): close connection
- frame exceeding max size (> 64KB): close connection
- request timeout: invoke callback with error status, remove from pending map
- connection closed while requests pending: invoke all pending callbacks with error
- server handler throws exception: not caught by RpcServer (caller responsibility)
- duplicate requestId in pending map: should not happen (monotonic counter)

---

## 9. Extension Points
- protobuf / flatbuffers payload serialization (future, orthogonal to framing)
- streaming RPC (future, new message types)
- service discovery integration (future, above RpcClient)
- connection pooling (future, wraps multiple RpcClients)
- bidirectional RPC (future, server can call client)
- compression (future, transparent payload transformation)

---

## 10. Test Contracts
- RpcCodec encodes and decodes request/response/error frames correctly
- RpcCodec handles partial frames (kIncomplete) across multiple buffer reads
- RpcCodec rejects frames exceeding max size
- RpcCodec rejects frames with invalid message type
- RpcServer dispatches request to correct registered handler
- RpcServer returns error response for unregistered method
- RpcClient callback receives response matching request
- RpcClient timeout invokes callback with error
- RpcClient pending callbacks invoked with error on connection close
- full RPC round-trip: client call → server handler → client callback
- coroutine round-trip: `co_await asyncCall()` returns response
- coroutine timeout: `co_await asyncCall()` throws on timeout

---

## 11. Review Checklist
- Is RpcChannel ever accessed from a thread other than the connection owner?
- Can pending map leak entries (timeout + response race)?
- Does requestId wrap safely (uint64 — effectively never)?
- Is response sent exactly once per request on the server side?
- Are all pending callbacks invoked on connection close (no leaked continuations)?
- Does the server handle back-to-back requests on the same connection?
- Is max frame size enforced before allocating payload memory?
