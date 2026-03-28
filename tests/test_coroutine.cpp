#include "mini/coroutine/Task.h"

#include <cassert>

namespace {

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

}  // namespace

int main() {
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
    return 0;
}
