// Intent: loop_handle / event_loop. A producer may outlive its posting target.
#include "mini/net/EventLoop.h"
#include "mini/net/LoopHandle.h"

#include <atomic>
#include <barrier>
#include <cassert>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using mini::net::EventLoop;
using mini::net::LoopHandle;

namespace {
void defaultAndExpiredHandlesReject() {
    LoopHandle handle;
    assert(!handle.queue([] { assert(false); }));
    {
        EventLoop loop;
        handle = loop.handle();
    }
    assert(!handle.queue([] { assert(false); }));
    bool rejectedEmpty = false;
    try { (void)handle.queue({}); } catch (const std::invalid_argument&) { rejectedEmpty = true; }
    assert(rejectedEmpty);
}

void queuedWorkExecutesOnOwnerAndDrainsNestedPosts() {
    EventLoop loop;
    auto handle = loop.handle();
    int calls = 0;
    std::thread producer([&] {
        assert(handle.queue([&] {
            assert(loop.isInLoopThread());
            ++calls;
            loop.quit();
            assert(handle.queue([&] { ++calls; }));
        }));
    });
    producer.join();
    assert(calls == 0); // no inline execution, even before loop() starts
    loop.loop();
    assert(calls == 2);
    assert(!handle.queue([] { assert(false); }));
}

void producerRacesOwnerDestruction() {
    std::promise<LoopHandle> ready;
    auto readyFuture = ready.get_future();
    std::barrier destroyRace(2);
    std::thread owner([&] {
        EventLoop loop;
        ready.set_value(loop.handle());
        destroyRace.arrive_and_wait();
    });
    auto handle = readyFuture.get();
    std::vector<std::weak_ptr<int>> lifetimes;
    destroyRace.arrive_and_wait();
    for (int i = 0; i < 1000; ++i) {
        auto lifetime = std::make_shared<int>(i);
        lifetimes.emplace_back(lifetime);
        (void)handle.queue([lifetime] { assert(false); });
    }
    owner.join();
    assert(!handle.queue([] { assert(false); }));
    for (auto& observed : lifetimes) { assert(observed.expired()); }
}

void rejectedCallbackDestructionMayReenter() {
    LoopHandle handle;
    { EventLoop loop; handle = loop.handle(); }
    struct Reenter {
        LoopHandle handle;
        int* destroyed;
        Reenter(LoopHandle value, int* count) : handle(std::move(value)), destroyed(count) {}
        ~Reenter() {
            assert(!handle.queue([] { assert(false); }));
            ++*destroyed;
        }
    };
    int destroyed = 0;
    assert(!handle.queue([resource = std::make_shared<Reenter>(handle, &destroyed)] {}));
    assert(destroyed == 1);
}
} // namespace

int main() {
    defaultAndExpiredHandlesReject();
    queuedWorkExecutesOnOwnerAndDrainsNestedPosts();
    producerRacesOwnerDestruction();
    rejectedCallbackDestructionMayReenter();
}
