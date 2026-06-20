#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"

#include <cassert>
#include <atomic>
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

    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::promise<void> taskStarted;
        auto taskStartedFuture = taskStarted.get_future();
        std::atomic<int> taskFinished{0};

        loop->queueInLoop([&] {
            taskStarted.set_value();
            std::this_thread::sleep_for(30ms);
            ++taskFinished;
        });

        assert(taskStartedFuture.wait_for(1s) == std::future_status::ready);
        loopThread.stop();
        assert(taskFinished.load() == 1);

        mini::net::EventLoop* restartedLoop = loopThread.startLoop();
        assert(restartedLoop != nullptr);

        std::promise<void> restartedTask;
        auto restartedTaskFuture = restartedTask.get_future();
        restartedLoop->queueInLoop([&] {
            assert(restartedLoop->isInLoopThread());
            restartedTask.set_value();
        });

        assert(restartedTaskFuture.wait_for(1s) == std::future_status::ready);
        loopThread.stop();
    }

    return 0;
}
