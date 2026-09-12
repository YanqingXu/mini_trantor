// Intent: coroutine_task / async_timer; destruction unregisters, cancellation resumes.
#include "mini/coroutine/SleepAwaitable.h"
#include "mini/coroutine/Task.h"
#include "mini/net/EventLoop.h"

#include <cassert>
#include <chrono>
#include <memory>

using namespace std::chrono_literals;
using mini::coroutine::CancellationSource;
using mini::coroutine::CancellationToken;
using mini::coroutine::Task;
using mini::net::EventLoop;

namespace {
struct FrameProbe {
    int& destroyed;
    ~FrameProbe() { ++destroyed; }
};

Task<void> sleeping(EventLoop* loop, int& resumed, int& destroyed,
                    CancellationToken token = {}) {
    FrameProbe probe{destroyed};
    co_await mini::coroutine::asyncSleep(loop, 1ms, std::move(token));
    ++resumed;
}

Task<void> parent(EventLoop* loop, int& resumed, int& destroyed) {
    FrameProbe probe{destroyed};
    co_await sleeping(loop, resumed, destroyed);
    ++resumed;
}

void runPastDeadline(EventLoop& loop) {
    loop.runAfter(5ms, [&] { loop.quit(); });
    loop.loop();
}

void destroyedPendingSleep() {
    EventLoop loop;
    int resumed = 0, destroyed = 0;
    { auto task = sleeping(&loop, resumed, destroyed); task.start(); }
    assert(destroyed == 1);
    runPastDeadline(loop);
    assert(resumed == 0);
    assert(destroyed == 1);
}

void destroyedAfterCancellationWasQueued() {
    EventLoop loop;
    int resumed = 0, destroyed = 0;
    CancellationSource source;
    {
        auto task = sleeping(&loop, resumed, destroyed, source.token());
        task.start();
        source.cancel();
    }
    runPastDeadline(loop);
    assert(resumed == 0);
    assert(destroyed == 1);
}

void moveAssignmentUnregistersOldFrame() {
    EventLoop loop;
    int resumed = 0, destroyed = 0;
    auto task = sleeping(&loop, resumed, destroyed);
    task.start();
    task = Task<void>{};
    runPastDeadline(loop);
    assert(resumed == 0);
    assert(destroyed == 1);
}

void parentDestructionUnregistersChild() {
    EventLoop loop;
    int resumed = 0, destroyed = 0;
    { auto task = parent(&loop, resumed, destroyed); task.start(); }
    runPastDeadline(loop);
    assert(resumed == 0);
    assert(destroyed == 2);
}

void detachedFrameCompletesOnce() {
    EventLoop loop;
    int resumed = 0, destroyed = 0;
    CancellationSource source;
    sleeping(&loop, resumed, destroyed, source.token()).detach();
    runPastDeadline(loop);
    source.cancel(); // registration was removed before frame completion
    assert(resumed == 1);
    assert(destroyed == 1);
}
} // namespace

int main() {
    destroyedPendingSleep();
    destroyedAfterCancellationWasQueued();
    moveAssignmentUnregistersOldFrame();
    parentDestructionUnregistersChild();
    detachedFrameCompletesOnce();
}
