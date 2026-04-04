# Tests Layout

`tests/` 按“层级 + 模块”组织，避免所有测试平铺在一个目录里。

## 层级

- `unit/`: 只验证单模块局部语义、小不变量、基础回调分发
- `contract/`: 验证公共 API、线程亲和、生命周期和模块间契约
- `integration/`: 验证主链路是否真正跑通，包括 server 主路径与协程桥接

## 当前映射

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

也可以直接使用构建目标：`check-unit`、`check-contract`、`check-integration`、`check-tests`。

## VSCode 断点调试

- 打开任意一个 `tests/.../*.cpp` 测试文件
- 选择 `gdb: debug current test file`
- 按 `F5`，会先自动构建当前文件对应的 CMake 测试目标，再进入断点调试

这个调试入口依赖当前活动编辑器文件路径来推导测试目标，因此需要从测试源文件本身发起调试。
