# 测试与证据

当前模块范围以 [scope intent](../intents/architecture/reactor_scope_reset.intent.md) 为准。
已撤除功能的测试清单及旧基线失败保存在 [审计记录](../docs/audit_2026-09-12.md)。

- unit：Buffer、Channel、Task/取消、连接协作者、地址、timer id 等局部不变量。
- contract：公开 API、线程归属、关闭顺序、timer/coroutine、DNS 和安装消费。
- integration：TCP echo、线程分配、backpressure、idle timeout、协程与可选 TLS。
- fuzz：只保留 PacketFramer；Clang 工具链、ASan/UBSan 和实现层 coverage instrumentation。
- package：独立 find_package 消费，不从源码 include；检查退休 API 不进入安装包。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
ctest --test-dir build --output-on-failure -L lifecycle
ctest --test-dir build --output-on-failure -L threading
```

测试目标在 Release 中也启用 assert，避免已有 setup 表达式被 NDEBUG 删除。
新增测试把有副作用的调用放在 assert 外，再断言返回结果。测试数量来自本次配置，
不能跨 TLS/平台/阶段直接比较。Windows 当前是可移植子集，新增 TCP 关闭事件契约
在两个平台执行；不以 Windows 子集替代 Linux 全量集成测试。

已知 coroutine frame 销毁、DNS 锁重入和 TLS 对端验证缺口在 S1 账本中。
现有套件未覆盖它们；必须补失败回归并修复后才能关闭，不得用当前通过率宣称安全。

Clang/libc++ 普通构建 57/57，完整 TSan 为 49/57，失败项包含库内竞争、测试同步问题和
待归因的控制块报告；[分诊与堆栈](../docs/audit_2026-09-12.md#71-tsan-失败分诊)是 S1 的验收起点。
PacketFramer 已用 4 份初始语料执行 1,000 次 fuzz；变异目录位于 build，不修改提交的种子。
