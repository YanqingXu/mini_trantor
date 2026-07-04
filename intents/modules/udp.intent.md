# Module Intent: UDP Transport

## 1. Intent
UDP transport provides datagram-oriented network input/output on one owner
EventLoop while preserving mini-trantor's Reactor scheduling discipline.

It is the lightweight unreliable transport base for game traffic and for
preview reliable transports such as KCP. It must not behave like a stream
connection, and it must not monopolize the owner loop under datagram bursts.

---

## 2. Responsibilities
- own one non-blocking UDP socket and its Channel registration
- receive datagrams on the owner EventLoop thread
- expose datagrams to UdpServer as packet callbacks
- send datagrams to explicit peer addresses
- let UdpServer maintain peer-address to TransportSessionId mapping
- limit the number of datagrams processed per read event with a read budget
- emit lightweight read-batch metrics for fairness and burst diagnostics
- optionally translate platform UDP PMTU events into path MTU failure callbacks
  without parsing higher-level transport protocols; Linux uses `MSG_ERRQUEUE`,
  while other platforms expose explicit adapter capability/query facts until
  their runtime PMTU sources are validated
- optionally run a Linux IPv4/IPv6 raw ICMP Packet Too Big listener and convert
  matching quoted UDP datagrams into the same value-semantic path MTU failure
  callback
- preserve the PMTU signal source and a bounded quoted UDP payload prefix in
  `PathMtuFailure` so higher-level transports can decide whether the signal is
  authentic for their protocol/session

---

## 3. Non-Responsibilities
- does not provide reliable delivery by itself
- does not parse game protocol frames
- does not own player/session business state
- does not implement global traffic fairness across multiple sockets
- does not implement cross-platform raw ICMP or KCP-specific PMTU policy
- does not decide userspace ICMP authentication; it only supplies the bounded
  quoted UDP payload evidence to the consumer
- does not bypass EventLoop scheduling for cross-thread send/stop requests

---

## 4. Core Invariants
- UdpSocket fd and Channel state belong to exactly one owner EventLoop
- Channel registration changes happen only on the owner loop
- read callbacks run on the owner loop and return after at most the configured
  datagram budget
- platform PMTU signal adapter drain runs from the owner-loop Channel error
  handler; synchronous send `EMSGSIZE` callbacks run in the caller context and
  are intended for owner-loop send paths
- raw ICMP listener Channel registration and read handling run only on the
  owner EventLoop; received ICMP/ICMPv6 packets are filtered by quoted UDP
  source port before emitting a path MTU failure sample
- `PathMtuFailure` is a value-semantic sample. Its source enum and quoted UDP
  payload prefix are advisory evidence, not ownership of packet storage or
  higher-level session state.
- stop disables read interest and removes the Channel before destruction
- UdpServer session maps are cleared on stop
- stop-after-send requests are dropped by the owner-loop started gate

---

## 5. Threading Rules
- recv and callback dispatch run on the owner EventLoop thread
- send/close/stop requested cross-thread must marshal to the owner loop through
  EventLoop APIs
- metrics callbacks run synchronously on the owner loop and must stay lightweight
- raw ICMP listener enable/disable is an owner-loop operation; callers must not
  use it as a cross-thread setter
- read budget configuration is intended to be set before start or from the owner
  loop

---

## 6. Ownership Rules
- UdpServer owns UdpSocket through unique_ptr
- UdpSocket owns Socket and Channel, but does not own EventLoop
- UdpSocket owns no platform PMTU adapter state; adapter functions are invoked
  synchronously from owner-loop paths and emit value-semantic samples
- UdpSocket owns the optional raw ICMP listener through unique_ptr; the listener
  owns its raw socket and Channel and is stopped before UdpSocket releases it
- UdpTransportEndpoint observes UdpServer lifetime through a token and does not
  extend server lifetime
- UdpServer owns peer/session maps; upper layers should treat session ids as
  handles, not ownership tokens

---

## 7. Failure Semantics
- EAGAIN/EWOULDBLOCK ends the current read batch normally
- EINTR retries without consuming read budget
- send errors that are not transient are reported through the error callback
- local `EMSGSIZE` and Linux error queue `EMSGSIZE` may emit a path MTU failure
  callback with peer address, failed UDP payload size, and any kernel-suggested
  safe UDP payload size; local/error-queue paths also preserve a bounded copy
  of the original UDP payload prefix when available
- unsupported builds keep async platform PMTU signals disabled through explicit
  adapter capability facts and return false when async configuration is requested
- raw ICMP listener setup is best-effort: Linux IPv4/IPv6 builds may fail
  without `CAP_NET_RAW`; unsupported or permission-denied setup returns false
  and leaves normal UDP/KCP probing behavior intact
- stop is idempotent and clears session state
- budget exhaustion returns control to EventLoop; level-triggered readiness will
  deliver remaining datagrams in later loop iterations

---

## 8. Extension Points
- configurable read budget per UdpSocket/UdpServer
- read-batch metrics for datagrams, bytes, duration, and budget exhaustion
- optional path MTU failure callback for reliability layers that own PMTU policy
- platform PMTU signal adapter for Linux error queue, explicit capability facts,
  and connected-socket MTU query hooks when the OS exposes them
- optional IPv4/IPv6 raw ICMP Packet Too Big parser/listener for PMTU signal preview
- KCP or other reliability layers may reuse UdpSocket without changing EventLoop
  ownership semantics

---

## 9. Test Contracts
- UdpSocket loopback send/receive works on the owner loop
- UdpSocket read handler stops at the configured datagram budget
- UdpServer forwards read-budget configuration to its socket
- read-batch metrics run on the owner loop and report budget exhaustion
- UdpSocket reports synchronous `EMSGSIZE` as a path MTU failure event
- PathMtuSignalAdapter preserves UDP payload-size conversion, Linux error queue
  configure/drain semantics, platform capability reporting, and unsupported
  platform disablement
- IPv4 raw ICMP and ICMPv6 Packet Too Big parsers extract peer address, failed
  UDP payload size, and suggested safe UDP payload size while rejecting wrong
  local ports; they also mark the signal as raw ICMP and preserve a bounded
  quoted UDP payload prefix for upper-layer authentication
- cross-thread send and stop still marshal safely and drop sends after stop

---

## 10. Review Checklist
- Does read handling return to EventLoop after the configured budget?
- Are Channel mutations still owner-loop only?
- Are metrics callbacks lightweight and owner-loop scoped?
- Is path MTU callback free of higher-level session ownership or protocol parsing?
- Is quoted UDP payload preservation bounded and left as evidence for consumers,
  rather than turning UDP into a KCP parser?
- Is platform PMTU signal handling isolated from UdpSocket ownership and thread state?
- Is raw ICMP/ICMPv6 setup best-effort, owner-loop scoped, and filtered before callback?
- Does stop still converge on remove-before-destroy?
- Does UdpServer avoid holding session-map locks while invoking user callbacks?
