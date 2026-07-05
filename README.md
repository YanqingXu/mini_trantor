# mini-trantor

mini-trantor 是一个面向游戏服务端实验的现代 C++23 异步网络框架。

它用来验证 Reactor、Coroutine、RPC、WebSocket、UDP/KCP、Session Pipeline 与 Intent 驱动开发流程如何组合在一个可演进的网络底座里。它参考 trantor/muduo 的 Reactor 思想，但目标不是直接替代 trantor、muduo、asio 或任何成熟生产框架。

## What It Is

mini-trantor 适合：

- 学习 Reactor、one-loop-per-thread、连接生命周期、跨线程投递和定时器模型。
- 实验 C++20/23 coroutine 如何在不绕开 EventLoop 的前提下接入网络 I/O。
- 构建游戏服务端网络底座的样板：transport、session、framing、codec、logic loop、broadcast。
- 验证 AI 辅助开发在 intent、rules、contract tests 约束下是否能稳定演进复杂 C++ 项目。

mini-trantor 当前不是：

- trantor / muduo / asio 的生产替代品。
- 隐藏线程所有权的全局异步 runtime。
- 无边界的通用中间件集合。
- 已经完成长期 soak、真实压测、跨平台兼容和完整 fuzz corpus 的生产级网络库。

## Current Status

当前项目已经形成一条完整但仍在收敛边界的主线：

- Reactor core：`EventLoop` / `Channel` / `Poller` / `TimerQueue`。
- TCP 主链路：`TcpServer` / `TcpClient` / `TcpConnection` / `Connector` / `Acceptor`。
- 线程模型：`EventLoopThread` / `EventLoopThreadPool`，坚持 one-loop-per-thread。
- Coroutine bridge：`Task<T>`、`SleepAwaitable`、`WhenAll`、`WhenAny`、`Timeout`、`ResolveAwaitable`。
- 协议层：HTTP/1.1、WebSocket、RPC，协议层通过窄接口适配连接。
- 客户端生态：`HttpClient`、`RpcConnectionPool`、DNS、TLS、IPv6。
- 游戏网络底座：TCP/UDP transport、KCP preview、PacketFramer、CodecAdapter、SessionManager、LogicLoop、GameServerPipeline、广播与指标 hook。
- 工程护栏：GitHub Actions、ASan/UBSan、TSan 入口、fuzz 入口、benchmark 标签、install + `find_package` 验证。
- 平台后端：Linux 使用 `EPollPoller`，Windows 使用 `SelectPoller` 预览后端；核心 EventLoop/TimerQueue/TCP 语义保持一致。

最近本地验证快照：

- ASan/UBSan Debug：`ctest -L "unit|contract"`，75/75 passed。
- Benchmark：Release 与 ASan 配置下 `ctest -L benchmark` passed。
- Install verify：Release install 后，临时 consumer `find_package(mini_trantor CONFIG REQUIRED)` + link passed。
- Fuzz/TSan：CI 已配置 Clang 入口；本机无 Clang，GCC TSan runtime 存在 `unexpected memory mapping` 限制。

详细执行记录见 [plan_and_execute.md](plan_and_execute.md)。

## Module Maturity Matrix

状态含义：

- `Stable`：主路径和核心生命周期已有 contract/integration 覆盖，变更必须保持兼容。
- `Beta`：可用于实验或非核心服务路径，但失败路径、race、长跑或 fuzz 仍需补强。
- `Experimental`：用于方向验证或 vertical slice，接口和策略仍可能调整。

| 模块 | 状态 | 当前依据 | 升级或保持条件 |
| --- | --- | --- | --- |
| `EventLoop` / `Channel` / `Poller` / `EPollPoller` / `SelectPoller` / `TimerQueue` | Stable | Reactor 主链路与 owner-thread contract 已建立，ASan 覆盖核心路径；Windows select 后端保持同一 Channel 语义 | 持续通过 ASan/UBSan；TSan 风险集合长期无告警；Windows VS2026 构建持续验证 |
| `Buffer` / `Acceptor` / `TcpConnection` / `TcpServer` | Stable | 服务端主路径、关闭路径、连接生命周期与 coroutine awaiter 有 contract/integration 覆盖 | 增加更多 close race、half-close、soak 与 backpressure 压测 |
| `EventLoopThread` / `EventLoopThreadPool` | Stable | one-loop-per-thread、stop、join、queued functor drain 语义已有测试 | Clang TSan 长期覆盖 threading 标签 |
| `TcpClient` / `Connector` / `DnsResolver` | Beta | 主链路、hostname connect、DNS awaitable、重连路径可用 | 补齐 DNS fail/cache-expire/cancel race 与更多重连压力测试 |
| `Task` / `SleepAwaitable` / `WhenAll` / `WhenAny` / `Timeout` / `ResolveAwaitable` | Beta | cancellation、timeout、winner/loser cancel、awaiter 生命周期已有 contract | 加入更长时间 double-resume/cancel race soak 与 TSan CI 证据 |
| TLS / IPv6 / graceful shutdown / signal | Beta | TLS echo/handshake、IPv6 connect、graceful shutdown 基线可用 | 补齐证书失败、peer close during handshake、shutdown stress |
| HTTP / WebSocket / RPC | Beta | server/client/pool 主路径可用，协议层已与 transport 窄接口解耦 | fuzz corpus 长跑、畸形输入覆盖、协议错误路径补齐 |
| `ProtocolConnectionAdapter` / `PacketFramer` / `CodecAdapter` | Beta | adapter、framing、codec roundtrip 有 unit/contract/integration，已新增 fuzz 入口 | fuzz smoke 进入 CI 后继续扩展 corpus 与 oversized/halfpack 压测 |
| BroadcastRouter / BroadcastDispatcher / PayloadPool / metrics hooks | Beta | owner-loop fanout、payload sharing、广播延迟 benchmark 已存在 | 扩大连接数、group/aoi-id bucket、重连窗口压力基线 |
| UDP / KCP transport | Experimental | loopback、reliable-flow、transport abstraction 基线已跑通 | 保持 KCP/PMTU/FEC preview 标签与 opt-in；补基础 UDP/KCP 游戏消息路径验证 |
| PlayerSession / SessionManager / LogicLoop / GameServerPipeline | Experimental | vertical slice 已接通：framed packet -> auth/session -> logic -> response/broadcast | 示例 main、长跑、reconnect replay 压测、边界文档持续同步 |

