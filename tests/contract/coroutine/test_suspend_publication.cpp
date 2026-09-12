// Intent: async_timer; publish all registration state before a callback can resume.
#include "mini/coroutine/SleepAwaitable.h"
#include "mini/coroutine/Task.h"
#include "mini/net/EventLoop.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <latch>
#include <thread>

using namespace std::chrono_literals;
using mini::coroutine::CancellationSource;
using mini::coroutine::CancellationToken;
using mini::coroutine::Task;
using mini::net::EventLoop;
using mini::net::Expected;
using mini::net::NetError;

namespace {
Task<void> observe(EventLoop* loop, CancellationToken token,
                   std::promise<Expected<void>>* result, std::atomic<int>* destroyed) {
    struct Probe {
        std::atomic<int>* count;
        ~Probe() { ++*count; }
    } probe{destroyed};
    auto outcome = co_await mini::coroutine::asyncSleep(loop, 0ms, std::move(token));
    assert(loop->isInLoopThread());
    result->set_value(outcome);
}

void cancellationBeforePublicationWins() {
    EventLoop loop;
    CancellationSource source;
    source.cancel();
    std::atomic<int> destroyed{0};
    std::promise<Expected<void>> result;
    auto future = result.get_future();
    auto task = observe(&loop, source.token(), &result, &destroyed);
    task.start();
    loop.runAfter(5ms, [&] { loop.quit(); });
    loop.loop();
    assert(future.wait_for(0ms) == std::future_status::ready);
    auto outcome = future.get();
    assert(!outcome && outcome.error() == NetError::Cancelled);
    assert(task.done());
    assert(destroyed == 1);
}

void offThreadStartAndCancellation() {
    std::promise<EventLoop*> ready;
    auto readyFuture = ready.get_future();
    // Keep the loop alive until the last external quit/wakeup call has returned.
    // EventLoopThread's own shutdown protocol has a separate S1 regression.
    std::latch releaseLoop(1);
    std::atomic<int> destroyed{0};
    std::thread worker([&] {
        EventLoop loop;
        ready.set_value(&loop);
        loop.loop();
        releaseLoop.wait();
    });
    auto* loop = readyFuture.get();
    constexpr int attempts = 200;
    for (int i = 0; i < attempts; ++i) {
        CancellationSource source;
        const bool preCancelled = i % 2 == 0;
        if (preCancelled) { source.cancel(); }
        std::promise<Expected<void>> result;
        auto future = result.get_future();
        observe(loop, source.token(), &result, &destroyed).detach();
        source.cancel();
        assert(future.wait_for(2s) == std::future_status::ready);
        auto outcome = future.get();
        if (preCancelled) {
            assert(!outcome && outcome.error() == NetError::Cancelled);
        } else {
            assert(outcome || outcome.error() == NetError::Cancelled);
        }
    }
    loop->quit();
    releaseLoop.count_down();
    worker.join();
    assert(destroyed == attempts);
}
} // namespace

int main() {
    cancellationBeforePublicationWins();
    offThreadStartAndCancellation();
}
