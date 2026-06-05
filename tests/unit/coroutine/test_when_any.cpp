// Unit tests for WhenAny coroutine combinator.
// Covers: first-completion wins, void tasks, value tasks, single task,
//         exception from winner, move semantics.
//
// These are pure coroutine-layer tests using synchronous (immediately completing)
// sub-tasks. Cross-thread and cancellation scenarios are in contract tests.

#include "mini/coroutine/Task.h"
#include "mini/coroutine/WhenAny.h"

#include <cassert>
#include <stdexcept>
#include <string>
#include <string_view>

using mini::coroutine::Task;
using mini::coroutine::whenAny;
using mini::coroutine::WhenAnyResult;

namespace {

// --- Helper coroutines ---

Task<int> intTask(int value) {
    co_return value;
}

Task<void> voidTask(bool* ran) {
    *ran = true;
    co_return;
}

Task<std::string> stringTask(std::string value) {
    co_return value;
}

Task<int> throwingIntTask(const char* msg) {
    throw std::runtime_error(msg);
    co_return 0;
}

Task<void> throwingVoidTask(const char* msg) {
    throw std::runtime_error(msg);
    co_return;
}

// A task that would never complete synchronously in real usage,
// but for unit tests with synchronous sub-tasks, all complete immediately.
// The "first" one in parameter order wins when all are synchronous.

}  // namespace

int main() {
    // Unit 1: First completion wins — with synchronous tasks, first in order wins
    {
        auto task = whenAny(intTask(10), intTask(20), intTask(30));
        task.start();
        assert(task.done());
        auto result = std::move(task).result();
        assert(result.index == 0);
        assert(result.value == 10);
    }

    // Unit 2: WhenAny with all void tasks — returns winning index
    {
        bool ran1 = false, ran2 = false;
        auto task = whenAny(voidTask(&ran1), voidTask(&ran2));
        task.start();
        assert(task.done());
        auto result = std::move(task).result();
        assert(result.index == 0);
        assert(ran1);
        // ran2 may or may not have run (cancellation is best-effort for sync tasks)
    }

    // Unit 3: WhenAny with value tasks — index + value correct
    {
        auto task = whenAny(stringTask("alpha"), stringTask("beta"));
        task.start();
        assert(task.done());
        auto result = std::move(task).result();
        assert(result.index == 0);
        assert(result.value == "alpha");
    }

    // Unit 4: Single task — degenerates correctly
    {
        auto task = whenAny(intTask(42));
        task.start();
        assert(task.done());
        auto result = std::move(task).result();
        assert(result.index == 0);
        assert(result.value == 42);
    }

    // Unit 5: Single void task
    {
        bool ran = false;
        auto task = whenAny(voidTask(&ran));
        task.start();
        assert(task.done());
        auto result = std::move(task).result();
        assert(result.index == 0);
        assert(ran);
    }

    // Unit 6: Exception from winner is propagated to parent
    {
        auto task = whenAny(throwingIntTask("boom"), intTask(99));
        task.start();
        assert(task.done());
        bool threw = false;
        try {
            (void)std::move(task).result();
        } catch (const std::runtime_error& e) {
            threw = true;
            assert(std::string_view(e.what()) == "boom");
        }
        assert(threw);
    }

    // Unit 7: Exception from void winner
    {
        bool ran = false;
        auto task = whenAny(throwingVoidTask("void_boom"), voidTask(&ran));
        task.start();
        assert(task.done());
        bool threw = false;
        try {
            (void)std::move(task).result();
        } catch (const std::runtime_error& e) {
            threw = true;
            assert(std::string_view(e.what()) == "void_boom");
        }
        assert(threw);
    }

    // Unit 8: Move semantics — input tasks are consumed
    {
        auto t1 = intTask(1);
        auto t2 = intTask(2);
        auto combined = whenAny(std::move(t1), std::move(t2));
        assert(t1.done());  // moved-from task appears done (empty)
        assert(t2.done());
        combined.start();
        assert(combined.done());
        auto result = std::move(combined).result();
        assert(result.index == 0);
        assert(result.value == 1);
    }

    return 0;
}
