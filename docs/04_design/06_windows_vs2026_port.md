# Windows / VS2026 跨平台移植设计

## Intent

本次移植让 mini-trantor 在 Windows + Visual Studio 2026 / MSVC 下可构建，
同时保持现有 reactor 语义：Channel、Poller、EventLoop、TimerQueue、
TcpConnection 的线程归属和生命周期规则不变。

## 后端选择

- Linux: `Poller::newDefaultPoller()` 返回 `EPollPoller`。
- Windows: `Poller::newDefaultPoller()` 返回 `SelectPoller`。
- `Channel` 暴露 backend-neutral 事件：`kReadEvent`、`kWriteEvent`、`kErrorEvent`、`kCloseEvent`。
- `EPollPoller` 负责 epoll event 与 Channel event 的双向转换。
- `SelectPoller` 负责 WinSock `select()` fd_set 与 Channel event 的转换。

## Source Layout

- `mini/net/platform/SocketTypes.h` 定义 `SocketFd`、`socklen_t`、`ssize_t` 等平台 socket 类型。
- `mini/net/platform/SocketsOps_win.cc` 与 `mini/net/platform/SocketsOps_linux.cc` 分开实现 WinSock / POSIX socket 操作。
- `mini/net/platform/Wakeup_win.cc` 与 `mini/net/platform/Wakeup_linux.cc` 分开实现 EventLoop wakeup。
- `mini/net/poller/SelectPoller.*` 与 `mini/net/poller/EPollPoller.*` 分开承载具体 poller 后端。
- `mini/net/SocketTypes.h`、`mini/net/SelectPoller.h`、`mini/net/EPollPoller.h` 保留为兼容转发头，避免上层 include 大面积迁移。
- `mini/net/poller/PollerFactory.cc` 集中实现默认后端选择。

## Wakeup 与 Timer

```mermaid
sequenceDiagram
    participant W as Worker Thread
    participant L as EventLoop
    participant P as Poller
    participant T as TimerQueue

    W->>L: queueInLoop(fn)
    L->>L: wakeup write (eventfd or socket pair)
    P-->>L: readable wakeup channel
    L->>L: drain wakeup
    L->>T: handleExpired(now)
    L->>L: doPendingFunctors()
```

Windows 没有 `eventfd` / `timerfd`。因此：

- `EventLoop` 在 Windows 使用 WinSock loopback socket pair 作为 wakeup 句柄。
- `TimerQueue` 不再注册 timer fd，而是让 EventLoop 在每轮 poll 前计算最近超时。
- 所有 timer callback 仍由 owner EventLoop 执行。

## Unsupported Platform Features

- `SignalWatcher` 基于 Linux `signalfd`，Windows 构造时显式抛出 unsupported。
- Linux UDP PMTU error queue / raw ICMP listener 在 Windows 上保持 capability=false。
- 这些 unsupported path 不影响普通 TCP/UDP、HTTP/WebSocket/RPC/KCP 编译。

## Build

```powershell
& "D:\VS2026\2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --preset windows-vs2026-x64
& "D:\VS2026\2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build --preset windows-vs2026-x64
& "D:\VS2026\2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --preset windows-vs2026-x64
```

preset 固定 `CMAKE_GENERATOR_INSTANCE=D:/VS2026/2026`，以便同机存在 VS2026 Insiders 或其它 Visual Studio instance 时仍使用目标安装路径。

Windows preset 默认 `MINI_ENABLE_TLS=OFF`，普通 TCP/UDP、HTTP、WebSocket、RPC、KCP 和示例不依赖 OpenSSL。若 OpenSSL 由 vcpkg 提供，可显式开启 TLS：

```powershell
& "D:\VS2026\2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --preset windows-vs2026-x64 `
  -DMINI_ENABLE_TLS=ON `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
```

## Verified Snapshot

- Configure: VS2026 bundled CMake `4.2.3-msvc3`, generator `Visual Studio 18 2026`, instance `D:/VS2026/2026`.
- Build: `mini_trantor.lib`, `echo_server.exe`, `coroutine_echo_server.exe`, `game_server.exe`, platform split sources, and Windows test targets built in Debug.
- Test: `ctest --preset windows-vs2026-x64`, 27/27 passed.

## Core Module Change Gate

1. **Loop / Thread**: `EventLoop` 仍由创建它的 owner thread 拥有；`SelectPoller`、`TimerQueue`、wakeup Channel 都只在该 owner thread 被驱动。
2. **Ownership / Release**: `EventLoop` owns `Poller`、`TimerQueue`、wakeup descriptors；`Poller` observes `Channel` and never owns it；`Socket` owns `SocketFd` unless `releaseFd()` transfers it upward.
3. **Re-entrant Callbacks**: Channel read/write/error/close callbacks、timer callbacks、pending functors 仍可能 re-enter public APIs such as `send()` / `shutdown()` / `queueInLoop()`，但 mutation 继续通过 owner loop。
4. **Cross-thread Operations**: `runInLoop()` / `queueInLoop()` / `quit()` / TCP send-shutdown paths marshal through EventLoop wakeup. Windows wakeup uses socket pair; Linux keeps eventfd.
5. **Test File Mapping**:
   - `tests/contract/event_loop/test_event_loop.cpp`
   - `tests/contract/timer_queue/test_timer_queue.cpp`
   - `tests/contract/channel/test_channel_contract.cpp`
   - `tests/contract/poller/test_poller_contract.cpp`
   - `tests/contract/tcp_connection/test_tcp_connection.cpp`
   - `tests/contract/tcp_client/test_tcp_client.cpp`
   - `tests/contract/tcp_server/test_tcp_server.cpp`
   - Windows smoke/contract subset from `tests/CMakeLists.txt`, verified by `ctest --preset windows-vs2026-x64`.
