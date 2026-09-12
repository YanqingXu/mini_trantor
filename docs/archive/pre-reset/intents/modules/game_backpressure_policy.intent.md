# Module Intent: Game Backpressure Policy

## 1. Intent
Game backpressure policy defines how the game-server network path reacts when
input frames, logic commands, outbound connection buffers, or broadcast fanout
grow faster than the system can process them.

It sits above the per-connection TCP backpressure controller. The TCP controller
protects one socket's output buffer; game backpressure protects the whole
network-to-logic-to-broadcast path while preserving Reactor thread-affinity.

---

## 2. Responsibilities
- define admission limits for per-connection framed input backlog
- define admission limits for LogicLoop command backlog and oldest command lag
- define output limits for per-connection/default transport sends
- define broadcast burst limits for group/AOI/global fanout
- define priority-shedding semantics for soft overload zones
- define adaptive soft-threshold derivation when only a hard limit is named
- define overload actions: accept, defer, drop-low-priority, reject, or close
- surface every policy decision through metrics with enough context to debug
- keep all enforcement on the owner loop of the resource being protected

---

## 3. Non-Responsibilities
- does not replace `ConnectionBackpressureController`
- does not parse game business rules or decide game-specific message priority
- does not implement cross-process or cross-node traffic shaping
- does not hide overload by silently dropping authoritative state updates
- does not bypass `EventLoop::runInLoop` / `queueInLoop` semantics

---

## 4. Policy Layers

### 4.1 Input Framing Layer

Owner: connection owner I/O loop.

Protected resources:
- per-connection `GameServerPipeline::ConnectionState::input`
- frames decoded per input batch
- continuation count and queued continuation latency

Default desired actions:
- continue decoding up to the frame batch budget
- defer remaining buffered input through owner-loop continuation
- close malformed frames immediately
- close or reject connections whose buffered framed input exceeds a configured
  hard limit

### 4.2 Logic Admission Layer

Owner: `LogicLoop` owner loop and `GameCommandQueue`.

Protected resources:
- total logic command backlog
- per-session command backlog if session-level accounting exists
- oldest queued command lag
- commands drained per fixed-step tick

Default desired actions:
- accept commands while backlog and lag are below thresholds
- reject or drop explicitly low-priority commands when soft limits are crossed
- close or rate-limit abusive sessions when hard limits are crossed
- never let I/O loop execute game logic synchronously as a fallback

### 4.3 Output Send Layer

Owner: target connection or transport endpoint owner loop.

Protected resources:
- per-connection output buffer
- queued default logic outputs
- output queue-to-send latency

Default desired actions:
- use existing per-connection backpressure to pause reads on high-water
- treat messages without explicit priority metadata as normal priority
- drop explicitly low-priority outputs in a soft/adaptive overload zone before
  marshaling them to the target owner loop
- reject non-critical outputs when target endpoint is gone or already overloaded
- close only after an explicit hard policy threshold, not from incidental metric
  observation

### 4.4 Broadcast Fanout Layer

Owner: base loop for route selection; target I/O loops for batch send.

Protected resources:
- route target count
- per-loop broadcast batch size
- payload bytes per broadcast burst
- broadcast queue latency and fanout latency

Default desired actions:
- route only eligible sessions/groups/AOI
- split large fanout into loop-owned batches
- treat broadcast frames without priority flags as normal priority for backward
  compatibility
- reject low-priority broadcast bursts in the soft/adaptive zone before any
  per-loop dispatch is scheduled
- allow policy to reject or defer huge fanout bursts before scheduling thousands
  of sends
- keep AOI/spatial selection outside `TcpServer` so broadcast policy does not
  turn the server into a business object

---

## 5. Core Invariants
- policy state must belong to a clearly named owner loop
- enforcement must happen where the protected mutable state lives
- policy actions must be explicit and observable; silent drops are forbidden
- overload handling must converge on existing close/remove/session paths
- metrics callbacks observe decisions but do not make hidden state changes
- backpressure must not introduce a second scheduler beside EventLoop/LogicLoop
- packet flags with no priority bits must map to normal priority, not low
- priority shedding may only run where the protected backlog/fanout is measured

---

## 6. Threading Rules
- input backlog decisions run on the connection owner loop
- logic admission decisions run on the LogicLoop owner loop or are submitted to it
- output decisions run on the target endpoint owner loop
- broadcast routing decisions run on base loop before per-loop dispatch
- cross-thread policy updates must marshal to the owning loop before mutating
  thresholds or counters

---

