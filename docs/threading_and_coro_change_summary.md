# Threading And Coroutine Change Summary

## Intent Reference

- `intents/modules/event_loop.intent.md`
- `intents/modules/event_loop_thread.intent.md`
- `intents/modules/event_loop_thread_pool.intent.md`
- `intents/modules/tcp_server.intent.md`
- `intents/modules/tcp_connection.intent.md`
- `intents/architecture/threading_model.intent.md`
- `intents/architecture/v1_stages.intent.md`
- stage: `v1-beta` + `v1-coro-preview`

## Scope

This change set tightens thread-affinity and shutdown semantics around:

- `EventLoop` scheduling and quit behavior
- `EventLoopThread` lifecycle publication and teardown
- `EventLoopThreadPool` base-loop ownership rules
- `TcpServer` base-loop connection bookkeeping
- coroutine resume behavior on `TcpConnection`

The main outcome is that several previously implicit guarantees are now covered by direct contract or integration tests.

## Core Module Change Gate

### EventLoop

1. Loop / Thread:
   `EventLoop` belongs to exactly one owner thread. Poll wait, active channel dispatch, pending functor flush, and loop-exit draining all run on that owner thread.
2. Ownership / Release:
   `EventLoop` owns `Poller` and wakeup resources. It must be destroyed on the owner thread after `loop()` has stopped.
3. Re-entrant Callbacks:
   Channel callbacks and pending functors may enqueue more pending functors. Quit may be requested while processing the current iteration.
4. Cross-thread Operations:
   Allowed cross-thread operations remain `runInLoop`, `queueInLoop`, `quit`, and `wakeup`. They marshal work back to the owner loop through the pending-functor queue.
5. Test File Mapping:
   `tests/contract/event_loop/test_event_loop.cpp`

### EventLoopThread

1. Loop / Thread:
   The published `EventLoop*` is owned by the worker thread created by `EventLoopThread`.
2. Ownership / Release:
   The loop is stack-owned inside `threadFunc()`. `EventLoopThread` coordinates `quit + join` during teardown.
3. Re-entrant Callbacks:
   Queued work may schedule additional work on the worker loop before shutdown.
4. Cross-thread Operations:
   Creator thread may call `startLoop()`. After publication, cross-thread interaction with the loop still goes through normal `EventLoop` scheduling APIs.
5. Test File Mapping:
   `tests/contract/event_loop_thread/test_event_loop_thread.cpp`

### EventLoopThreadPool

1. Loop / Thread:
   The pool itself is controlled from the base loop thread. Worker loops are selected for connection assignment but not mutated directly cross-thread.
2. Ownership / Release:
   `EventLoopThreadPool` owns `EventLoopThread` instances and borrows the base loop.
3. Re-entrant Callbacks:
   Thread-init callbacks may run during startup on worker threads or on the base loop when zero worker threads are configured.
4. Cross-thread Operations:
   `start()` and `getNextLoop()` are base-loop-thread operations only.
5. Test File Mapping:
   `tests/contract/event_loop_thread_pool/test_event_loop_thread_pool.cpp`

### TcpServer

1. Loop / Thread:
   `TcpServer` bookkeeping belongs to the base loop thread. Per-connection I/O belongs to each connection's owner loop.
2. Ownership / Release:
   `TcpServer` owns `Acceptor` and thread-pool coordination, and it keeps shared ownership of live `TcpConnection` instances in its base-loop connection map.
3. Re-entrant Callbacks:
   Connection close callbacks may re-enter removal logic by scheduling `removeConnection()` back to the base loop.
4. Cross-thread Operations:
   Cross-loop handoff is explicit: accept on base loop, `connectEstablished()` on io loop, removal marshaled back to base loop, `connectDestroyed()` queued on io loop.
5. Test File Mapping:
   `tests/contract/tcp_server/test_tcp_server.cpp`
   `tests/integration/tcp_server/test_tcp_server_threaded.cpp`

### TcpConnection Coroutine Bridge

1. Loop / Thread:
   `TcpConnection` belongs to exactly one owner loop. Awaiter arming, state mutation, and coroutine resume all converge on that owner loop.
2. Ownership / Release:
   `TcpConnection` owns its socket/channel/buffers and is usually shared across callbacks and awaiters through `shared_ptr`.
3. Re-entrant Callbacks:
   Read, write, close, and error paths may resume awaiters and then trigger connection/close callbacks in the same owner-loop context.
4. Cross-thread Operations:
   Cross-thread send/shutdown and cross-thread awaiter arming must marshal through the owner loop.
5. Test File Mapping:
   `tests/contract/tcp_connection/test_tcp_connection.cpp`
   `tests/integration/coroutine/test_coroutine_echo_server.cpp`

## Behavior Strengthened In This Round

- `queueInLoop()` deferred semantics are contract-tested more directly.
- `quit()` no longer abandons already-queued nested functors; loop exit now drains accepted pending work before returning.
- `EventLoopThread` startup and teardown are covered by a dedicated contract test.
- `EventLoopThreadPool` now has direct tests for base-loop-only `start()` and `getNextLoop()` usage.
- `TcpServer` now has a direct contract for base-loop connection-map insertion and removal in threaded mode.
- coroutine echo integration now asserts that `asyncReadSome()` and `waitClosed()` resume on the actual worker loop thread, not just "not the base thread".

## Test Inventory

- `tests/contract/event_loop/test_event_loop.cpp`
- `tests/contract/event_loop_thread/test_event_loop_thread.cpp`
- `tests/contract/event_loop_thread_pool/test_event_loop_thread_pool.cpp`
- `tests/contract/tcp_server/test_tcp_server.cpp`
- `tests/contract/tcp_connection/test_tcp_connection.cpp`
- `tests/integration/tcp_server/test_tcp_server.cpp`
- `tests/integration/tcp_server/test_tcp_server_threaded.cpp`
- `tests/integration/coroutine/test_coroutine_echo_server.cpp`

## Verification

Recommended verification commands:

```bash
cmake --build build --target check-contract
cd build && ctest --output-on-failure -L contract
cd build && ctest --output-on-failure -L integration
```

## Notes For Review

- The most subtle semantic change is `EventLoop` exit behavior: `quit()` now means "stop future polling, then drain already-accepted pending functors" rather than "stop immediately and potentially strand queued owner-loop work".
- This matters directly for coroutine close-path correctness because close waiters are resumed through owner-loop queueing.
- No new alternative scheduler was introduced; coroutine behavior remains layered on top of `EventLoop` scheduling.
