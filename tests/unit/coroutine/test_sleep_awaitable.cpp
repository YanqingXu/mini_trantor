// Unit tests for SleepAwaitable local invariants.
// Tests await_ready, construction, and basic state management.

#include "mini/coroutine/SleepAwaitable.h"
#include "mini/net/EventLoop.h"

#include <cassert>
#include <chrono>

using namespace std::chrono_literals;

int main() {
    // Unit 1: await_ready always returns false
    {
        mini::net::EventLoop loop;
        auto awaitable = mini::coroutine::asyncSleep(&loop, 100ms);
        assert(!awaitable.await_ready());
    }

    // Unit 2: await_ready returns false even for zero duration
    {
        mini::net::EventLoop loop;
        auto awaitable = mini::coroutine::asyncSleep(&loop, 0ms);
        assert(!awaitable.await_ready());
    }

    // Unit 3: construction captures loop pointer correctly
    {
        mini::net::EventLoop loop;
        auto awaitable = mini::coroutine::asyncSleep(&loop, 50ms);
        assert(awaitable.state()->loop == &loop);
    }

    // Unit 4: initial state is not resumed and not cancelled
    {
        mini::net::EventLoop loop;
        auto awaitable = mini::coroutine::asyncSleep(&loop, 50ms);
        assert(!awaitable.state()->resumed);
        assert(!awaitable.state()->cancelled);
    }

    return 0;
}