## 7. Ownership Rules
- `GameServerPipeline` owns only per-connection pipeline state; it does not own
  `TcpServer`, `SessionManager`, `LogicLoop`, or `TransportManager`
- `LogicLoop` owns queue draining cadence and backlog accounting
- `TcpConnection` / transport endpoints own their output buffers
- `TcpServer` owns broadcast routing/dispatch infrastructure but not AOI business
  rules
- policy configuration should be value-type options passed before start, then
  applied through owner-loop safe reconfiguration APIs if runtime changes are
  later supported

---

## 8. Failure Semantics
- malformed input closes the offending connection through normal transport close
- input hard-limit overflow closes or rejects the offending connection/session
- logic soft-limit overflow may reject low-priority commands but must report the
  rejection to metrics
- logic hard-limit overflow may close or rate-limit a session through
  SessionManager, not by mutating PlayerSession directly from I/O code
- output soft/adaptive-limit overflow may drop only messages below the required
  priority and must report `DropLowPriority`
- output overload must not strand a connection in permanently paused-read state
- broadcast soft/adaptive-limit overflow may reject only messages below the
  required priority and must report `DropLowPriority`
- broadcast overload must fail before scheduling unbounded per-loop sends

---

## 9. Metrics Contract

Future metrics should distinguish:
- `InputDeferred`: input batch reached frame or byte budget and scheduled continuation
- `InputRejected`: input exceeded hard byte/frame/session policy
- `LogicAccepted`: command admitted to LogicLoop
- `LogicRejected`: command rejected by backlog/lag policy
- `OutputQueued`: output handed to target endpoint owner loop
- `OutputDropped`: output dropped/rejected with explicit reason
- `BroadcastAccepted`: fanout routed and dispatched
- `BroadcastDeferred`: burst intentionally split/deferred
- `BroadcastRejected`: burst rejected before scheduling sends

Metric samples should include at least:
- owner loop
- session token or transport session id when available
- message id / priority when available
- current backlog or fanout size
- soft/hard threshold that triggered the decision
- policy action and reason
- whether a soft/adaptive priority-shedding decision dropped the item

---

## 10. Test Contracts
- pipeline input over the hard buffered-byte limit closes or rejects the connection
- pipeline continuation still yields after frame budget without losing frames
- LogicLoop rejects commands when configured backlog or lag limits are exceeded
- rejected logic commands emit metrics and do not execute processor callbacks
- connection output high/low water still pauses and resumes reads on owner loop
- broadcast hard fanout limit rejects before per-loop sends are queued
- output soft-limit priority shedding drops low-priority messages while allowing
  higher-priority messages through the same path
- broadcast soft-limit priority shedding rejects low-priority fanout while
  allowing higher-priority fanout before dispatch
- packet priority flags preserve backward compatibility by treating zero flags
  as normal priority
- policy actions are deterministic under cross-thread submit/send/broadcast calls

---

## 11. Implementation Stages

### Stage A: Policy Options and Metrics Schema
- add value-type options for input, logic, output, and broadcast thresholds
- extend metrics with explicit policy decision events
- reject invalid threshold combinations in `validate()`

### Stage B: Input and Logic Admission
- enforce per-connection input byte hard limit in `GameServerPipeline`
- add LogicLoop queue capacity / oldest-lag admission checks
- return explicit submit result rather than only bool when more detail is needed

### Stage C: Output and Broadcast Enforcement
- connect default logic output path to endpoint overload decisions
- add broadcast fanout and payload-byte hard limits before dispatch scheduling
- preserve existing BroadcastRouter / BroadcastDispatcher ownership boundaries

### Stage D: Priority and Adaptive Soft Limits
- add value-type priority-shedding configuration under output and broadcast policy
- derive soft thresholds from hard thresholds when adaptive mode is enabled
- map game packet priority flags to metric priority without changing the default
  zero-flag behavior
- report low-priority soft-zone drops through explicit backpressure metrics

### Stage E: Stress Contracts
- add burst input, saturated logic queue, output-buffer, and large fanout tests
- add benchmark assertions against policy metrics

---

## 12. Review Checklist
- Does each threshold protect a named resource?
- Does each policy action happen on the resource owner loop?
- Are rejects/drops explicit in metrics and tests?
- Does zero priority metadata still mean normal priority for compatibility?
- Does soft/adaptive shedding drop only below-threshold priorities?
- Does any code execute business logic on an I/O loop as overload fallback?
- Does the policy compose with existing TcpConnection backpressure rather than
  duplicating it?
- Can a user reason from a metric event back to the policy decision that caused it?
