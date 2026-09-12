# Module Intent: PathMtuCache

## Intent

`transport::PathMtuCache` provides a small in-process cache for peer path MTU facts that can be
shared by multiple transport instances without sharing their EventLoop, socket, session, or flow
state.

It exists to let game-server KCP transports reuse confirmed datagram payload sizes and PMTU
blackhole cooldowns across transport restart/rebind cycles while preserving reactor ownership.

## Responsibilities

- Store peer-address keyed PMTU facts as value-semantic `PathMtuCacheEntry` values.
- Preserve the largest confirmed successful datagram payload size on success updates.
- Allow explicit failure updates to downgrade the cached safe datagram payload size.
- Preserve blackhole cooldown expiry and blackhole count hints.
- Provide internal synchronization so multiple owner loops may share one cache object.
- Provide a stable peer key helper matching `InetAddress::toIpPort()`.

## Non-Responsibilities

- No socket, Channel, EventLoop, session, KCP flow, or game object ownership.
- No packet parsing, ICMP authentication, or platform PMTU signal handling.
- No automatic global singleton or hidden ownership transfer.
- No disk persistence, cross-process replication, eviction policy, or background thread.
- No decision to send, fragment, retransmit, or probe; transports consume entries as advisory hints.

## Invariants

1. Entries contain only value data: confirmed datagram payload size, cooldown expiry, and blackhole count.
2. All mutations and reads of the internal map are protected by the cache mutex.
3. `recordSuccess()` never lowers an existing confirmed size.
4. `recordFailure()` may lower the confirmed safe size because it represents an explicit PMTU failure signal.
5. `clearCooldown()` must not erase the confirmed safe size.
6. The cache must not invoke callbacks while holding its mutex.
7. The cache must not call into `KcpTransport`, `UdpSocket`, or upper-layer game code.

## Collaboration

- `KcpTransport` may hold a `std::shared_ptr<PathMtuCache>` in `KcpTransportOptions`.
- `KcpTransport` still owns session flow state and applies cache entries only on its owner loop.
- UDP PMTU signal adapters and raw ICMP listeners produce value events; they do not write this cache directly.

## Threading Rules

- `PathMtuCache` can be called from any thread.
- Callers must not assume cache operations run on a particular EventLoop.
- A transport must still marshal its own flow-state changes to its owner loop before using cached values.

## Ownership Rules

- The application or transport factory owns the shared cache through `std::shared_ptr`.
- `KcpTransport` observes the shared cache through its options value; it does not clear or release global entries on `stop()`.
- Local transport caches remain owned by a single `KcpTransport` and may be cleared on stop.

## Failure Semantics

- Missing entries return `std::nullopt`.
- Zero-size success or failure updates are ignored.
- Cache entries are advisory; stale or conservative entries can at worst reduce probing eagerness or initial datagram size.
- Expired cooldown cleanup is driven by consumers such as `KcpTransport`, not by a background thread.

## Extension Points

- Future work may add bounded capacity, TTL pruning, path key customization, disk snapshotting, or distributed PMTU sharing.
- Such extensions must keep cache callbacks out of owner-loop hot paths and must not introduce hidden transport ownership.

## Test Contracts

- Unit: `tests/unit/transport/test_path_mtu_cache.cpp`
  - success keeps the largest confirmed size
  - explicit failure can downgrade cached safe size
  - cooldown can be cleared without erasing safe size
  - blackhole updates preserve the longest cooldown and largest confirmed size
  - concurrent shared access keeps one coherent peer entry
- KCP contract: `tests/contract/kcp/test_kcp_transport_stress_contract.cpp`
  - shared cache survives one `KcpTransport` stop/destruction and seeds confirmed size / cooldown into the replacement transport for the same peer path

## Review Checklist

- Does the change keep transport/session/socket ownership outside `PathMtuCache`?
- Does any shared-state access hold only the cache mutex and avoid callbacks?
- Does `KcpTransport` still apply cached facts on its owner loop?
- Is stop behavior clear: local cache cleared, shared cache retained?
- Which unit and KCP contract test verifies the behavior?
