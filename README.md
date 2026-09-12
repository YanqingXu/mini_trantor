# mini-trantor

一个小型、可审计的 C++23 Reactor TCP 网络库。研发重点是线程归属、连接生命周期、
跨线程调度和协程桥接的正确性。

2026-09 审计后，项目停止向游戏框架、协议集合和可靠 UDP 研究平台扩展。
游戏 Session/LogicLoop/AOI 广播、HTTP/WebSocket/RPC、codec 生态、
自研 KCP/PMTU/raw ICMP/FEC 已退出当前源码和安装接口。
旧实现保存在 Git 提交 `3eba368`，迁移影响见[审计与裁剪记录](docs/audit_2026-09-12.md)。

## 当前范围

| 能力 | 定位 | 当前限制 |
| --- | --- | --- |
| EventLoop / Channel / Poller / Buffer | 核心调度与字节缓冲 | 核心候选，尚未作稳定发布承诺 |
| TCP server/client、连接生命周期、基础背压 | 核心网络路径 | 下一阶段优先补关闭、重入与资源上限契约 |
| EventLoopThread / ThreadPool、TimerQueue | 必要运行时支撑 | 已补启动失败回传与停止竞争回归；定时器继续留在 owner loop |
| Task、网络 awaitable、取消/超时/组合器 | 协程预览 | 已补 sleep/TCP 注销和组合器父帧保护；请求取消与统一关闭继续验证 |
| DNS | 已有辅助能力，冻结扩展 | 缓存、关闭投递及有序候选回退已有合同；捕获资源须遵守释放线程约束 |
| TLS | 显式可选，默认关闭 | 需 OpenSSL；单独测试，不等同于完整 TLS 安全审计 |
| PacketFramer | 有界字节 framing 工具 | 不提供应用协议、身份或路由策略 |
| Linux epoll / Windows select | Linux 主验证平台 / Windows 预览 | select 有容量限制，Windows 测试子集不代表全平台等价 |

当前阶段是**收敛与加固**。历史“Stable”“阶段完成”和测试数量不再作为当前成熟度承诺。
审计中的协程悬空恢复、DNS 锁边界与迟到投递、线程启停和 TcpServer 关闭投递已有回归和修复；
DNS 候选回退和 Connector 重入已补合同；接下来处理一般回调异常与重入、TLS 对端身份校验，
再补 TCP 资源边界和负载证据。
最新测试和开放问题以[研发路线](docs/roadmap.md)及其 S1
执行记录为准；部分合同通过不代表全部生命周期已闭环。

## 构建与验证

Linux（GCC 13+，CMake 3.25+，C++23）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

普通 TCP 默认无需 OpenSSL。需要 TLS 时配置 `-DMINI_ENABLE_TLS=ON` 并安装 OpenSSL 开发包。
测试目标在 Release 中也强制启用断言，库本身仍按 Release 编译。

Clang 18 在本机默认搭配 libstdc++ 13 时缺少可用的 `std::expected`。
Clang 检查使用 libc++ 18，需安装 libc++/libc++abi 开发包并配置
`-DCMAKE_CXX_COMPILER=clang++ "-DCMAKE_CXX_FLAGS=-stdlib=libc++ -fexperimental-library"`；
后一个选项用于该版 libc++ 的 jthread 实现。GCC/MSVC 不需要这些选项。

TSan 使用[隔离构建并插桩的 C++ 运行库](docs/tsan_toolchain.md)，先检查合法
shared/weak 释放与故意数据竞争两个独立对照，再运行全部保留测试。
系统未插桩 libc++ 的控制块报告需要对照归因，不能直接作为项目缺陷或通过证据。

Windows（Visual Studio 2026）：

```powershell
cmake --preset windows-vs2026-x64
cmake --build --preset windows-vs2026-x64
ctest --preset windows-vs2026-x64
```

Preset 由 CMake 自动发现 VS 安装位置；构建产物不进入版本控制。
也可手动选择支持 C++23 的 Visual Studio generator 和独立构建目录。

```bash
./build/echo_server
./build/coroutine_echo_server
```

协程示例展示调度路径，不能据此推断任意时刻销毁挂起 Task 已安全。

## 安装消费

```bash
cmake --install build --prefix ./_install
```

```cmake
find_package(mini_trantor CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE mini_trantor::mini_trantor)
```

安装包依赖随 TLS 开关变化。CTest 的 `contract.build.install_consumer` 会检查安装头文件边界，
并通过独立工程编译、链接消费；TLS 关闭时禁止消费工程发现 OpenSSL，以验证没有隐式依赖。

## 工程证据与阅读入口

- [未完成目标交接：当前状态、下一步与环境恢复](docs/HANDOFF.md)
- [深度审计、已修复项、未解决风险及实测记录](docs/audit_2026-09-12.md)
- [唯一当前研发路线](docs/roadmap.md)
- [完整框架理解文档：模块、文件、调用链、生命周期、排错入口](docs/framework_understanding.md)
- [当前架构意图](intents/architecture/reactor_scope_reset.intent.md)
- [本次 core change gate 与生命周期图](docs/core_change_2026-09-12.md)
- [历史文档区](docs/archive/README.md)

CI 验证 Linux Debug/Release、ASan/UBSan、Clang/TSan、Windows 预览合同及 framing fuzz smoke。
配置了 job 不代表本次远端运行已通过；实际本地结果和运行限制统一记入审计记录。
