# 审计缺陷的独立复现

这些程序验证审计中的**未解决问题**，预期会报错或超时。它们不计入通过的 CTest 数量，
也不被描述为现有回归覆盖。S1 修复时，应把对应场景转成期望安全退出的常规 contract test。

将下列代码保存为本地 `repro.cpp`（不作为库源码）：

```cpp
#include "mini/coroutine/SleepAwaitable.h"
#include "mini/coroutine/Task.h"
#include "mini/net/DnsResolver.h"
#include "mini/net/EventLoop.h"
#include "mini/net/TlsContext.h"
#include <openssl/ssl.h>
#include <chrono>
#include <cstdio>
#include <string_view>
using namespace std::chrono_literals;
mini::coroutine::Task<void> sleeper(mini::net::EventLoop* loop) {
    co_await mini::coroutine::asyncSleep(loop, 1ms);
}
int main(int argc, char** argv) {
    const auto mode = argc > 1 ? std::string_view(argv[1]) : "sleep";
    if (mode == "tls") {
        const auto ctx = mini::net::TlsContext::newClientContext();
        std::printf("TLS default verify mode: %d (SSL_VERIFY_NONE=%d)\n",
            SSL_CTX_get_verify_mode(ctx->nativeHandle()), SSL_VERIFY_NONE);
        return 0;
    }
    mini::net::EventLoop loop;
    if (mode == "dns") {
        mini::net::DnsResolver resolver(1);
        resolver.enableCache(60s);
        resolver.resolve("localhost", 80, &loop, [&](auto result) {
            if (!result) { loop.quit(); return; }
            resolver.resolve("localhost", 80, &loop, [&](auto) {
                std::puts("cache hit callback: entering clearCache");
                std::fflush(stdout);
                resolver.clearCache();
                std::puts("clearCache returned");
                loop.quit();
            });
        });
        loop.loop();
    } else {
        { auto task = sleeper(&loop); task.start(); }
        loop.runAfter(20ms, [&] { loop.quit(); });
        loop.loop();
    }
}
```

在 Linux/GCC 上：

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DMINI_ENABLE_TLS=ON -DMINI_ENABLE_ASAN_UBSAN=ON
cmake --build build-asan --parallel 4
c++ -std=c++23 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DMINI_ENABLE_TLS=1 -I. repro.cpp build-asan/libmini_trantor.a \
  -lssl -lcrypto -pthread -o repro
./repro sleep
./repro tls
timeout 4s ./repro dns
```

本次实际结果：

- sleep：ASan `heap-use-after-free`，从 SleepAwaitable 定时回调进入已释放 frame 的 resume。
- tls：`TLS default verify mode: 0 (SSL_VERIFY_NONE=0)`。
- dns：只输出 `cache hit callback: entering clearCache`，timeout 进程退出码为 124。

源码、修复建议和证据边界见[审计记录](audit_2026-09-12.md)。
