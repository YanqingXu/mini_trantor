# Module Intent: Path MTU Signal Authentication Preview

## Intent

Path MTU signal authentication preview lets a reliability transport reject
raw ICMP Packet Too Big samples that do not quote one of its active sessions.

The preview protects KCP from obvious cross-session PMTU downgrade signals
without changing UDP ownership, EventLoop scheduling, or the existing explicit
operator/test `notifyPathMtuFailure(peer, ...)` path.

## Responsibilities

- Treat `udp::PathMtuFailure` as value-semantic evidence.
- Require raw ICMP PMTU samples to carry a bounded quoted UDP payload prefix
  when authentication is enabled.
- Decode only the minimal KCP frame prefix needed for magic, version, and
  session id matching.
- Apply accepted PMTU failures through the existing owner-loop KCP PMTU failure
  state machine.

## Non-Responsibilities

- No cryptographic authentication, HMAC, anti-replay token, or router trust model.
- No UDP-layer parsing of KCP sessions.
- No validation for manual trusted `notifyPathMtuFailure(peer, ...)` calls.
- No disk, distributed, BSD, or Windows PMTU source.

## Invariants

- UDP owns packet reception and preserves bounded quoted payload evidence only.
- KCP owns session-aware authentication and PMTU policy.
- Raw ICMP signals that fail authentication do not change flow state, path cache,
  cooldown, in-flight probes, or application delivery.
- Platform error queue and local `EMSGSIZE` samples are not rejected by this
  preview gate; they remain lower-level trusted/local signals.

## Threading Rules

- Authentication runs on the KCP owner EventLoop after public callbacks marshal
  through `KcpTransport::post()`.
- Peer-address to session-id matching happens under the KCP transport mutex.
- No callback is invoked from the authentication helper.

## Ownership Rules

- `udp::PathMtuFailure` owns its quoted prefix bytes by value.
- `KcpTransport` owns the option and helper logic.
- No helper stores references to UDP packet buffers, sessions, sockets, or loops.

## Failure Semantics

- Missing, too-short, invalid-magic, invalid-version, or wrong-session prefixes
  cause the raw ICMP signal to be ignored.
- Accepted signals reuse `applyPathMtuFailureLocked()` and therefore inherit the
  existing safe-size downgrade, probe cancel, cooldown, and path-cache behavior.

## Test Contracts

- `tests/unit/udp/test_icmp_path_mtu_listener.cpp` verifies raw ICMP parser
  source tagging and quoted UDP payload prefix preservation.
- `tests/contract/kcp/test_kcp_transport_stress_contract.cpp` verifies a wrong
  quoted KCP session is rejected and a matching quoted session is accepted.

## Review Checklist

- Does UDP remain free of KCP session parsing?
- Is quoted payload copying bounded?
- Does KCP authenticate only after peer/session lookup on the owner loop?
- Does an ignored signal leave MTU, probe, cache, and delivery state unchanged?
