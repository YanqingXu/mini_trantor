# 未完成目标交接

更新时间：2026-09-12。本文用于跨会话接续；[研发路线](roadmap.md)仍是唯一阶段路线，
[范围意图](../intents/architecture/reactor_scope_reset.intent.md)决定保留与裁剪边界。

## 交接快照

- 原目标：**“按照建议依次执行！”**，即依次完成 S1 正确性、S2 最小 TCP 基线、S3 负载与发布证据。
- 目标工具在交接时返回 **paused**，目标未完成。本次只交接，不恢复实施；用户要求继续后再接续。
  新会话即使没有原目标工具状态，也应保留这里的未完成任务，不能因 CI 通过将总目标标为完成。
- 交接前 `main` 与 `origin/main` 同步，工作区干净，基线为
  `660cde58602c3f15d667471c0daa1a43fe221544`。本次交接文档提交在该基线之后。
- 最近代码变更为 `0ea897cb866862f28a5de7c80aa1f19b9bae0bda`，完成 DNS 候选回退与 Connector 重入。
- 一般回调异常和 TLS 的后续工作**只有只读调查与方案草稿，没有未提交实现、intent 修改或新测试**。
- 上轮本地构建、测试、辅助审查均已结束；没有需要恢复的本地执行会话。不要复用旧工具 session ID。
  远端 CI 状态应按实际提交重新查询。

| 阶段 | 状态 | 下一步与退出边界 |
| --- | --- | --- |
| S0 裁剪与证据重置 | 已完成 | 保持小型 TCP Reactor 范围，不恢复被裁剪功能 |
| S1 正确性 | 进行中 | 回调异常与清理 → TLS 默认身份校验和失败关闭；完成全部阻塞项与验证矩阵 |
| S2 最小 TCP 基线 | 待实施 | 队列准入、内存硬上限、半关闭/drain、accept 公平性、Windows 行为合同 |
| S3 负载与发布 | 待实施 | 可复现性能基线、持续负载、资源阈值及 0.1 发布候选证据 |

## 用户约束与工作方式

1. **直接在 main 提交并推送 main，不创建分支。** 用户已经明确授权提交和推送。
   先检查状态与差异，不覆盖其他修改；更新远端时优先正常快进，不强推。
2. 所有 agent 临时构建、日志、探针、脚本、归档、提交/PR 描述都放在 `.tmp/` 下。
   推荐 `.tmp/build/`、`.tmp/logs/`、`.tmp/probes/`；`/.tmp/` 已加入 `.gitignore`。
3. 不再在项目主目录创建 `build_*` 文件或目录。旧工具的默认输出路径也必须检查，
   尤其 TSan 脚本要显式传 `.tmp/` 路径，不能直接使用无参数默认值。
4. 先 intent、不变量、线程与所有权合同，再测试和实现；生命周期变更同步状态/时序图，
   每次 core 变更说明回答 AGENTS.md 的五项 gate。
5. 保留能力的测试不得删减或排除；TSan 不使用 suppression 代替归因和修复。
   新测试用可控 I/O、promise/barrier 等建立时序，watchdog 只作截止保护，setup 不放进 assert。
6. TimerQueue 和基础背压继续保留。游戏/session/AOI、HTTP/WS/RPC、自研 KCP/UDP/PMTU/FEC、
   业务指标平台不属于当前路线；archive 中的计划不构成恢复授权。

## 已完成工作与可信证据

按以下顺序读现有记录即可恢复上下文，不需要重做完整项目审计：

| 记录 | 已完成范围 |
| --- | --- |
| [审计账本](audit_2026-09-12.md)、[框架理解](framework_understanding.md) | 范围裁剪、模块边界、风险和阅读入口 |
| [协程生命周期](s1_coroutine_lifecycle.md) | sleep/TCP 等待注销、Task 启动与所有权、组合器父帧及发布顺序 |
| [DNS 生命周期](s1_dns_lifecycle.md) | 缓存锁外回调、取消注册、安全投递、ResolveAwaitable 析构与迟到通知 |
| [线程生命周期](s1_thread_lifecycle.md) | 启动结果、提前退出保留、部分线程池回滚、owner 停止；`19f7355`、`fe45927` |
| [取消回调重入](s1_callback_reentry.md) | 锁外释放捕获、异常不截断观察者通知、WhenAny 取消失败清理；`0b0f209` |
| [服务端关闭](s1_server_close.md) | worker 只投递连接名，base 管理 map；移除跨线程 server 成员访问及强连接保活；`79438ff` |
| [DNS/Connector 候选](s1_dns_candidates.md) | 候选顺序、轮次隔离、终态重入、socket family 失败回退；`0ea897c` |
| [TSan 工具链](tsan_toolchain.md) | 固定版本且完整插桩的 libc++/libc++abi/libunwind，以及独立正负对照 |