## Architecture Map

```text
Client
  |
TCP / TLS / UDP / KCP preview
  |
TransportEndpoint / TransportManager
  |
PacketFramer / CodecAdapter
  |
Game Network Foundation
  └── GameServerPipeline
        ├── SessionManager / PlayerSession
        ├── LogicLoop / GameCommandQueue
        ├── BroadcastRouter / BroadcastDispatcher
        └── TransportManager
              ├── TCP endpoint
              ├── UDP endpoint
              └── KCP endpoint (preview)

Upper Game Server (out of core)
  ├── Actor / Scene / Room
  ├── DB / Redis / service proxy
  └── Match / Rank / Guild / Mail

Protocol Layer
  ├── HTTP / HttpClient
  ├── WebSocket
  ├── RPC / RpcConnectionPool
  └── PacketFramer + CodecAdapter

Async Layer
  ├── Task<T>
  ├── CancellationToken
  ├── SleepAwaitable / ResolveAwaitable
  ├── WhenAll / WhenAny
  └── Timeout

Reactor Core
  ├── EventLoop
  ├── Channel
  ├── Poller / EPollPoller / SelectPoller
  ├── TimerQueue
  └── PendingFunctor queue

Thread Model
  └── EventLoopThread / EventLoopThreadPool
```

核心原则：所有 I/O、timer、coroutine resume、跨线程操作都必须回到 owner `EventLoop` 的调度语义中，不能引入绕过 EventLoop 的隐藏 runtime。

## Quick Start

### Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### Windows / Visual Studio 2026

Windows 构建使用 CMake 的 `Visual Studio 18 2026` generator。项目提供 preset：

```powershell
cmake --preset windows-vs2026-x64
cmake --build --preset windows-vs2026-x64
ctest --preset windows-vs2026-x64
```

如果 OpenSSL 由 vcpkg 提供，可在配置时加上 toolchain：

```powershell
cmake --preset windows-vs2026-x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
```

Windows 当前后端说明：

- `EventLoop` 使用 WinSock loopback socket pair 作为 wakeup 机制。
- `Poller::newDefaultPoller()` 在 Windows 返回 `SelectPoller`，在 Linux 返回 `EPollPoller`。
- `TimerQueue` 由 `Poller::poll(timeoutMs)` 驱动，不依赖 Linux `timerfd`。
- Linux-only `SignalWatcher` / raw ICMP PMTU listener 在 Windows 上显式不可用；普通 TCP/UDP、HTTP/WebSocket/RPC/KCP 代码可编译。
- Windows 测试矩阵先覆盖核心可移植契约子集；Linux 继续构建完整测试矩阵。

### Run Examples

```bash
./build/echo_server
./build/coroutine_echo_server
./build/game_server 8890 0
```

`game_server` 使用 `PacketFramer` 帧协议：`auth=1`、`command=2`、`broadcast=3`、`response=4`。认证 payload 可使用 `<session>` 或 `<session>|<nonce>`；成功后默认 command 响应为 `logic:<payload>`，broadcast 响应为 `broadcast:<payload>`。

### Install And Consume

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-release -j$(nproc)
cmake --install build-release --prefix ./build-release/_install
```

External CMake project:

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
find_package(mini_trantor CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE mini_trantor::mini_trantor)
```

Minimal server:

```cpp
#include "mini/base/Timestamp.h"
#include "mini/net/Buffer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpServer.h"

int main() {
    mini::net::EventLoop loop;
    mini::net::TcpServer server(&loop, mini::net::InetAddress(8080, true), "echo");
    server.setMessageCallback([](const mini::net::TcpConnectionPtr& conn,
                                 mini::net::Buffer* buffer) {
        conn->send(buffer->retrieveAllAsString());
    });
    server.start();
    loop.loop();
}
```

