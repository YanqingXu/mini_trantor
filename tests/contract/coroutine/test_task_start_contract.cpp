// Intent: coroutine_task; start controls initial execution, not arbitrary resume.
#include "mini/coroutine/SleepAwaitable.h"
#include "mini/coroutine/Task.h"
#include "mini/net/EventLoop.h"

#include <cassert>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

using namespace std::chrono_literals;
using mini::coroutine::Task;
using mini::net::EventLoop;

static_assert(!std::is_copy_constructible_v<Task<int>::Awaiter>);
static_assert(std::is_move_constructible_v<Task<int>::Awaiter>);

namespace {
Task<int> delayed(EventLoop* loop, int& resumed, int& destroyed) {
    struct Probe { int& count; ~Probe() { ++count; } } probe{destroyed};
    co_await mini::coroutine::asyncSleep(loop, 1ms);
    ++resumed;
    co_return 42;
}

Task<int> immediate() { co_return 42; }
Task<int> adopt(Task<int> child) { co_return co_await std::move(child); }
Task<void> holdResource(std::shared_ptr<int> resource) { co_return; }

void drain(EventLoop& loop) {
    loop.runAfter(5ms, [&] { loop.quit(); });
    loop.loop();
}

void repeatedStartIsRejected() {
    EventLoop loop;
    int resumed = 0, destroyed = 0;
    auto task = delayed(&loop, resumed, destroyed);
    task.start();
    bool rejected = false;
    try { task.start(); } catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
    assert(!task.done() && resumed == 0);
    drain(loop);
    assert(task.result() == 42 && resumed == 1 && destroyed == 1);
}

void detachStartedTaskDoesNotResumeAgain() {
    EventLoop loop;
    int resumed = 0, destroyed = 0;
    auto task = delayed(&loop, resumed, destroyed);
    task.start();
    task.detach();
    assert(resumed == 0 && destroyed == 0);
    drain(loop);
    assert(resumed == 1 && destroyed == 1);
}

void detachCompletedTaskOnlyReleasesFrame() {
    auto task = immediate();
    task.start();
    task.detach();
    assert(task.done());
}

void awaitStartedTaskDoesNotResumeAgain() {
    EventLoop loop;
    int resumed = 0, destroyed = 0;
    auto child = delayed(&loop, resumed, destroyed);
    child.start();
    auto parent = adopt(std::move(child));
    parent.start();
    assert(!parent.done() && resumed == 0);
    drain(loop);
    assert(parent.result() == 42 && resumed == 1 && destroyed == 1);
}

void emptyTaskAwaitReportsError() {
    auto parent = adopt(Task<int>{});
    parent.start();
    assert(parent.done());
    bool rejected = false;
    try { (void)parent.result(); } catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
}

void movedAwaiterOwnsTheFrameOnce() {
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> observed = resource;
    {
        auto task = holdResource(resource);
        auto first = std::move(task).operator co_await();
        auto second = std::move(first);
        resource.reset();
        assert(!observed.expired());
    }
    assert(observed.expired());
}
} // namespace

int main() {
    repeatedStartIsRejected();
    detachStartedTaskDoesNotResumeAgain();
    detachCompletedTaskOnlyReleasesFrame();
    awaitStartedTaskDoesNotResumeAgain();
    emptyTaskAwaitReportsError();
    movedAwaiterOwnsTheFrameOnce();
}
