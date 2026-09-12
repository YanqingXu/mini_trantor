# Module Intent: WebSocket Support

## 1. Intent
The WebSocket layer provides RFC 6455 compliant WebSocket communication on top
of the existing HTTP/1.1 infrastructure. It leverages the HTTP upgrade handshake
mechanism and then switches to WebSocket frame-based communication.

Components:

- **WebSocketCodec**: frame encoder/decoder (RFC 6455 §5) — handles opcode, masking,
  payload length (7-bit/16-bit/64-bit), fragmentation is NOT supported in v1
- **WebSocketConnection**: per-connection state machine wrapping TcpConnection —
  manages upgraded connection lifecycle, provides send/onMessage API
- **WebSocketServer**: extends HttpServer to detect Upgrade requests, perform
  handshake, and hand off to WebSocket message callbacks

The WebSocket layer is a protocol adapter built on top of the HTTP layer —
it does not own or modify the Reactor core.

---

## 2. Responsibilities
- detect HTTP Upgrade: websocket requests
- compute and validate Sec-WebSocket-Accept handshake
- decode incoming WebSocket frames (text/binary/ping/pong/close)
- encode outgoing WebSocket frames (server-to-client, unmasked)
- unmask client-to-server frames (RFC 6455 §5.3)
- handle close handshake (opcode 0x8)
- handle ping/pong automatically (opcode 0x9 → 0xA response)
- deliver text/binary messages to user callbacks
- support per-connection upgraded state that persists across reads

---

## 3. Non-Responsibilities
- does not implement WebSocket extensions (permessage-deflate)
- does not implement fragmented messages (v1 requires FIN=1)
- does not implement WebSocket client (only server-side)
- does not implement subprotocol negotiation
- does not own EventLoop or TcpConnection lifecycle
- does not implement TLS-level wss:// (use TcpServer's enableSsl separately)

---

## 4. Core Invariants
- WebSocket state is per-connection, stored via TcpConnection::setContext
- a connection is either in HTTP mode or WebSocket mode, never both simultaneously
- server frames are never masked; client frames must be masked (reject unmasked)
- close frame triggers close handshake: send close frame back, then shutdown
- ping always gets an automatic pong response with same payload
- frame payload exceeding 64KB results in connection close (v1 safety limit)
- after upgrade, HTTP parsing is no longer invoked on that connection

---

## 5. Collaboration
- **WebSocketServer** wraps **HttpServer** or **TcpServer** with an upgrade hook
- on HTTP request with `Upgrade: websocket`, perform handshake and switch context
- after upgrade, the message callback delivers raw WebSocket frames
- user interacts via `WebSocketCallback` / `WebSocketMessageCallback`
- existing TcpServer threading model is preserved (one-loop-per-thread)

---

## 6. Threading Rules
- WebSocketCodec encode/decode are stateless utilities — no thread affinity
- WebSocketConnection state is per-connection, accessed only on owner loop thread
- WebSocket callbacks fire on the connection's owner loop thread
- WebSocketServer configuration must happen before start()

---

## 7. Ownership Rules
- WebSocketServer owns the underlying TcpServer
- WebSocketConnection state is stored in TcpConnection context (std::any)
- WebSocketCodec is a stateless utility namespace — no ownership
- user callbacks borrow the connection (shared_ptr) and message (string_view)

---

## 8. Failure Semantics
- invalid upgrade request (missing headers): respond 400, close
- invalid Sec-WebSocket-Key: respond 400, close
- unmasked client frame: close connection with 1002 (Protocol Error)
- oversized frame (> 64KB payload): close connection with 1009 (Too Large)
- unknown opcode: close connection with 1002 (Protocol Error)
- malformed frame header: close connection

---

## 9. Extension Points
- WebSocket client (future, using TcpClient)
- fragmented message reassembly (future)
- permessage-deflate extension (future)
- subprotocol negotiation (future)
- wss:// via TcpServer::enableSsl() (already supported at transport level)

---

## 10. Test Contracts
- frame encoding produces correct bytes for text/binary/ping/pong/close
- frame decoding parses 7-bit/16-bit/64-bit payload lengths correctly
- unmasking produces correct payload from masked frame
- HTTP upgrade request is detected and handshake response is correct
- Sec-WebSocket-Accept computation is RFC 6455 compliant
- after upgrade, text messages are delivered to WebSocket callback
- ping receives automatic pong with same payload
- close frame triggers close handshake and connection shutdown
- invalid upgrade request gets 400 response
- unmasked client frame causes protocol error close
- oversized frame causes close with 1009

---

## 11. Review Checklist
- Is WebSocket state ever accessed from a thread other than the connection owner?
- Can state leak between HTTP mode and WebSocket mode?
- Does the handshake compute Sec-WebSocket-Accept correctly (SHA-1 + Base64)?
- Are masked frames correctly unmasked using the 4-byte mask key?
- Does the server never mask outgoing frames?
- Are control frames (ping/pong/close) handled before delivering to user callback?
- Does close handshake follow RFC 6455 §7 (send close frame back)?
