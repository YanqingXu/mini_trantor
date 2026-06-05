# Module Intent: HTTP/1.1 Protocol Layer

## 1. Intent
The HTTP layer provides HTTP/1.1 request parsing and response generation on top
of the existing TcpServer infrastructure. It consists of four components:

- **HttpRequest**: value object holding parsed method, path, query, version, headers, body
- **HttpResponse**: builder for HTTP response status, headers, body
- **HttpContext**: per-connection incremental parser state machine that converts raw
  bytes from Buffer into HttpRequest objects
- **HttpServer**: thin wrapper around TcpServer that connects the parsing pipeline
  and exposes an `HttpCallback` interface for user request handling

The HTTP layer is a protocol adapter — it does not own or modify the Reactor core.

---

## 2. Responsibilities
- parse HTTP/1.1 request lines, headers, and bodies (Content-Length based)
- support GET, POST, PUT, DELETE, HEAD, OPTIONS, PATCH methods
- support keep-alive (Connection: keep-alive / close semantics)
- generate well-formed HTTP/1.1 responses with status code, headers, body
- provide per-connection context that survives across partial reads
- route completed requests to the user's HttpCallback
- handle malformed requests gracefully (400 Bad Request)
- close connection after response when Connection: close

---

## 3. Non-Responsibilities
- does not implement chunked transfer encoding (v1 uses Content-Length only)
- does not implement HTTP/2 or HTTP/3
- does not implement URL routing / middleware framework
- does not implement multipart form parsing
- does not implement WebSocket upgrade (deferred)
- does not implement request body streaming
- does not own EventLoop or TcpConnection lifecycle

---

## 4. Core Invariants
- HttpContext is per-connection; it is stored as connection context/state
- parsing is incremental: partial data accumulates until a complete request
- one HttpRequest is completed at a time; pipelining is sequential processing
- HttpResponse is generated entirely before sending
- HttpServer delegates all network I/O to the underlying TcpServer
- invalid request line or headers produce a 400 response and close the connection
- HttpContext is reset after each completed request-response cycle

---

## 5. Collaboration
- **HttpServer** wraps **TcpServer** and sets message/connection callbacks
- **HttpContext** is created per-connection and stored via TcpConnection::setContext
- on message arrival, HttpContext::parseRequest incrementally parses Buffer
- when a complete request is parsed, HttpServer invokes the user's HttpCallback
- the user fills an HttpResponse; HttpServer serializes and sends it
- HttpServer respects TcpServer's threading model (one-loop-per-thread)

---

## 6. Threading Rules
- HttpRequest, HttpResponse are value objects — no thread affinity
- HttpContext is per-connection, accessed only on the connection's owner loop thread
- HttpCallback fires on the connection's owner loop thread
- HttpServer configuration (setHttpCallback, setThreadNum) must happen before start()

---

## 7. Ownership Rules
- HttpServer owns TcpServer
- HttpContext is owned by the connection (via shared_ptr context storage)
- HttpRequest and HttpResponse are stack/value objects — no ownership complexity
- user's HttpCallback borrows HttpRequest (const ref) and HttpResponse (mutable ptr)

---

## 8. Failure Semantics
- malformed request line: respond 400, close connection
- header too large (> 8KB): respond 400, close connection  
- unrecognized HTTP method: respond 400, close connection
- Content-Length mismatch: respond 400, close connection
- exception in HttpCallback: not caught by HttpServer (caller responsibility)

---

## 9. Extension Points
- chunked transfer encoding (future)
- WebSocket upgrade detection and handoff (future)
- URL routing / middleware chain (future, built on top of HttpCallback)
- request body streaming for large uploads (future)
- HTTP client request builder (future, using TcpClient)

---

## 10. Test Contracts
- parse simple GET request returns correct method, path, version, headers
- parse POST request with body respects Content-Length
- parse request with query string extracts path and query correctly
- incremental parsing handles partial data across multiple Buffer reads
- malformed request line produces parse failure
- HttpResponse serializes status, headers, body correctly
- HttpServer delivers parsed request to HttpCallback on correct thread
- keep-alive connection processes multiple sequential requests
- Connection: close causes server to close after response
- 400 response sent for invalid requests

---

## 11. Review Checklist
- Is HttpContext ever accessed from a thread other than the connection owner?
- Can HttpContext state leak between requests (reset after each cycle)?
- Does the parser handle \r\n correctly for all header lines?
- Is Buffer data consumed correctly (retrieveUntil) during parsing?
- Are all HttpResponse fields serialized in correct HTTP format?
- Does HttpServer respect TcpServer's thread model?
