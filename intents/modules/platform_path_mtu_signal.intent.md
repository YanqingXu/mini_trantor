# Module Intent: Platform Path MTU Signal Source

## Intent

Platform path MTU signal source isolates operating-system PMTU facilities from
UDP socket ownership and KCP flow policy.

The module should make platform capability explicit. Linux has a runtime-tested
async source through `MSG_ERRQUEUE`; BSD-like and connected-socket MTU query
facilities are represented through guarded capability/query hooks so future
ports do not have to alter KCP semantics.

## Responsibilities

- Report platform PMTU source capabilities for IPv4 and IPv6 sockets.
- Configure and drain async PMTU signals when the current platform supports it.
- Keep Linux `IP_RECVERR` / `IPV6_RECVERR` + `MSG_ERRQUEUE` behind the adapter.
- Offer a connected-socket MTU query hook when `IP_MTU` / `IPV6_MTU` style
  socket options exist.
- Convert path MTU bytes to UDP payload bytes with IP/UDP overhead accounted.

## Non-Responsibilities

- No KCP session matching, authentication, or flow-state mutation.
- No raw ICMP socket ownership.
- No guarantee that BSD/Windows runtime behavior is validated on Linux CI.
- No disk or distributed PMTU cache.

## Invariants

- The adapter owns no fd, Channel, EventLoop, session, or game object.
- Adapter functions are synchronous and value-semantic.
- `UdpSocket` decides when to call configure/drain from its owner loop.
- `KcpTransport` consumes resulting `PathMtuFailure` samples only on its owner loop.

## Threading Rules

- Capability and payload-size conversion are thread-neutral pure queries.
- Configure/drain calls must be made by the socket owner path.
- Querying connected MTU is a socket operation and must be treated like other
  fd access by the caller.

## Ownership Rules

- `UdpSocket` owns the fd and callback wiring.
- `PathMtuSignalAdapter` observes the fd only for the duration of a synchronous
  function call.
- `PathMtuFailure` samples own copied data by value.

## Failure Semantics

- Unsupported platforms report disabled capability instead of pretending PMTU
  signals are available.
- Async configure returns false when the platform cannot enable that source.
- Connected MTU query returns `std::nullopt` when the socket is unsupported,
  unconnected, or the OS has no discovered path MTU.

## Test Contracts

- `tests/unit/udp/test_path_mtu_signal_adapter.cpp` verifies Linux capability
  facts, generic platform configure aliases, UDP payload-size conversion, and
  unconnected query failure.
- `tests/unit/udp/test_udp_socket.cpp` verifies the public socket API exposes
  generic platform PMTU signal toggles while preserving old error-queue aliases.

## Review Checklist

- Does the adapter avoid owning reactor or KCP state?
- Are unsupported platform capabilities explicit?
- Does Linux keep the existing real error-queue behavior?
- Are future BSD/Windows hooks guarded so Linux builds remain honest?
