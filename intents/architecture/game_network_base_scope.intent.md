# Architecture Intent: Game Network Base Scope Boundary

## 1. Intent

The game-server direction must stay a network foundation, not grow silently into
a full commercial gateway, game framework, or custom production transport stack.

This intent freezes M1-M32 as a v1 game-network foundation preview and defines
the scope gate for future changes.

---

## 2. Foundation Scope

### 2.1 Reactor Core

These modules are always in core scope:

- EventLoop / Channel / Poller / EPollPoller
- TimerQueue where it preserves EventLoop ownership semantics
- TcpConnection / TcpServer / TcpClient / Acceptor / Connector
- Buffer / Socket / InetAddress / callback dispatch helpers
- EventLoopThread / EventLoopThreadPool
- coroutine bridge pieces that resume through EventLoop semantics

### 2.2 Transport Foundation

These modules are in scope when they remain small and ownership-neutral:

- TCP/TLS transport entry points
- UDP datagram input/output and read-budget fairness hooks
- transport endpoint/session abstractions
- packet framing and codec adapters
- basic metrics hooks and in-memory/text snapshot exporters

### 2.3 Game Network Foundation

These modules are in scope because they are networking-facing foundation pieces:

- PlayerSession and SessionManager lifecycle state
- GameServerPipeline admission, framing, and network-to-logic handoff
- LogicLoop and GameCommandQueue fixed-step bridge
- BroadcastRouter and BroadcastDispatcher
- basic gateway admission security skeleton
- basic game backpressure policy

They must not own business game state, account systems, room simulation, AOI
state, or deployment topology.

---

## 3. Experimental Transport Scope

KCP and PMTU/FEC-style work may live in the tree only as an explicit preview
layer. It must remain opt-in through runtime options, documented as preview, and
covered by tests labeled as experimental.

Experimental transport scope includes:

- KCP-style reliable UDP preview
- selective ACK, retry/RTO tuning, and dynamic MTU probe/backoff previews
- congestion-window preview
- redundant-copy preview
- XOR parity recovery preview
- platform PMTU signal adapter preview
- raw ICMP/ICMPv6 PMTU listener preview
- userspace raw ICMP PMTU signal authentication preview
- process-local shared PathMtuCache preview

These features must not be described as production-grade KCP, production
congestion control, production FEC, or a complete cross-platform PMTU system.

---

## 4. Out Of Core Scope

The following belong outside the core library unless a new intent explicitly
creates an adapter boundary:

- account/auth provider implementations
- ban, risk-control, device fingerprint, grey routing, and security audit systems
- AOI, space indexes, room/shard simulation, and game world state
- multiprocess or multinode gateway orchestration
- service discovery and deployment control planes
- Prometheus scrape servers, push gateways, alerting, dashboards, or storage
- disk-persistent or cross-process PMTU services
- cryptographic PMTU authentication and router trust models
- Reed-Solomon or multi-loss FEC codecs
- adaptive redundancy controllers
- production-grade congestion/window tuning

These may be examples, adapters, plugins, or downstream integrations, but they
must not become default responsibilities of reactor, UDP, KCP, session, or game
foundation modules.

---

## 5. Scope Gate

Every future core/game-network change must answer:

1. Is this required by most game-server networking foundations?
2. Does it preserve EventLoop ownership and explicit cross-thread marshaling?
3. Does it avoid owning business game state or deployment topology?
4. Can its public contract be expressed without turning the core into protocol
   research or an operations platform?
5. Should this be core, game foundation, experimental transport, adapter, or
   example code?
6. Which test label and file prove the boundary?

If the answer is unclear, default to adapter or experimental transport rather
than expanding core.

---

## 6. Test And Build Policy

- Experimental transport tests remain built by default so failures are visible.
- Experimental tests must carry a label such as `transport-experimental`,
  `kcp-preview`, or `pmtu-preview`.
- Optional build flags must not hide required foundation contract tests.
- A feature may be preview-quality only if the intent, docs, option names, and
  tests all say so consistently.

---

## 7. Review Checklist

- Does the change strengthen the network foundation rather than adding a game
  framework feature?
- Does the module still have one clear owner and release path?
- Does every cross-thread operation marshal through EventLoop or an explicit
  synchronized value object?
- Are preview features opt-in and labeled?
- Is productionization work kept out of core unless an adapter boundary exists?
