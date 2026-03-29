# Module Intent: TcpServer

## 1. Intent
TcpServer coordinates accept, worker-loop selection, and connection bookkeeping.
It is the lifecycle boundary between listening infrastructure and per-connection objects.

---

## 2. Responsibilities
- own Acceptor and EventLoopThreadPool collaboration
- create TcpConnection on chosen loop
- maintain connection map on base loop thread
- remove connections safely during close/shutdown

---

## 3. Non-Responsibilities
- does not perform per-connection I/O itself
- does not own worker EventLoop objects directly beyond thread-pool coordination
- does not process application protocol payloads

---

## 4. Core Invariants
- base loop owns connection map mutation
- close/remove path must not dereference a destroyed TcpServer
- connection creation and removal remain explicit and loop-safe
- shutdown should detach callbacks before asynchronous teardown continues

---

## 5. Threading Rules
- newConnection/removeConnectionInLoop run on base loop thread
- connectEstablished/connectDestroyed run on owning connection loop
- cross-loop handoff happens only through EventLoop scheduling APIs

---

## 6. Failure Semantics
- worker-loop teardown should not leave stale entries in the base-loop map
- shutdown should tolerate already-closing connections
- callback lifetime must remain safe during server destruction

---

## 7. Test Contracts
- new connections are assigned to a loop and registered there
- close callback removes the connection through the base loop
- destruction invalidates delayed removal callbacks safely

---

## 8. Review Checklist
- Does any callback still capture raw TcpServer lifetime unsafely?
- Is base-loop bookkeeping isolated from worker-loop teardown?
- Are shutdown and connection removal still explicit and predictable?
