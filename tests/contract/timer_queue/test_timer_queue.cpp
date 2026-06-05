#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"

#include <cassert>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace std::chrono_literals;

int main() {
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();
        const auto callerThread = std::this_thread::get_id();

        std::promise<std::thread::id> firedOn;
        auto firedFuture = firedOn.get_future();

        loop->runAfter(20ms, [loop, &firedOn] {
            firedOn.set_value(std::this_thread::get_id());
            loop->quit();
        });

        assert(firedFuture.wait_for(1s) == std::future_status::ready);
        assert(firedFuture.get() != callerThread);
    }

    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::atomic<bool> fired{false};
        auto timerId = loop->runAfter(50ms, [&] { fired = true; });
        loop->runAfter(80ms, [loop] { loop->quit(); });

        std::thread canceller([loop, timerId] {
            std::this_thread::sleep_for(10ms);
            loop->cancel(timerId);
        });

        std::this_thread::sleep_for(120ms);
        canceller.join();
        assert(!fired.load());
    }

    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::promise<int> firedCount;
        auto firedCountFuture = firedCount.get_future();
        int count = 0;
        mini::net::TimerId repeating;

        repeating = loop->runEvery(10ms, [loop, &count, &firedCount, &repeating] {
            ++count;
            if (count == 3) {
                loop->cancel(repeating);
            }
            if (count == 3) {
                loop->runAfter(30ms, [loop, &firedCount, &count] {
                    firedCount.set_value(count);
                    loop->quit();
                });
            }
        });

        assert(firedCountFuture.wait_for(1s) == std::future_status::ready);
        assert(firedCountFuture.get() == 3);
    }

    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();
        const auto callerThread = std::this_thread::get_id();

        std::promise<std::thread::id> firedOn;
        auto firedFuture = firedOn.get_future();
        const auto when = mini::base::now() + 20ms;

        std::thread scheduler([loop, when, &firedOn] {
            loop->runAt(when, [loop, &firedOn] {
                firedOn.set_value(std::this_thread::get_id());
                loop->quit();
            });
        });

        assert(firedFuture.wait_for(1s) == std::future_status::ready);
        assert(firedFuture.get() != callerThread);
        scheduler.join();
    }

    return 0;
}
