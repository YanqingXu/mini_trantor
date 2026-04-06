# Module Intent: ConnectionBackpressureController

## 1. Intent
ConnectionBackpressureController enforces per-connection threshold-based read
throttling when outbound buffered bytes grow too large.

It is the concrete loop-owned carrier of the backpressure policy for one
TcpConnection. The policy itself remains simple hysteresis; this controller
holds the mutable enforcement state so TcpConnection does not need to embed
all of that logic inline.

---

## 2. Responsibilities
- hold the active high-water and low-water thresholds for one connection
- evaluate outbound buffered bytes on the owner loop thread
- pause Channel read interest when high-water is crossed
- resume Channel read interest only after draining to low-water
- track whether reading is currently paused by backpressure
- compose with normal connection close/error teardown

---

## 3. Non-Responsibilities
- does not own TcpConnection lifecycle
- does not implement global fairness or admission control
- does not parse protocol-level flow-control semantics
- does not replace high-water-mark notification callback behavior

---

## 4. Core Invariants
- one controller belongs to exactly one TcpConnection and one owner loop
- threshold evaluation and Channel interest changes happen on owner loop thread only
- hysteresis is preserved: pause at high-water, resume at low-water
- teardown clears transient paused-read state by reusing the connection's normal close path
- disabled policy (high-water == 0) leaves reads enabled unless the connection lifecycle says otherwise

---

## 5. Collaboration
- TcpConnection owns ConnectionBackpressureController
- TcpConnection notifies the controller after writes, drains, connection establishment,
  policy reconfiguration, and teardown
- Channel performs the actual enableReading/disableReading calls
- the higher-level backpressure policy intent remains defined by
  `intents/modules/backpressure_policy.intent.md`

---

## 6. Threading Rules
- policy configuration from other threads must marshal through TcpConnection onto the owner loop
- controller never mutates Channel registration off-thread
- controller does not introduce locks or shared mutable state across loops

---

## 7. Ownership Rules
- TcpConnection owns the controller
- controller observes TcpConnection output-buffer state and Channel read-interest state
- controller owns only its own threshold and paused-state bookkeeping

---

## 8. Failure Semantics
- invalid threshold pairs are rejected explicitly before controller state changes
- controller must not strand the connection in permanently paused-read state after drain
- close/error/force-close clear paused-read state by following the existing connection teardown

---

## 9. Extension Points
- future metrics hooks for pause/resume counts
- future policy variants selected by TcpConnection while reusing the same owner-loop discipline

---

## 10. Test Contracts
- crossing high-water pauses read processing for that connection
- draining to low-water resumes read processing on the owner loop
- disabling the policy restores normal read interest
- teardown does not leave the connection stuck in paused-read state
- cross-thread policy configuration still mutates behavior on the owner loop only

---

## 11. Review Checklist
- Is all backpressure state isolated from the public connection state machine?
- Do Channel interest changes still happen only on the owner loop?
- Can reads resume automatically after drain?
- Does teardown clear paused state without a side channel?
