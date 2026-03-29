# mini/ 整改清单

本清单把 `mini/` 目录的旧实现，按当前项目的 intent-first 开发哲学重新校正。

## 优先级 1：线程与生命周期硬问题
- [x] 为 `TcpConnection` 补充 intent，明确“错误路径统一收敛到安全关闭”
- [x] 为 `TcpServer` 补充 intent，明确“关闭回调不能依赖裸 `this` 生命周期”
- [x] 修复 `TcpServer` 连接关闭/移除路径中的悬空回调窗口
- [x] 修复 coroutine awaiter 在非 owner 线程直接读取连接内部状态的问题
- [x] 让 `TcpConnection` 的 fatal read/write error 收敛到统一 teardown
- [x] 给 `EventLoop` / `Acceptor` / `Channel` 增加关键 teardown 守卫

## 优先级 2：契约与文档补齐
- [x] 为 `Acceptor` / `TcpConnection` / `TcpServer` 增加 intent 文档
- [x] 为 `Buffer` / `EventLoopThread` / `EventLoopThreadPool` 增加 intent 文档
- [x] 为连接关闭路径补一份生命周期说明文档
- [x] 为本轮修复补最核心的 EventLoop / Channel 合同测试

## 优先级 3：仍然存在的后续工作
- [x] 为 `Poller` / `TcpConnection` / `TcpServer` 增加失败路径与集成契约测试
- [x] 为更多核心头文件补中文说明块，统一到当前编码规范
- [x] 为 echo server usecase 增加端到端契约测试
