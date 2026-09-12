# System Intent: mini-trantor

## Intent

mini-trantor is a small, auditable C++23 Reactor TCP library. Its value is clear
thread ownership, resource lifetime, executable contracts and a coroutine bridge
that preserves EventLoop scheduling. The current scope authority is
[Reactor Scope Reset](reactor_scope_reset.intent.md), and the execution stages are
[the current roadmap](../../docs/roadmap.md), S0 through S3.

## Responsibilities

- Bind one EventLoop to one thread and drive I/O, timers and queued work there.
- Provide TCP accept/connect, connection buffers, callbacks and explicit teardown.
- Scale with EventLoopThread/ThreadPool without sharing unsynchronized reactor state.
- Keep TimerQueue and basic backpressure as TCP runtime support.
- Preserve a small coroutine bridge; fix cancellation/frame lifetime before expansion.
- Maintain optional TLS, existing DNS and a byte-only PacketFramer with explicit limits.

## Non-responsibilities

Game sessions, room/AOI state, business schedulers, account/security systems,
HTTP/WebSocket/RPC ecosystems, custom reliable UDP/PMTU/FEC and observability
platforms are outside this library. Historical implementations and intents are
archived; their prior existence does not authorize adding them back to core.
Linux is the primary verification platform; Windows select is a preview backend.

## Invariants

1. One loop has exactly one owner thread; mutable reactor state belongs there.
2. EventLoop owns Poller, TimerQueue and wakeup resources. Poller borrows Channel.
3. Channel does not own its fd or loop; registration and active-batch borrowing
   must end before destruction.
4. TcpConnection owns Socket, Channel and buffers, but borrows its EventLoop.
5. Server connection bookkeeping belongs to base loop; I/O and close events to ioLoop.
6. Cross-thread mutations return through runInLoop/queueInLoop; wakeup is a signal.
7. Coroutine handles and callback targets require lifetime protocols beyond shared_ptr.
8. Source, installed interfaces, tests and current documentation describe the same scope.

## Priorities and failure semantics

Lifecycle safety, thread affinity, API clarity and debuggability precede feature
breadth or optimization. Runtime I/O failures, program contract violations and
backend registration failures must remain distinguishable. Do not replace these
contracts with a generic error framework or silently swallow callback failures.

Known open risks are documented in the audit, particularly coroutine destruction,
DNS re-entry/loop lifetime, thread startup failure and TLS peer identity. None of
the current modules has a new Stable release claim based solely on passing tests.

## Ownership, threading and review

Follow [lifetime rules](lifetime_rules.intent.md), [threading model](threading_model.intent.md),
[ownership rules](../../rules/ownership_rules.md) and [thread affinity rules](../../rules/thread_affinity_rules.md).
Every core change answers owner thread, owner/releaser, re-entry, cross-thread
marshaling and the exact verifying test files. Lifecycle changes require diagrams.

## Contracts and extension points

Tests cover unit logic, public contracts, failure paths, lifecycle and cross-thread
behavior. Assertions must execute in Release. All retained tests run in applicable
configurations; optional TLS tests depend on TLS availability, not desired results.
Installed-package tests validate dependency selection and exclusion of retired APIs.
Applications extend through connection/message callbacks or framing, without making
core own business state. New backends and coroutine mechanisms require evidence
from real consumers and must pass existing reactor contracts.
