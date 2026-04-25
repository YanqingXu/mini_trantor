# Module Intent: Graceful Shutdown and Signal Integration (v5-beta)

## 1. Intent

Formalize four shutdown paths as library-level contracts:
- **process shutdown**: SIGINT/SIGTERM triggers coordinated teardown
- **server shutdown**: TcpServer::stop() orchestrates accept stop → connection drain → loop exit
- **client shutdown**: TcpClient::disconnect()/stop() already exist; no structural change needed
- **worker loop shutdown**: EventLoopThreadPool::stop() quits and joins all worker loops

The goal is to make "how a service stops" an explicit, ordered, testable contract
rather than relying on destructor side effects or ad-hoc application code.

---

## 2. In Scope

- signals handled: SIGINT, SIGTERM; SIGPIPE globally ignored
- modules touched: Acceptor, EventLoopThreadPool, TcpServer, EventLoop (SignalWatcher)
- shutdown ordering defined:
  1. stop accepting new connections (Acceptor::stop)
  2. close or drain existing connections (TcpServer::stop)
  3. quit worker loops (EventLoopThreadPool::stop)
  4. quit base loop (EventLoop::quit)
- drain policy: TcpServer::stop() immediately force-closes all connections.
  A future drain-aware API (wait for in-flight requests) is deferred to v5-delta.

---

## 3. Non-Responsibilities

- does not create hidden global runtime
- does not mutate loop-owned state off-thread
- does not rely on destructor accidents for cleanup
- does not implement drain-with-timeout policy (deferred)
- does not handle SIGUSR1/SIGUSR2 or custom signal wiring
- does not change TcpClient shutdown (already sufficient)

---

## 4. Core Invariants

- accept stop precedes final loop teardown
- connection shutdown ordering is explicit (map mutation on base loop, destruction on IO loop)
- pending callbacks do not outlive safe owners (lifetimeToken guards)
- worker loop exit remains coordinated (quit → drain functors → join)
- SignalWatcher delivers signal events through normal Channel callback on owner loop
- SIGPIPE is ignored process-wide; write errors are detected via errno/SSL_error

---

## 5. Threading Rules

- which loop receives signal wakeup: the base loop (where SignalWatcher is installed)
- how shutdown requests cross threads:
  - TcpServer::stop() runs on base loop thread
  - EventLoopThreadPool::stop() calls quit() on each worker loop (cross-thread safe via atomic + wakeup)
  - TcpConnection::forceClose() marshaled to IO loop via runInLoop
- where final teardown executes: base loop thread, after loop() returns

---

## 6. Failure Semantics

- repeated shutdown request: idempotent — second stop() is a no-op
- signal during pending close: close callbacks complete; no double-resume
- forced close fallback: forceClose() always works regardless of write-buffer state
- shutdown on already-stopped server: safe no-op

---

## 7. Test Contracts

- unit:
  - Acceptor::stop() disables listening without destroying the Acceptor
  - EventLoopThreadPool::stop() quits all worker loops
- contract:
  - signal wakeup delivers callback on owner loop thread
  - TcpServer::stop() stops accepting and closes all connections
  - shutdown ordering: accept stop → connection close → worker loop exit → base loop exit
  - repeated stop() is idempotent
- integration:
  - SIGINT triggers graceful shutdown of a multi-threaded echo server
  - no pending functors access destroyed objects after shutdown

Suggested files:
- `tests/contract/signal/test_signal_handling.cpp`
- `tests/integration/tcp_server/test_graceful_shutdown.cpp`

---

## 8. Review Questions

- Is shutdown ordering documented and testable?
- Can callbacks still target destroyed upper-layer objects?
- Does threaded shutdown converge cleanly?
- Is SignalWatcher safe when the loop is not running?
- Does SIGPIPE ignoring interfere with existing error-detection paths?
