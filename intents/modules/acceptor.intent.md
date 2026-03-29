# Module Intent: Acceptor

## 1. Intent
Acceptor owns the listening socket registration for one EventLoop.
It converts readable listenfd events into accepted connection fds and
hands them upward through a narrow callback boundary.

---

## 2. Responsibilities
- own listen socket wrapper
- own listen Channel registration
- accept as many ready connections as possible per readable event
- deliver accepted fds to upper layer on the loop thread

---

## 3. Non-Responsibilities
- does not create TcpConnection directly
- does not choose worker loop
- does not own TcpServer
- does not implement business protocol logic

---

## 4. Core Invariants
- Acceptor belongs to exactly one EventLoop
- listen Channel mutation happens only on owner loop thread
- accepted fds are either handed upward or closed explicitly
- destruction must not mutate Poller state from the wrong thread

---

## 5. Threading Rules
- listen() is owner-thread only
- handleRead() runs on owner loop thread
- destructor must respect owner-thread teardown discipline

---

## 6. Failure Semantics
- accept interruption and would-block are handled explicitly
- unexpected accept failure should not silently corrupt registration state
- teardown should not rely on accidental destructor side effects

---

## 7. Test Contracts
- listen() registers readable interest on owner loop
- accepted fd is forwarded through callback on loop thread
- no callback means accepted fd is closed explicitly
- destroy-before-listen path is safe

---

## 8. Review Checklist
- Is Acceptor still a thin listenfd adapter?
- Is listen Channel removed on the correct thread?
- Are unexpected accept errors explicit?
- Can accepted fds leak on callback absence or teardown?
