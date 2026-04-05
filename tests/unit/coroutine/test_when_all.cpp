// Unit tests for WhenAll coroutine combinator.
// Covers: void tasks, value tasks, single task, empty case,
//         exception propagation, multiple exceptions, move semantics.
//
// These are pure coroutine-layer tests — no EventLoop needed for basic cases.

#include "mini/coroutine/Task.h"
#include "mini/coroutine/WhenAll.h"

#include <cassert>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

using mini::coroutine::Task;
using mini::coroutine::whenAll;

namespace {

// --- Helper coroutines ---

Task<void> voidTask(bool* ran) {
    *ran = true;
    co_return;
}

Task<int> intTask(int value) {
    co_return value;
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

// Wrapper that runs a Task<T> synchronously via start() and returns result.
template <typename T>
T runSync(Task<T> task) {
    task.start();
    assert(task.done());
    if constexpr (std::is_void_v<T>) {
        task.result();
    } else {
        return std::move(task).result();
    }
}

}  // namespace

int main() {
    // Unit 1: WhenAll with multiple void tasks — all run to completion
    {
        bool ran1 = false, ran2 = false, ran3 = false;
        auto task = whenAll(voidTask(&ran1), voidTask(&ran2), voidTask(&ran3));
        task.start();
        assert(task.done());
        task.result();  // should not throw
        assert(ran1);
        assert(ran2);
        assert(ran3);
    }

    // Unit 2: WhenAll with value tasks — results collected in order
    {
        auto task = whenAll(intTask(10), intTask(20), intTask(30));
        task.start();
        assert(task.done());
        auto [a, b, c] = std::move(task).result();
        assert(a == 10);
        assert(b == 20);
        assert(c == 30);
    }

    // Unit 3: WhenAll with mixed value types (int, string)
    {
        auto task = whenAll(intTask(42), stringTask("hello"));
        task.start();
        assert(task.done());
        auto [num, str] = std::move(task).result();
        assert(num == 42);
        assert(str == "hello");
    }

    // Unit 4: WhenAll with single task — degenerates correctly
    {
        auto task = whenAll(intTask(99));
        task.start();
        assert(task.done());
        auto [val] = std::move(task).result();
        assert(val == 99);
    }

    // Unit 5: WhenAll with single void task
    {
        bool ran = false;
        auto task = whenAll(voidTask(&ran));
        task.start();
        assert(task.done());
        task.result();
        assert(ran);
    }

    // Unit 6: WhenAll with zero tasks — completes immediately (void)
    {
        auto task = whenAll();
        task.start();
        assert(task.done());
        task.result();  // Task<void>, should not throw
    }

    // Unit 7: Exception propagation — one sub-task throws
    {
        auto task = whenAll(intTask(1), throwingIntTask("boom"), intTask(3));
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

    // Unit 8: Exception propagation with void tasks
    {
        bool ran = false;
        auto task = whenAll(voidTask(&ran), throwingVoidTask("void_boom"));
        task.start();
        assert(task.done());
        bool threw = false;
        try {
            task.result();
        } catch (const std::runtime_error& e) {
            threw = true;
            assert(std::string_view(e.what()) == "void_boom");
        }
        assert(threw);
        assert(ran);  // first task should still have run
    }

    // Unit 9: Multiple exceptions — first (in completion order) is propagated
    {
        auto task = whenAll(throwingIntTask("first"), throwingIntTask("second"));
        task.start();
        assert(task.done());
        bool threw = false;
        try {
            (void)std::move(task).result();
        } catch (const std::runtime_error& e) {
            threw = true;
            // We expect "first" since synchronous tasks complete in order
            assert(std::string_view(e.what()) == "first");
        }
        assert(threw);
    }

    // Unit 10: Move semantics — input tasks are consumed
    {
        auto t1 = intTask(1);
        auto t2 = intTask(2);
        auto combined = whenAll(std::move(t1), std::move(t2));
        assert(t1.done());  // moved-from task appears done (empty)
        assert(t2.done());
        combined.start();
        assert(combined.done());
        auto [a, b] = std::move(combined).result();
        assert(a == 1);
        assert(b == 2);
    }

    return 0;
}
