#include "mini/game/logic/GameCommandQueue.h"

#include <cassert>
#include <chrono>
#include <thread>

namespace {

void testEnqueueAndDrainOrder() {
    using namespace std::chrono_literals;

    mini::game::logic::GameCommandQueue queue;
    auto c1 = std::make_shared<mini::game::logic::GameCommand>(
        "s1", std::weak_ptr<mini::net::TcpConnection>{}, "first");
    auto c2 = std::make_shared<mini::game::logic::GameCommand>(
        "s2", std::weak_ptr<mini::net::TcpConnection>{}, "second");
    auto c3 = std::make_shared<mini::game::logic::GameCommand>(
        "s3", std::weak_ptr<mini::net::TcpConnection>{}, "third");

    queue.enqueue(c1);
    queue.enqueue(c2);
    queue.enqueue(c3);
    assert(queue.size() == 3);

    const auto drained = queue.drain(2);
    assert(drained.size() == 2);
    assert(drained[0]->payload == "first");
    assert(drained[1]->payload == "second");
    assert(queue.size() == 1);

    const auto drainedMore = queue.drain(10);
    assert(drainedMore.size() == 1);
    assert(drainedMore[0]->payload == "third");
    assert(queue.size() == 0);
}

void testDrainCapacityAndEmptyBehavior() {
    mini::game::logic::GameCommandQueue queue;
    for (int i = 0; i < 4; ++i) {
        queue.enqueue(mini::game::logic::GameCommand{"s", {}, std::to_string(i)});
    }

    const auto drained = queue.drain(0);
    assert(drained.empty());
    assert(queue.size() == 4);

    const auto drainedTwo = queue.drain(2);
    assert(drainedTwo.size() == 2);
    assert(queue.size() == 2);
}

void testOldestLagForQueuedCommand() {
    mini::game::logic::GameCommandQueue queue;
    auto command = std::make_shared<mini::game::logic::GameCommand>(
        "s", std::weak_ptr<mini::net::TcpConnection>{}, "lag");
    command->enqueuedAt = mini::base::Timestamp{}; // ensure fallback to constructor timestamp.

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    queue.enqueue(command);

    const auto firstLag = queue.oldestLag();
    assert(firstLag >= std::chrono::milliseconds::zero());
    assert(queue.size() == 1);

    const auto drained = queue.drain(1);
    assert(!drained.empty());
    assert(queue.size() == 0);
    assert(queue.oldestLag() == std::chrono::milliseconds::zero());
}

void testClearQueue() {
    mini::game::logic::GameCommandQueue queue;
    queue.enqueue(mini::game::logic::GameCommand{"s1", {}, "a"});
    queue.enqueue(mini::game::logic::GameCommand{"s2", {}, "b"});
    assert(queue.size() == 2);
    queue.clear();
    assert(queue.size() == 0);
    assert(queue.drain(3).empty());
}

}  // namespace

int main() {
    testEnqueueAndDrainOrder();
    testDrainCapacityAndEmptyBehavior();
    testOldestLagForQueuedCommand();
    testClearQueue();
    return 0;
}

