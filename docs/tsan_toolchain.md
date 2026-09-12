# TSan 运行库与报告归因

TSan 使用隔离安装的 libc++、libc++abi 和 libunwind，三者与项目一起插桩。
只给项目加 `-fsanitize=thread`、链接系统未插桩 libc++ 的组合，不作为当前关闭
shared/weak 控制块报告的依据。TSan 官方说明，未插桩代码中的原子同步不可见，
可能产生误报或漏报；这是一项工具限制，不能自动否定具体代码报告。
[官方说明](https://github.com/google/sanitizers/wiki/ThreadSanitizerCppManual#non-instrumented-code)。

## 独立对照

`tests/toolchain/shared_weak_probe.cpp` 不包含项目代码：主线程释放 weak_ptr，
worker 释放另一个 shared_ptr 实例，每轮 join 后再开始下一轮，共 2000 轮。
两个智能指针实例没有被多线程共同修改。系统 libc++ 18.1.3 的运行结果报告
控制块删除与 `__shared_count::__release_shared` 读取冲突；同一测试链接插桩
运行库时通过。该版本的弱引用释放包含库内原子操作，见
[固定版本实现](https://github.com/llvm/llvm-project/blob/llvmorg-18.1.3/libcxx/src/memory.cpp)。

同一文件以 `MINI_TSAN_NEGATIVE_CONTROL` 编译时，故意让两个线程写同一 int。
验证脚本要求出现 data-race 报告且退出码为 66，避免将检测器未工作或启动失败
当作验证成功。脚本同时通过 ldd 核查三个运行库的实际加载路径。

本地证据：合法对照退出 0，负对照报告竞争并退出 66；脚本重复执行成功。
这只解释上述控制块模式。插桩运行库的全量首轮曾为 68/68，重复运行仍抓到
`TcpServer::~TcpServer` 重置 `lifetimeToken_` 与 worker `removeConnection`
读取同一 shared_ptr 实例的竞争；该报告是项目缺陷，修复记录见
[关闭通知边界](s1_server_close.md)。测试数量是该次快照，不是最新通过率。

## 复现命令

Ubuntu 24.04，安装 CMake、Ninja、Clang 18、GCC 开发环境和 Git。脚本固定 LLVM
tag `llvmorg-18.1.3`，并验证提交 `c13b7485b87909fcf739f62cfa382b55407433c0`。
这是复现基线，不声明此版本为最新 LLVM。运行库采用 LLVM 的 runtimes CMake
入口和独立 prefix，参见 [libc++ 构建文档](https://libcxx.llvm.org/VendorDocumentation.html)。

```bash
bash tests/toolchain/build_tsan_runtime.sh
prefix="$(cat build_tsan_runtime/build/runtime-prefix.txt)"
cmake -S . -B build_tsan -DCMAKE_CXX_COMPILER=clang++-18 \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DMINI_ENABLE_TLS=OFF -DMINI_ENABLE_TSAN=ON \
  "-DCMAKE_CXX_FLAGS=-fsanitize=thread -stdlib=libc++ -fexperimental-library -nostdinc++ -isystem $prefix/include/c++/v1 -L$prefix/lib -Wl,-rpath,$prefix/lib"
cmake --build build_tsan --parallel 4
ctest --test-dir build_tsan --output-on-failure --timeout 120
ctest --test-dir build_tsan --output-on-failure --timeout 120 --repeat until-fail:10 \
  -R 'contract.dns.test_dns_lifetime|contract.coroutine.test_cancel_during_teardown|contract.tcp_server.test_close_during_destruction|integration.tcp_server.test_tcp_server$|integration.tcp_server.test_tcp_server_threaded|integration.coroutine.test_coroutine_idle_timeout'
```

脚本默认全部写入被忽略的 `build_tsan_runtime/`，不替换系统库。第一个参数可以
改变工作根目录；已存在的源码、构建和安装目录分别可通过 `MINI_LLVM_SOURCE_DIR`、
`MINI_TSAN_RUNTIME_BUILD`、`MINI_TSAN_RUNTIME_PREFIX` 复用。已有源码必须保持
固定提交且无 tracked 修改。运行库可重复配置和增量构建；安装 prefix 不应在后续
项目构建或运行时移动，因为它记录在 RPATH 中。

`LLVM_USE_SANITIZER=Thread` 覆盖三个 C++ 运行库；禁用的是依赖自身的测试目标，
项目全部保留测试仍运行。CI 使用独立 `linux-tsan` job 执行相同脚本和全量测试，
随后重复历史报告入口。没有 suppressions、ignorelist 或按 TSan 排除的项目测试。
运行库验证失败时 job 直接失败；诊断和 CTest 日志作为 artifact 保存。

编译和链接所需选项一起保存在 `CMAKE_CXX_FLAGS`，使安装消费合同可以把同一组
参数传给独立消费工程。这会产生部分“链接参数未用于编译”的 Clang 提示，
不会改变实际链接路径。若以后拆分 toolchain/linker 参数，必须同步修改消费合同
的参数传递，不能只验证库本身编译。

## 证据边界

本地验证不等于远端 CI 已通过；远端结论需检查具体 run。TSan 当前验证普通 TCP
配置，TLS 由 ASan/UBSan 的启用配置覆盖。系统未插桩库的旧日志保留供对照；
任何新的实例字段竞争、应用数据竞争或插桩配置下报告都必须单独调查。
