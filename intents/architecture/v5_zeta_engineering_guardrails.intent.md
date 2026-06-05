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
    - cmake build（Debug + Release）
    - cmake build 时启用 ASan + UBSan（Debug 模式）
    - ctest（unit + contract + integration 三层）
    - cmake install + find_package 消费验证
    - 示例程序编译验证
  - 平台：ubuntu-latest（GitHub Actions 默认）

- Sanitizer:
  - `cmake/Sanitizers.cmake` 模块
  - AddressSanitizer（ASan）：捕获内存访问越界、use-after-free、double-free
  - UndefinedBehaviorSanitizer（UBSan）：捕获整数溢出、空指针、对齐错误
  - 仅 Debug 模式启用，Release 模式关闭（零运行时开销）
  - 生命周期敏感模块优先：TcpConnection、EventLoop、Coroutine awaiter

- Fuzz targets:
  - `tests/fuzz/` 目录结构
  - `tests/fuzz/http/fuzz_http_context.cpp` — HTTP 请求解析器 fuzz
  - `tests/fuzz/ws/fuzz_ws_codec.cpp` — WebSocket 帧编解码 fuzz
  - fuzzer 入口：LLVM `LLVMFuzzerTestOneInput` 接口
  - 注：fuzz 是 optional target，不在 CI 的默认 build 中；仅通过 `-DENABLE_FUZZ=ON` 启用

- Benchmarks:
  - 当前阶段不新增 benchmark 目标

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
- [x] unit/contract/integration 测试全部通过（基线 68/68）
- [x] install + find_package 路径可消费
- [x] 生命周期敏感模块有 sanitizer 覆盖
- [x] CI 配置在 GitHub Actions 上可运行

---

## 5. Validation

- workflows added:
  - `.github/workflows/ci.yml`
- commands run:
  ```bash
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
  cmake --build build -j$(nproc)
  ctest --test-dir build --output-on-failure
  cmake --install build --prefix ./build/_install
  ```
- remaining blind spots:
  - macOS / Windows 平台兼容性
  - Valgrind / ThreadSanitizer 覆盖（暂不纳入，留给后续阶段）
  - Fuzz 需手动或通过 oss-fuzz 集成（本阶段仅提供入口）

---

## 6. Review Questions

- 新的护栏能捕获什么类别的回归？
  * ASan → use-after-free / heap-buffer-overflow / double-free
  * UBSan → signed-integer-overflow / misaligned-pointer / null-pointer
  * CI → build break / test regression / install break
- 新的护栏是否足够轻量以持续运行？
  * ASan + UBSan 在 Debug 模式下约 2x-3x 性能损耗，对 CI 测试可接受
  * CI 单次运行预计 < 5 分钟
- 哪些高风险模块仍未覆盖？
  * Coroutine promise / awaiter 生命周期（已覆盖 TcpConnection、SleepAwaitable 等）
  * 多线程竞态条件（ThreadSanitizer 留给 v6 阶段）
