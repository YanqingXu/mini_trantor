#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"

#include <cassert>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

using namespace std::chrono_literals;

int main() {
    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        assert(loop != nullptr);

        std::promise<std::thread::id> executedOn;
        auto executedFuture = executedOn.get_future();
        const auto callerThread = std::this_thread::get_id();

        loop->queueInLoop([&] {
            assert(loop->isInLoopThread());
            executedOn.set_value(std::this_thread::get_id());
            loop->quit();
        });

        const auto status = executedFuture.wait_for(1s);
        assert(status == std::future_status::ready);
        assert(executedFuture.get() != callerThread);
    }

    {
        auto loopThread = std::make_unique<mini::net::EventLoopThread>();
        mini::net::EventLoop* loop = loopThread->startLoop();

        std::promise<void> taskRan;
        auto taskRanFuture = taskRan.get_future();
        std::promise<void> destroyed;
        auto destroyedFuture = destroyed.get_future();

        loop->queueInLoop([&] {
            assert(loop->isInLoopThread());
            taskRan.set_value();
            loop->quit();
        });

        const auto taskStatus = taskRanFuture.wait_for(1s);
        assert(taskStatus == std::future_status::ready);

        std::thread destroyer([&] {
            loopThread.reset();
            destroyed.set_value();
        });

        const auto destroyStatus = destroyedFuture.wait_for(1s);
        assert(destroyStatus == std::future_status::ready);
        destroyer.join();
    }

    return 0;
}
