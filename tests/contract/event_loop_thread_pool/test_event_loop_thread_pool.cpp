#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThreadPool.h"

#include <cassert>
#include <future>
#include <stdexcept>
#include <thread>

int main() {
    {
        mini::net::EventLoop baseLoop;
        mini::net::EventLoopThreadPool pool(&baseLoop, "zero");
        bool callbackRan = false;
        pool.start([&](mini::net::EventLoop* loop) {
            callbackRan = true;
            assert(loop == &baseLoop);
            assert(loop->isInLoopThread());
        });

        assert(pool.getNextLoop() == &baseLoop);
        const auto loops = pool.getAllLoops();
        assert(loops.size() == 1);
        assert(loops.front() == &baseLoop);
        assert(callbackRan);
    }

    {
        mini::net::EventLoop baseLoop;
        mini::net::EventLoopThreadPool pool(&baseLoop, "workers");
        pool.setThreadNum(2);
        pool.start();

        const auto loops = pool.getAllLoops();
        assert(loops.size() == 2);
        assert(loops[0] != nullptr);
        assert(loops[1] != nullptr);
        assert(loops[0] != loops[1]);

        assert(pool.getNextLoop() == loops[0]);
        assert(pool.getNextLoop() == loops[1]);
        assert(pool.getNextLoop() == loops[0]);
    }

    {
        mini::net::EventLoop baseLoop;
        mini::net::EventLoopThreadPool pool(&baseLoop, "wrong-thread-start");

        std::promise<void> observed;
        auto observedFuture = observed.get_future();
        std::thread worker([&] {
            try {
                pool.start();
                assert(false);
            } catch (const std::runtime_error&) {
                observed.set_value();
            }
        });

        const auto status = observedFuture.wait_for(std::chrono::seconds(1));
        assert(status == std::future_status::ready);
        worker.join();
    }

    {
        mini::net::EventLoop baseLoop;
        mini::net::EventLoopThreadPool pool(&baseLoop, "wrong-thread-next");
        pool.setThreadNum(1);
        pool.start();

        std::promise<void> observed;
        auto observedFuture = observed.get_future();
        std::thread worker([&] {
            try {
                (void)pool.getNextLoop();
                assert(false);
            } catch (const std::runtime_error&) {
                observed.set_value();
            }
        });

        const auto status = observedFuture.wait_for(std::chrono::seconds(1));
        assert(status == std::future_status::ready);
        worker.join();
    }

    return 0;
}
