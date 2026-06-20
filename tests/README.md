# Tests Layout

`tests/` 按“层级 + 模块”组织，避免所有测试平铺在一个目录里。

## 层级

- `unit/`: 只验证单模块局部语义、小不变量、基础回调分发
- `contract/`: 验证公共 API、线程亲和、生命周期和模块间契约
- `integration/`: 验证主链路是否真正跑通，包括 server 主路径与协程桥接
- `fuzz/`: 可选 libFuzzer 入口，验证协议解析器与帧解析器对畸形输入的健壮性

## 风险标签

除 `unit` / `contract` / `integration` 分层标签外，高风险测试还会带有风险维度标签：

- `lifecycle`: 生命周期、关闭、销毁、remove-before-destroy、awaiter resume 等路径
- `threading`: 跨线程投递、owner loop 回流、线程池停止、广播分桶等路径
- `coro`: coroutine suspend/resume、cancel、timeout、combinator 等路径
- `protocol`: HTTP / WebSocket / RPC / codec / framing 等协议解析与序列化路径
- `transport`: TCP / UDP / KCP / transport adapter 等传输抽象路径
- `benchmark`: 轻量性能基线，默认仍作为 integration 测试构建

## 当前映射摘要

- `unit/buffer/`: `Buffer`
- `unit/channel/`: `Channel`
- `unit/coroutine/`: `Task`
- `contract/event_loop/`: `EventLoop`
- `contract/poller/`: `Poller`
- `contract/tcp_connection/`: `TcpConnection`
- `contract/event_loop_thread_pool/`: `EventLoopThreadPool`
- `integration/tcp_server/`: 同步 Reactor 主链路
- `integration/coroutine/`: 协程桥接主链路

## 运行方式

- 全量：`ctest --output-on-failure`
- 只跑 unit：`ctest --output-on-failure -L unit`
- 只跑 contract：`ctest --output-on-failure -L contract`
- 只跑 integration：`ctest --output-on-failure -L integration`
- 生命周期风险集合：`ctest --output-on-failure -L lifecycle`
- 线程边界集合：`ctest --output-on-failure -L threading`
- 协议解析集合：`ctest --output-on-failure -L protocol`
- 性能基线集合：`ctest --output-on-failure -L benchmark`

也可以直接使用构建目标：

- `check-unit`
- `check-contract`
- `check-integration`
- `check-tests`
- `check-lifecycle`
- `check-threading`
- `check-protocol`
- `check-benchmark`

## Fuzz 入口

Fuzz target 默认不进入普通构建。需要使用 Clang 显式启用：

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

## Sanitizer 入口

ASan/UBSan 用于生命周期与未定义行为护栏：

```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DMINI_ENABLE_ASAN_UBSAN=ON
cmake --build build-asan -j$(nproc)
ctest --test-dir build-asan --output-on-failure -L "unit|contract"
```

TSan 用于线程边界与 coroutine 调度护栏。推荐使用 Clang，与 CI 保持一致：

```bash
CC=clang CXX=clang++ cmake -S . -B build-tsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DMINI_ENABLE_TSAN=ON
cmake --build build-tsan -j$(nproc)
ctest --test-dir build-tsan --output-on-failure -L "threading|coro"
```

## VSCode 断点调试

- 打开任意一个 `tests/.../*.cpp` 测试文件
- 选择 `gdb: debug current test file`
- 按 `F5`，会先自动构建当前文件对应的 CMake 测试目标，再进入断点调试

这个调试入口依赖当前活动编辑器文件路径来推导测试目标，因此需要从测试源文件本身发起调试。
