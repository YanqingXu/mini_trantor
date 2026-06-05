#include "mini/coroutine/Task.h"

#include <cassert>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {

struct LifetimeProbe {
    explicit LifetimeProbe(int* destructions) : destructions_(destructions) {
    }

    LifetimeProbe(const LifetimeProbe&) = delete;
    LifetimeProbe& operator=(const LifetimeProbe&) = delete;
    LifetimeProbe(LifetimeProbe&&) = delete;
    LifetimeProbe& operator=(LifetimeProbe&&) = delete;

    ~LifetimeProbe() {
        ++(*destructions_);
    }

private:
    int* destructions_;
};

mini::coroutine::Task<void> lazyTask(bool* ran) {
    *ran = true;
    co_return;
}

mini::coroutine::Task<int> immediate() {
    co_return 21;
}

mini::coroutine::Task<int> chained() {
    const int value = co_await immediate();
    co_return value * 2;
}

mini::coroutine::Task<void> voidChain(bool* reached) {
    const int value = co_await immediate();
    assert(value == 21);
    *reached = true;
    co_return;
}

mini::coroutine::Task<std::unique_ptr<int>> moveOnly() {
    co_return std::make_unique<int>(7);
}

mini::coroutine::Task<int> throwing() {
    throw std::runtime_error("boom");
    co_return 0;
}

mini::coroutine::Task<void> detachedTask(bool* ran, int* destructions) {
    LifetimeProbe probe(destructions);
    *ran = true;
    co_return;
}

}  // namespace

int main() {
    bool lazyRan = false;
    auto lazy = lazyTask(&lazyRan);
    assert(!lazyRan);
    lazy.start();
    assert(lazy.done());
    lazy.result();
    assert(lazyRan);

    auto task = chained();
    task.start();
    assert(task.done());
    assert(task.result() == 42);

    bool reached = false;
    auto voidTask = voidChain(&reached);
    voidTask.start();
    assert(voidTask.done());
    voidTask.result();
    assert(reached);

    auto moveOnlyTask = moveOnly();
    auto movedTask = std::move(moveOnlyTask);
    movedTask.start();
    assert(movedTask.done());
    auto value = std::move(movedTask).result();
    assert(value);
    assert(*value == 7);

    auto failing = throwing();
    failing.start();
    assert(failing.done());
    bool threw = false;
    try {
        (void)failing.result();
    } catch (const std::runtime_error& error) {
        threw = true;
        assert(std::string_view(error.what()) == "boom");
    }
    assert(threw);

    bool detachedRan = false;
    int destructions = 0;
    auto detached = detachedTask(&detachedRan, &destructions);
    assert(!detachedRan);
    assert(destructions == 0);
    detached.detach();
    assert(detachedRan);
    assert(destructions == 1);

    return 0;
}
