// Intent: when_all / when_any / coroutine_task; detached children borrow no dead parent.
#include "mini/coroutine/SleepAwaitable.h"
#include "mini/coroutine/Task.h"
#include "mini/coroutine/WhenAll.h"
#include "mini/coroutine/WhenAny.h"
#include "mini/net/EventLoop.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <latch>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace mini::coroutine;
using mini::net::EventLoop;

namespace {
Task<int> child(EventLoop* loop, std::atomic<int>* destroyed, std::latch* finished = nullptr) {
    struct Probe {
        std::atomic<int>* count;
        std::latch* done;
        ~Probe() { ++*count; if (done) { done->count_down(); } }
    } probe{destroyed, finished};
    co_await asyncSleep(loop, 1ms);
    co_return 42;
}

Task<int> immediate() { co_return 7; }

struct ThrowingValue {
    int generation{0};
    ThrowingValue() = default;
    ThrowingValue(const ThrowingValue&) = delete;
    ThrowingValue(ThrowingValue&& other) : generation(other.generation + 1) {
        if (generation == 3) { throw std::runtime_error("winner storage failed"); }
    }
};
Task<ThrowingValue> throwingValue() { co_return ThrowingValue{}; }

void winningValueFailureMustCompleteParent() {
    auto parent = whenAny(throwingValue());
    parent.start();
    assert(parent.done());
    bool caught = false;
    try { (void)parent.result(); } catch (const std::runtime_error&) { caught = true; }
    assert(caught);
}
Task<void> voidChild(EventLoop* loop, std::atomic<int>* destroyed) {
    (void)co_await child(loop, destroyed);
}
Task<void> immediateVoid() { co_return; }

void destroySuspendedVoidParent(bool any) {
    EventLoop loop;
    std::atomic<int> destroyed{0};
    if (any) {
        auto parent = whenAny(voidChild(&loop, &destroyed), voidChild(&loop, &destroyed));
        parent.start();
    } else {
        auto parent = whenAll(voidChild(&loop, &destroyed), voidChild(&loop, &destroyed));
        parent.start();
    }
    loop.runAfter(5ms, [&] { loop.quit(); });
    loop.loop();
    assert(destroyed == 2);
}

void destroySuspendedParent(bool any) {
    EventLoop loop;
    std::atomic<int> destroyed{0};
    if (any) {
        auto parent = whenAny(child(&loop, &destroyed), child(&loop, &destroyed));
        parent.start();
    } else {
        auto parent = whenAll(child(&loop, &destroyed), child(&loop, &destroyed));
        parent.start();
    }
    loop.runAfter(5ms, [&] { loop.quit(); });
    loop.loop();
    assert(destroyed == 2);
}

Task<void> immediateParent(EventLoop* loop, std::atomic<int>* destroyed, int* resumed) {
    auto result = co_await whenAny(immediate(), child(loop, destroyed));
    assert(result.index == 0 && result.value == 7);
    ++*resumed;
}

Task<void> immediateVoidParent(EventLoop* loop, std::atomic<int>* destroyed, int* resumed) {
    auto result = co_await whenAny(immediateVoid(), voidChild(loop, destroyed));
    assert(result.index == 0);
    co_await whenAll(immediateVoid(), immediateVoid());
    auto values = co_await whenAll(immediate(), immediate());
    assert(std::get<0>(values) == 7 && std::get<1>(values) == 7);
    ++*resumed;
}

void synchronousWinnerDoesNotDestroyLaunchState() {
    EventLoop loop;
    std::atomic<int> destroyed{0};
    int resumed = 0;
    immediateParent(&loop, &destroyed, &resumed).detach();
    immediateVoidParent(&loop, &destroyed, &resumed).detach();
    loop.runAfter(5ms, [&] { loop.quit(); });
    loop.loop();
    assert(resumed == 2 && destroyed == 2);
}

void otherLoopCompletionRacesParentDestruction(bool any) {
    std::promise<EventLoop*> ready;
    auto readyFuture = ready.get_future();
    std::latch releaseLoop(1);
    std::thread worker([&] {
        EventLoop loop;
        ready.set_value(&loop);
        loop.loop();
        releaseLoop.wait();
    });
    auto* loop = readyFuture.get();
    constexpr int attempts = 200;
    std::latch finished(attempts * 2);
    std::atomic<int> destroyed{0};
    for (int i = 0; i < attempts; ++i) {
        if (any) {
            auto parent = whenAny(child(loop, &destroyed, &finished), child(loop, &destroyed, &finished));
            parent.start();
        } else {
            auto parent = whenAll(child(loop, &destroyed, &finished), child(loop, &destroyed, &finished));
            parent.start();
        }
    }
    finished.wait();
    loop->quit();
    releaseLoop.count_down();
    worker.join();
    assert(destroyed == attempts * 2);
}

Task<void> nestedParent(EventLoop* loop, std::atomic<int>* destroyed,
                        std::latch* finished, bool any) {
    if (any) {
        (void)co_await whenAny(child(loop, destroyed, finished), child(loop, destroyed, finished));
    } else {
        (void)co_await whenAll(child(loop, destroyed, finished), child(loop, destroyed, finished));
    }
}
Task<void> adoptStarted(Task<void> nested) { co_await std::move(nested); }

void adoptedStartedChainSurvivesConcurrentCompletion() {
    std::promise<EventLoop*> ready;
    auto readyFuture = ready.get_future();
    std::latch beginCompletion(1), releaseLoop(1);
    std::thread worker([&] {
        EventLoop loop;
        ready.set_value(&loop);
        beginCompletion.wait();
        loop.loop();
        releaseLoop.wait();
    });
    auto* loop = readyFuture.get();
    constexpr int attempts = 200;
    std::latch finished(attempts * 2);
    std::atomic<int> destroyed{0};
    std::vector<Task<void>> parents;
    for (int i = 0; i < attempts; ++i) {
        auto nested = nestedParent(loop, &destroyed, &finished, i % 2 != 0);
        nested.start();
        parents.push_back(adoptStarted(std::move(nested)));
        parents.back().start();
    }
    // Ownership/adoption is finished before any child can resume. Only completion
    // and release race; the same Task object is never concurrently accessed.
    beginCompletion.count_down();
    parents.clear();
    finished.wait();
    loop->quit();
    releaseLoop.count_down();
    worker.join();
    assert(destroyed == attempts * 2);
}
} // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "all-cases";
    if (mode == "failure" || mode == "all-cases") { winningValueFailureMustCompleteParent(); }
    if (mode == "all" || mode == "all-cases") {
        destroySuspendedParent(false);
        destroySuspendedVoidParent(false);
    }
    if (mode == "any" || mode == "all-cases") {
        destroySuspendedParent(true);
        destroySuspendedVoidParent(true);
    }
    if (mode == "immediate" || mode == "all-cases") { synchronousWinnerDoesNotDestroyLaunchState(); }
    if (mode == "cross" || mode == "all-cases") {
        otherLoopCompletionRacesParentDestruction(false);
        otherLoopCompletionRacesParentDestruction(true);
        adoptedStartedChainSurvivesConcurrentCompletion();
    }
}
