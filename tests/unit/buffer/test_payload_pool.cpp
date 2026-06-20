// v6-alpha Task-06 — PayloadPool 回归测试
// 覆盖：对象池复用、引用计数回收、跨线程释放的 marshal 路径。

#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/buffer/PayloadPool.h"

#include <chrono>
#include <cassert>
#include <functional>
#include <future>
#include <string_view>
#include <string>
#include <thread>

namespace {

bool waitForPoolState(mini::net::EventLoop* loop,
                      const std::shared_ptr<mini::net::buffer::PayloadPool>& pool,
                      std::function<bool(std::size_t, std::size_t)> predicate,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto statePromise = std::make_shared<std::promise<std::pair<std::size_t, std::size_t>>>();
        auto stateFuture = statePromise->get_future();
        loop->queueInLoop([pool, promise = statePromise]() {
            promise->set_value({pool->inUseCount(), pool->cachedCount()});
        });

        auto [inUseCount, cachedCount] = stateFuture.get();
        if (predicate(inUseCount, cachedCount)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

}  // namespace

int main() {
    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();
    auto pool = std::make_shared<mini::net::buffer::PayloadPool>(loop);

    // 1) pool 应该能在相同对象回收后复用 payload，避免重复分配。
    auto firstPayload = std::make_shared<std::promise<mini::net::buffer::Payload*>>();
    auto firstFuture = firstPayload->get_future();
    loop->queueInLoop([pool = pool, promise = firstPayload]() {
        auto payload = pool->acquire(std::string_view{"task-06-reuse"});
        promise->set_value(payload.get());
    });
    const auto* first = firstFuture.get();
    assert(first != nullptr);
    assert(waitForPoolState(loop, pool,
                           [](std::size_t inUseCount, std::size_t cachedCount) {
                               return inUseCount == 0 && cachedCount >= 1;
                           }));

    auto secondPayload = std::make_shared<std::promise<mini::net::buffer::Payload*>>();
    auto secondFuture = secondPayload->get_future();
    loop->queueInLoop([pool = pool, promise = secondPayload]() {
        auto payload = pool->acquire(std::string_view{"task-06-reuse-next"});
        promise->set_value(payload.get());
    });
    const auto* second = secondFuture.get();
    assert(second != nullptr);
    assert(first == second);
    assert(waitForPoolState(loop, pool,
                           [](std::size_t inUseCount, std::size_t cachedCount) {
                               return inUseCount == 0 && cachedCount >= 1;
                           }));

    // 2) 跨线程释放（非 ioLoop 线程）必须通过 queueInLoop 回流回 pool 归还链路。
    auto crossPayload = std::make_shared<std::promise<mini::net::buffer::PayloadPtr>>();
    auto crossPayloadFuture = crossPayload->get_future();
    loop->queueInLoop([pool = pool, promise = crossPayload]() {
        promise->set_value(pool->acquire(std::string{"task-06-cross-thread"}));
    });
    auto payload = crossPayloadFuture.get();
    assert(payload != nullptr);
    assert(payload->view() == "task-06-cross-thread");

    payload.reset();
    assert(waitForPoolState(loop, pool,
                           [](std::size_t inUseCount, std::size_t cachedCount) {
                               return inUseCount == 0 && cachedCount >= 1;
                           },
                           std::chrono::milliseconds(1000)));

    return 0;
}