最近代码变更的关键合同：hostname 按系统 DNS 顺序逐候选尝试，每项不隐式无限 retry；
stop/disconnect/析构使旧解析和推进无效。最终失败 hook 前发布 Idle，允许显式开启新轮次；
中途重复 connect 保持幂等。Connector hook 使用快照，终态清理先于通知，重入后检查 generation。
固定地址的失败退避与已建立连接断开后的重连是两项独立开关。

新增合同已经存在于 `tests/contract/tcp_client/test_dns_candidates.cpp`、
`tests/contract/connector/test_connector_reentry.cpp`、`tests/contract/net/test_socket_creation.cpp`。
真实 hostname echo 保留系统解析；静态和共享库都验证受控 DNS 边界。

保留背压集成曾在最终 ASan 中失败：worker 的 Disconnected 提前让 base quit，随后移除消息被拒绝。
`tests/integration/tcp_server/test_tcp_server_backpressure_policy.cpp` 已改为收到真实 EOF/reset 后
投递 base 退出，检查 map 清空、worker join 和通知次数；没有放宽原 EOF 期限。
此处是测试退出协议修复，不是删除失败场景。

### 已核对的远端 CI

2026-09-12 交接时通过 `gh run view` 核对：

- `0ea897c`：[run 34685547831](https://github.com/YanqingXu/mini_trantor/actions/runs/34685547831)，**completed / success，8 个 job 全部成功**。
- `660cde5`：[run 34686972548](https://github.com/YanqingXu/mini_trantor/actions/runs/34686972548)，**completed / success，8 个 job 全部成功**。

八项为 gcc-debug、gcc-release、clang-debug、gcc-asan-tls、linux-tsan、
windows-preview Debug/Release、framing-fuzz。历史 `79438ff` 的 DNS 失败已由后续修复及
上述成功运行闭环；这不关闭尚未实施的一般回调异常与 TLS 合同。
交接文档自身或后续提交的 CI 必须另查，不能继承这些 run 的结论。

### 最后一次本地验证（历史快照，未在本次交接重跑）

| 配置 | `0ea897c` 最终结果 |
| --- | --- |
| Linux ASan/UBSan，TLS ON | 75/75 |
| Linux Release，TLS OFF | 72/72 |
| 完整插桩 libc++ TSan，TLS OFF | 72/72 |
| Windows Release，TLS OFF | 34/34 |
| 共享库关键合同和安装消费 | 4/4（定向验证，不是共享库全量） |
| 四个重点 TSan 入口 | DNS candidates、Connector reentry、hostname echo、背压策略各 20 次，共 80 次通过 |

**本地旧产物已清理。** 用户要求删除根目录的审计日志、一次性脚本和全部 `build_*`；
共删除 156 个审计日志、5 个审计 Python 脚本和另 38 个散落文件，19 个构建目录移入回收站。
交接时根目录 `build_*` 为 0。旧插桩运行库源码、安装目录、探针及测试二进制也不在工作区。
历史记录中的 `build_audit_*` 是当时路径，不是现在仍可读取的附件；以持久化记录、测试源码、
Git 和对应 CI 为证据入口。需要新证据时在 `.tmp/` 重建，不自动恢复已清理产物。

## 第一项未完成任务：一般回调异常与清理（拟 S1-04d）

以下是**待写入 intent、待合同验证的工作方案**，不是当前 API 保证。
先读 `intents/modules/` 中 event_loop、loop_handle、channel、timer_queue、acceptor、
event_loop_thread、tcp_connection、tcp_server、tcp_client 及相关 detail 模块 intent，
再读 `rules/` 的线程、所有权、测试和 review 规则。

### 拟定统一合同

1. 保存首个回调异常，完成当前 I/O → 到期 timer → pending 批次中的独立工作；
   同一 Channel/fd 抛出后停止其后续事件回调，其他 Channel 仍有清理机会。
2. 请求退出，逐项处理已经接受及清理过程中嵌套投递的队列工作；最终队列为空的判断
   与 LoopHandle 投递入口关闭需要原子边界，避免接受了任务却永不执行。
3. 恢复 dispatch 标志、当前 Channel 指针和容器一致性后，`EventLoop::loop()` 向调用者
   重抛首个异常。后续异常不覆盖首个；同线程 `runInLoop()` 仍直接向即时调用方传播。
4. 每次调用前才复制**当前**回调，保证自替换不析构正在执行的捕获；不要提前复制整个
   callback 集合，否则前一个回调清除后一个回调会失效。
5. 明确同步销毁边界：裸 Channel/Acceptor 与正在运行的 EventLoop 不能在其活动回调栈
   中被直接销毁，应延迟 owner 释放；tied owner 可由 dispatch guard 保活。
6. 异常保障范围要写清，不凭空承诺所有分配失败均有强保证。Loop/TimerQueue 析构时
   捕获析构又重建调度的行为，需要独立关闭入口合同，不能顺带宣称支持。

### 已读代码中需要处理的点

| 位置 | 已观察的缺口 / 下次检查重点 |
| --- | --- |
| `mini/net/EventLoop.cc` | 抛出会跳过 looping/eventHandling/currentActiveChannel/pending 标志复位、最终排空及入口关闭；pending metric hook 抛出会遗失已经 swap 出来的任务 |
| `mini/net/Channel.cc` | 直接调用成员回调，自替换捕获及异常时 eventHandling 状态未保护 |
| `mini/net/TimerQueue.cc` | 回调异常跳过 expired reset，使索引与定时调度不一致；cancelInLoop 应持有本地 TimerPtr 到两个容器都一致后再释放捕获 |
| `mini/net/Acceptor.cc` | 直接调用成员回调，stop 后无条件 accept 循环仍可能继续；明确成功 fd 在回调入口转移，由接收者立即 RAII 接管，抛出后不能重复 close |
| `mini/net/EventLoopThread.cc` | runtime 异常可能先析构栈上 EventLoop 再撤销已发布指针，使并发 stop 访问失效对象 |
| `mini/net/detail/ConnectionCallbackDispatcher.cc` | 各 hook 直接调用成员，需处理自替换及异常后的通知扇出 |
| `mini/net/detail/ConnectionBackpressureController.cc` | 同样检查 hook 快照、重入与后续状态 |
| `mini/net/TcpConnection.cc` | Disconnected 抛出会跳过 bookkeeping close；connectDestroyed 异常时仍须 remove Channel，并审查 kDisconnecting；TLS hook 后需重查状态 |
| `mini/net/TcpServer.cc` | stop/drain timeout/force close/析构需继续逐连接清理及 join；ForceClosed、IdleTimeout hook 抛出不能跳过实际关闭；析构必须有明确异常策略 |
| `mini/net/TcpClient.cc` | 析构与包装后的连接/TLS 通知须遵守同一清理规则，不能遗漏 owner 释放 |

只修 EventLoop/Channel 的布尔标志不构成该任务完成。尤其要证明连接注销、定时器容器复位、
所有已接受清理工作和 worker 取消发布在同一异常策略下仍成立。

### EventLoopThread 的候选接口与实现边界

候选接口 `std::exception_ptr failure() const;` **尚未添加**：mutex 保护快照，
不声明 noexcept；init/构造失败仍由 startLoop join 后抛出，并保留 failure 快照。
运行时异常应在栈上 EventLoop 仍存活的作用域内捕获，锁内发布 failure 与 Exited；
保留该 loop 到外部 stop/restart，与现有正常提前退出规则一致。

晚到的 starter 若观察到 Exited，返回仍存活的借用对象；不把运行期失败追认为启动期失败。
Stopping 时先撤销 loop 发布再析构；stop/析构不重抛运行期异常。真正的新启动轮次清空旧 failure，
幂等 Running start 不清空。释放旧 exception_ptr 要移到所有相关锁外，包含 controlMutex，
避免异常对象析构重入控制 API 造成死锁。上述设计先以合同校验，再决定最终公开接口。

### 必须补的验证

- queued 回调投递嵌套工作后抛出：同批及嵌套工作执行，首异常回传，结束后 handle 拒绝投递且可安全析构。
- Channel 可控 revents、自替换、后续 Channel 移除失效；metric 自替换并抛出仍不遗失 pending 批次。
- 同期限三个 timer：首个抛出、第二个取消第三个；重复/self-cancel、捕获释放及 owner 存活期间的析构重入。
- Acceptor 回调 stop/替换/抛出与 accepted fd 的单一所有权，无泄漏。
- worker runtime 抛出与 stop 竞争、starter 晚观察、退出对象借用、LoopHandle 关闭及新轮次清空 failure。
- Disconnected/ForceClosed/close hook 抛出仍完成通知、Channel remove、所有连接清理和 worker join。

拟新增 `tests/contract/channel/test_dispatch_exception.cpp`、
`tests/contract/acceptor/test_acceptor_reentry.cpp`、
`tests/contract/timer_queue/test_timer_exceptions.cpp`、
`tests/contract/event_loop_thread/test_runtime_exception.cpp`，**交接时均不存在**。
EventLoop/TCP 的异常合同也尚未写入；文件名可按最终职责调整，随后注册 CMake。
临时红回归放 `.tmp/probes/`，正式合同进入对应 tests 目录。

## 第二项未完成任务：TLS 默认身份与失败关闭

同样只有静态调查，未实施。先完成上述异常合同，再读 [TLS intent](../intents/modules/tls.intent.md)
及 connection_transport / tcp_client / tcp_connection intent。

- `TlsContext::newClientContext` 加载系统 CA 路径但未默认开启 peer 验证，加载结果也未检查。
- transport 当前只设置 SNI，没有验证参考身份；string_view 的终止符/嵌入 NUL 和配置返回值需处理。
- **失败关闭是阻塞项**：`TcpConnection::startTls` 忽略 `enableTls` 返回值；SSL 创建/配置失败
  可能使 ssl 为空并走明文 Connected 路径。失败状态必须阻止连接建立、成功通知及明文收发。
- 身份选择拟定：显式名称优先；hostname 构造默认取原始 DNS 名，候选 IP 回退不能改变它；
  数字地址按 IP 身份校验。DNS 才设置 SNI，非法或缺失参考身份失败。实现时核对 OpenSSL 官方 API 合同。
- 显式 `setVerifyPeer(false)` 作为不认证对端的 TLS 模式单独标明，即便如此也不能失败降级为明文。
- 成功测试使用测试 CA 与匹配 DNS/IP/IPv6 SAN；覆盖不可信、名称不符、初始化失败和 hook 重入关闭。
  失败断言须明确 Connected=0、Completed=0、Failed=1，并观察真实 peer close；
  现有“没有 Connected 或后来 Disconnected”断言不足。
- 已检视的静态测试证书到期日为 2027-04-05；改为测试期生成相对有效期的 CA/leaf，
  放测试构建目录，不依赖机器信任库或外部命令行工具。现有成功路径不能继续仅靠关闭验证。
- 当前 Windows CI 为 TLS OFF，不能声称 Windows TLS 已验证。

## S2 与 S3 接续清单

S2 细项以[路线](roadmap.md)为准，已发现以下验收缺口：

1. 复用 LoopHandle，统一 raw queue 与 quitting/stopped 的准入/反馈，明确配置时机、非法选项和 0 worker 行为。
2. 内存硬上限同时计入跨线程队列 payload 与 outputBuffer；现有高低水位限读不等于硬上限。
   明确拒绝/关闭反馈，用多生产者和慢读者检验 send 在入队前复制数据不能绕过预算。
3. 半关闭保证已接受数据先于 FIN，对端 SHUT_WR 后仍收完整响应。尚未发现保留测试直接覆盖
   `stop(Duration)` 调用；补自然 drain、deadline 强关、重入 stop、0/多 worker、timer 注销和资源归零。
4. Acceptor 目前无 accept 预算；三个连接的功能测试不证明公平性。用受控 Linux 子进程
   RLIMIT_NOFILE 触发 EMFILE，检查无忙转/泄漏、恢复可用及 pending/timer 获得调度。
5. Windows 需补真实 TCP client/connection、半关闭与慢读者合同；select 容量要计内部 Channel，
   形成支持矩阵，不扩展 IOCP/io_uring。
6. callback/coroutine echo 与安装消费继续复用；用全新 install prefix 和 consumer build，
   防止固定旧目录中的残留头文件造成假通过。

S3 尚无保留的 TCP benchmark/soak 工具。新增参数化请求/响应负载，记录提交、硬件、编译器、
线程/连接数、消息大小、吞吐、p50/p95/p99；运行慢读、突发连接、反复重连、停服写入并记录
RSS/FD/排队峰值。PacketFramer 已有 fuzz harness/种子，CI 的 1000 次 smoke 不等于持续验证。
先写资源和延迟阈值，再保存原始结果与最小化失败；不提前做 lock-free/pool 或协议扩张。

总目标只在 S1 无开放阻塞项、S2 合同/平台矩阵完成、S3 可复现基线及持续运行记录齐全后关闭。

## 环境恢复与构建入口

本机：Windows 仓库 `G:\github\mini_trantor`；WSL `Ubuntu-24.04`、用户 `xyq`，
Linux 路径 `/mnt/g/github/mini_trantor`。先重新核对工具是否可用，不能假设旧 build cache 存在。
既有验证环境为 GCC 13、Clang 18、Visual Studio 2026。

Linux 命令在仓库根执行。曾遇 `/tmp` 编译临时文件消失，统一显式 TMPDIR；
WSL 冷启动可能较慢，确认已有命令仍在运行时轮询原会话，不因观察超时重复启动。

```bash
mkdir -p .tmp/compiler .tmp/logs
export TMPDIR="$PWD/.tmp/compiler"
cmake -S . -B .tmp/build/asan -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON -DMINI_ENABLE_TLS=ON -DMINI_ENABLE_ASAN_UBSAN=ON
cmake --build .tmp/build/asan --parallel 4
ctest --test-dir .tmp/build/asan --output-on-failure --timeout 120
```

Linux Release 另建 `.tmp/build/release`，配置 `CMAKE_BUILD_TYPE=Release`、
`BUILD_TESTING=ON`、`MINI_ENABLE_TLS=OFF`；普通 Debug 与共享库也使用各自新目录。
TLS ON 需要 OpenSSL 开发包。新实现完成后执行相关定向合同及所需完整矩阵，不用旧测试数量硬编码验收。

TSan 运行库已经清理，首次使用必须重建。以下显式工作根参数不可省略：

```bash
bash tests/toolchain/build_tsan_runtime.sh "$PWD/.tmp/tsan-runtime"
prefix="$(cat .tmp/tsan-runtime/build/runtime-prefix.txt)"
cmake -S . -B .tmp/build/tsan -DCMAKE_CXX_COMPILER=clang++-18 \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DMINI_ENABLE_TLS=OFF -DMINI_ENABLE_TSAN=ON \
  "-DCMAKE_CXX_FLAGS=-fsanitize=thread -stdlib=libc++ -fexperimental-library -nostdinc++ -isystem $prefix/include/c++/v1 -L$prefix/lib -Wl,-rpath,$prefix/lib"
cmake --build .tmp/build/tsan --parallel 4
ctest --test-dir .tmp/build/tsan --output-on-failure --timeout 120
```

脚本固定 LLVM `llvmorg-18.1.3` / `c13b7485b87909fcf739f62cfa382b55407433c0`，
检查三个运行库加载路径、合法 shared/weak 对照退出 0 和故意 data race 对照报告且退出 66。
这些检查失败不能继续声称有效 TSan 验证；系统未插桩库的报告需独立归因，实际实例字段竞争仍须修复。
编译/链接参数一起放 `CMAKE_CXX_FLAGS` 以供安装消费传递；RPATH 固定后不要移动 runtime prefix。

Windows 不使用指向其他输出目录的 preset，显式指定 `.tmp/`：

```powershell
$taskCmake = 'D:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$taskCtest = 'D:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
& $taskCmake -S . -B .tmp/build/windows -G 'Visual Studio 18 2026' -A x64 -DBUILD_TESTING=ON -DMINI_ENABLE_TLS=OFF
& $taskCmake --build .tmp/build/windows --config Release --parallel 4
& $taskCtest --test-dir .tmp/build/windows -C Release --output-on-failure --timeout 120
```

需要 Debug 时同一多配置目录改用 `--config Debug` / `-C Debug`。
这些是后续恢复命令，本次交接没有执行构建，也没有产生新的 sanitizer 结论。

## 下次会话开始时

1. 读 `AGENTS.md` → 本文 → `docs/roadmap.md` → 当前范围 intent；按本次任务再读相关 module intent/rules。
2. 检查当前用户要求与目标状态。只有用户继续研发时才恢复上述顺序；交接本身不等于恢复目标。
3. 核对 `git status --short --branch`、`git log -5 --oneline`；若需同步，先 fetch 再检查差异，
   保持 main，工作区允许时 `git pull --ff-only origin main`，不覆盖未知修改或强推。
4. 用 `gh run list --branch main` 找到当前提交，并以 `gh run view <run-id>` 检查实际结论。
   工作区和 CI 有新状态时更新本文，不能继续引用本快照作为最新事实。
5. 从一般回调异常项开始：写 intent 和确定性失败合同，再实现；补图、五项 gate 和实测证据。
6. 每组边界清楚的改动验证后直接提交并推送 main，更新路线实施记录和本文，明确剩余工作。
   全阶段门槛未满足前继续保持总目标未完成。
