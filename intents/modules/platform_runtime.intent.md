# Module Intent: Platform Runtime Boundary

## Intent

Platform runtime boundary isolates operating-system differences required by the
reactor core while keeping `EventLoop`, `Channel`, `TcpConnection`, and protocol
layers backend-neutral.

Linux and Windows may use different socket handles, wakeup primitives, and
poller backends, but they must expose the same owner-loop scheduling semantics
to upper layers.

This module is not a scheduler.
This module is not a connection lifecycle owner.
This module is not business logic.

---

## Responsibilities

- Define platform socket handle aliases through `mini/net/platform/SocketTypes.h`.
- Implement `SocketsOps` behind the stable public `mini/net/SocketsOps.h` entry.
- Create, write, drain, and close EventLoop wakeup descriptors per platform.
- Host concrete poller backends under `mini/net/poller/`.
- Select the default poller backend at build time through `Poller::newDefaultPoller()`.
- Keep unsupported platform capabilities explicit.

---

## Non-Responsibilities

- Does not own `EventLoop`, `Channel`, `Socket`, or `TcpConnection`.
- Does not invoke user callbacks.
- Does not choose thread-pool scheduling or connection placement.
- Does not hide cross-thread operations from `EventLoop::queueInLoop()`.
- Does not make Linux-only PMTU/signalfd features available on Windows.

---

## Core Invariants

- Public reactor semantics stay identical across Linux and Windows.
- `EventLoop` owns wakeup descriptors and closes them after removing the wakeup Channel.
- `Poller` observes `Channel` and never owns it.
- Platform socket functions never transfer ownership implicitly.
- Platform-specific code must not leak backend event constants into user-facing APIs.
- Compatibility headers may forward old include paths, but implementation belongs in
  `platform/` or `poller/`.

---

## Collaboration

- `EventLoop` calls `platform::createWakeupFds()`, `writeWakeup()`, and `drainWakeup()`.
- `Socket`, `Acceptor`, `Connector`, `TcpConnection`, UDP, and DNS paths call
  `sockets::*` through the stable public header.
- `PollerFactory` chooses `SelectPoller` on Windows and `EPollPoller` on Linux.
- CMake selects exactly one socket implementation, one wakeup implementation, and
  one concrete poller backend for the target platform.

---

## Threading Rules

- Platform wakeup functions are part of the EventLoop cross-thread scheduling path.
- Descriptor creation and close are owned by the EventLoop owner thread lifecycle.
- Cross-thread wakeup writes are allowed only through `EventLoop::wakeup()`.
- Poller backend mutation remains owner-thread only.
- Synchronous capability queries may be thread-neutral only when they do not touch
  loop-owned descriptors.

---

## Ownership Rules

- `EventLoop` owns wakeup descriptors.
- `Socket` owns `SocketFd` unless `releaseFd()` explicitly transfers it.
- `Poller` owns only its backend kernel object, not `Channel`.
- `SocketsOps` functions observe descriptors passed by callers unless the function
  name explicitly creates or closes one.
- Windows WinSock process initialization is owned by the platform socket runtime.

---

## Failure Semantics

- Fatal socket or poller setup failures fail fast with platform error messages.
- Unsupported platform features must be reported as unsupported, not emulated
  silently.
- Would-block and interrupted errors must normalize to stable helper predicates.
- Wakeup drain failure is logged by `EventLoop` without changing callback ordering.

---

## Test Contracts

- `tests/contract/event_loop/test_event_loop.cpp` verifies wakeup and queued functor
  semantics.
- `tests/contract/poller/test_poller_contract.cpp` verifies backend-neutral poller
  registration behavior when enabled on the platform.
- TCP client/server/connection contract tests verify socket operation behavior
  through the public path.
- Windows VS2026 workflow verifies the Windows source selection and SelectPoller path.
- Linux CI/builds verify the Linux source selection and EPollPoller path.

---

## Review Checklist

- Does the change keep platform code under `mini/net/platform/` or `mini/net/poller/`?
- Does the public include path remain stable when practical?
- Which loop/thread owns the affected descriptors?
- Who owns and releases each descriptor/backend handle?
- Which callbacks may re-enter after platform events are translated?
- Which operations are allowed cross-thread, and are they still marshaled by EventLoop?
- Which test file verifies the behavior on each platform?
