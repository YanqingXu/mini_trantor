# v5-zeta: Engineering Guardrails

## 1. Intent

在 mini-trantor 已经具备完整运行时能力（v1~v4）和底座一致性（v5-alpha~epsilon）之后，
补全工程护栏，让项目从"能开发"进化到"敢长期维护"。

本阶段不新增任何运行时功能，只增加：
- 每次提交的自动验证（CI）
- 内存安全与未定义行为检测（sanitizer）
- 协议解析器健壮性验证（fuzz）
- 核心路径性能基线（benchmark）
- 安装与消费路径校验（install verification）

---

## 2. In Scope

- CI workflows:
  - `.github/workflows/ci.yml`
  - 触发条件：push（所有分支）+ pull_request（master）
  - 工作：
    - cmake configure
    - cmake build（Debug + Release + Clang Debug）
    - cmake build 时通过 `MINI_ENABLE_ASAN_UBSAN=ON` 启用 ASan + UBSan（Debug 模式）
    - 通过 `MINI_ENABLE_TSAN=ON` 单独运行 TSan 线程风险集合
    - ctest（unit + contract + integration + 风险标签）
    - fuzz target 构建与 smoke run
    - cmake install + find_package 消费验证
    - 示例程序编译验证
  - 平台：ubuntu-latest（GitHub Actions 默认）

- Sanitizer:
  - `cmake/Sanitizers.cmake` 模块
  - AddressSanitizer（ASan）：捕获内存访问越界、use-after-free、double-free
  - UndefinedBehaviorSanitizer（UBSan）：捕获整数溢出、空指针、对齐错误
  - 仅 Debug 模式启用，Release 模式关闭（零运行时开销）
  - ThreadSanitizer（TSan）：独立 Debug job，捕获跨线程调度与共享状态 data race
  - 生命周期敏感模块优先：TcpConnection、EventLoop、Coroutine awaiter

- Fuzz targets:
  - `tests/fuzz/` 目录结构
  - `tests/fuzz/http/fuzz_http_context.cpp` — HTTP 请求解析器 fuzz
  - `tests/fuzz/ws/fuzz_ws_codec.cpp` — WebSocket 帧编解码 fuzz
  - `tests/fuzz/rpc/fuzz_rpc_codec.cpp` — RPC 帧编解码 fuzz
  - `tests/fuzz/framing/fuzz_packet_framer.cpp` — PacketFramer 粘包/半包 fuzz
  - fuzzer 入口：LLVM `LLVMFuzzerTestOneInput` 接口
  - 注：fuzz 是 optional target，仅通过 `-DMINI_ENABLE_FUZZ=ON` 启用；CI 只做 smoke run

- Benchmarks:
  - 当前已有 `tests/integration/benchmark/test_fps_like_broadcast_latency.cpp`
  - 作为轻量 benchmark 标签进入 CTest，可通过 `ctest -L benchmark` 运行
  - 后续再扩展为趋势化性能报告

- Install verification:
  - 在 CI 中验证 `cmake --install` + `find_package` 消费

---

## 3. Non-Responsibilities

- 不改变任何运行时语义
- 不用 raw coverage 数字替代 contract 质量
- 不把失败测试隐藏在 optional 路径后面
- 不添加性能优化
- 不修改已有 public API

---

## 4. Required Checks

- [x] build 能从 clean checkout 成功
- [x] unit/contract 测试在 ASan/UBSan Debug 配置下通过（基线 75/75）
- [x] install + find_package 路径可消费
- [x] 生命周期敏感模块有 sanitizer 覆盖
- [x] CI 配置在 GitHub Actions 上可运行
- [x] TSan 有独立入口覆盖线程/协程风险标签
- [x] Fuzz target 可通过 Clang + libFuzzer 构建并 smoke run
- [x] benchmark 可通过 CTest 风险标签单独运行

---

## 5. Validation

- workflows added:
  - `.github/workflows/ci.yml`
- canonical commands:
  ```bash
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DMINI_ENABLE_ASAN_UBSAN=ON
  cmake --build build -j$(nproc)
  ctest --test-dir build --output-on-failure -L "unit|contract"
  cmake --install build --prefix ./build/_install

  CC=clang CXX=clang++ cmake -S . -B build-tsan \
    -DCMAKE_BUILD_TYPE=Debug \
    -DMINI_ENABLE_TSAN=ON
  cmake --build build-tsan -j$(nproc)
  ctest --test-dir build-tsan --output-on-failure -L "threading|coro"

  CC=clang CXX=clang++ cmake -S . -B build-fuzz \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=OFF \
    -DMINI_ENABLE_FUZZ=ON
  cmake --build build-fuzz -j$(nproc)
  ./build-fuzz/tests/fuzz/fuzz_http_context -runs=1000
  ```
- local validation notes:
  - ASan/UBSan Debug: `ctest -L "unit|contract"` passed 75/75 on GCC 13.3.
  - `MINI_ENABLE_FUZZ=ON` correctly rejects GCC with a clear Clang/libFuzzer requirement.
  - Local GCC TSan build succeeds, but this host's TSan runtime exits with
    `ThreadSanitizer: unexpected memory mapping`; CI uses Clang TSan instead.
- remaining blind spots:
  - macOS / Windows 平台兼容性
  - Valgrind 覆盖
  - Fuzz 仍是入口 + smoke，尚未接入长期 corpus / oss-fuzz
  - benchmark 尚未形成趋势化性能报告

---

## 6. Review Questions

- 新的护栏能捕获什么类别的回归？
  * ASan → use-after-free / heap-buffer-overflow / double-free
  * UBSan → signed-integer-overflow / misaligned-pointer / null-pointer
  * TSan → data race / 跨线程共享状态误用
  * Fuzz → 协议解析器畸形输入崩溃 / 越界 / UB
  * CI → build break / test regression / install break
- 新的护栏是否足够轻量以持续运行？
  * ASan + UBSan 在 Debug 模式下约 2x-3x 性能损耗，对 CI 测试可接受
  * TSan 与 fuzz smoke 独立 job 运行，避免阻塞 Release 主路径
- 哪些高风险模块仍未覆盖？
  * 长时间 soak 下的 coroutine promise / awaiter 生命周期
  * HTTP / WebSocket / RPC / PacketFramer fuzz 仍缺长期 corpus