## Engineering Guardrails

默认测试分层：

```bash
ctest --test-dir build --output-on-failure -L unit
ctest --test-dir build --output-on-failure -L contract
ctest --test-dir build --output-on-failure -L integration
```

风险标签：

```bash
ctest --test-dir build --output-on-failure -L lifecycle
ctest --test-dir build --output-on-failure -L threading
ctest --test-dir build --output-on-failure -L coro
ctest --test-dir build --output-on-failure -L protocol
ctest --test-dir build --output-on-failure -L benchmark
```

ASan/UBSan：

```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DMINI_ENABLE_ASAN_UBSAN=ON
cmake --build build-asan -j$(nproc)
ctest --test-dir build-asan --output-on-failure -L "unit|contract"
```

TSan 推荐使用 Clang，与 CI 保持一致：

```bash
CC=clang CXX=clang++ cmake -S . -B build-tsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DMINI_ENABLE_TSAN=ON
cmake --build build-tsan -j$(nproc)
ctest --test-dir build-tsan --output-on-failure -L "threading|coro"
```

Fuzz 入口需要 Clang/libFuzzer：

```bash
CC=clang CXX=clang++ cmake -S . -B build-fuzz \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=OFF \
  -DMINI_ENABLE_FUZZ=ON
cmake --build build-fuzz -j$(nproc)
./build-fuzz/tests/fuzz/fuzz_http_context -runs=1000
./build-fuzz/tests/fuzz/fuzz_ws_codec -runs=1000
./build-fuzz/tests/fuzz/fuzz_rpc_codec -runs=1000
./build-fuzz/tests/fuzz/fuzz_packet_framer -runs=1000
```

Benchmark：

```bash
ctest --test-dir build --output-on-failure -L benchmark
```

## Intent Driven Workflow

mini-trantor 不是“先写代码，再补说明”的项目。重要模块遵循：

1. intent
2. invariants
3. threading rules
4. ownership rules
5. contracts
6. tests
7. implementation

核心模块变更必须回答五个问题：

1. 这个模块归属哪个 loop / thread？
2. 谁拥有它，谁释放它？
3. 哪些回调可能重入？
4. 哪些操作允许跨线程，如何投递？
5. 对应哪个测试文件验证？

相关入口：

- [AGENTS.md](AGENTS.md)
- [rules/thread_affinity_rules.md](rules/thread_affinity_rules.md)
- [rules/ownership_rules.md](rules/ownership_rules.md)
- [rules/testing_rules.md](rules/testing_rules.md)
- [docs/core_module_change_gate.md](docs/core_module_change_gate.md)

## Directory Map

- `intents/`: 设计意图与模块边界。
- `rules/`: 线程亲和、所有权、测试、编码、review 规则。
- `mini/base/`: 基础工具。
- `mini/net/`: Reactor core、TCP、DNS、TLS、signal、transport、broadcast、framing、UDP/KCP。
- `mini/coroutine/`: coroutine bridge、取消、timeout、组合 awaitable。
- `mini/http/`: HTTP server/client 与增量解析。
- `mini/ws/`: WebSocket handshake、codec、connection、server。
- `mini/rpc/`: RPC codec、channel、server、client、connection pool。
- `mini/codec/`: Protobuf-style / FlatBuffers-style codec adapter。
- `mini/game/`: PlayerSession、SessionManager、LogicLoop、GameServerPipeline。
- `tests/`: `unit`、`contract`、`integration`、`fuzz` 分层测试。
- `examples/`: 最小 echo、coroutine echo 与 game server vertical-slice 示例。
- `docs/`: 架构阅读、模块说明、路线图、调用链与设计记录。

## Roadmap

短期优先级：

1. P1：README 与模块状态持续同步，避免定位漂移。
2. P2：扩展 benchmark、长期 fuzz corpus、soak test 与生产就绪度文档。
3. Scope hardening：持续打磨 `game_server` vertical-slice 示例、scope gate、测试标签和边界文档。
4. v6-alpha：继续补齐客户端生态示例与 HTTP/RPC client 复用能力。
5. 游戏底座后续：重连窗口长跑、广播规模压测、基础 UDP/KCP 消息路径验证；AOI、账号、安全平台和分布式网关默认进入 adapter/example/downstream。

详细路线：

- [plan_and_execute.md](plan_and_execute.md)
- [docs/roadmap.md](docs/roadmap.md)
- [docs/roadmap_game_server_network_base_execution_plan.md](docs/roadmap_game_server_network_base_execution_plan.md)
- [docs/game_server_network_base_scope_boundary.md](docs/game_server_network_base_scope_boundary.md)
- [intents/architecture/game_network_base_scope.intent.md](intents/architecture/game_network_base_scope.intent.md)
- [intents/architecture/v5_zeta_engineering_guardrails.intent.md](intents/architecture/v5_zeta_engineering_guardrails.intent.md)
- [intents/architecture/v6_stages.intent.md](intents/architecture/v6_stages.intent.md)
