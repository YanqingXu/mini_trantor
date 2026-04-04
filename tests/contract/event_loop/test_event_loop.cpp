#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"

#include <cassert>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

int main() {
    {
        mini::net::EventLoop loop;
        bool ran = false;
        loop.runInLoop([&] {
            ran = true;
            assert(loop.isInLoopThread());
        });
        assert(ran);
    }

    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::promise<std::thread::id> executedOn;
        auto future = executedOn.get_future();
        const auto callerThread = std::this_thread::get_id();

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        loop->queueInLoop([&] {
            executedOn.set_value(std::this_thread::get_id());
            loop->quit();
        });

        const auto status = future.wait_for(std::chrono::seconds(2));
        assert(status == std::future_status::ready);
        assert(future.get() != callerThread);
    }

    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::promise<void> completed;
        auto future = completed.get_future();
        std::vector<int> order;

        loop->queueInLoop([&] {
            order.push_back(1);
            loop->queueInLoop([&] {
                order.push_back(3);
                assert(loop->isInLoopThread());
                completed.set_value();
                loop->quit();
            });
            order.push_back(2);
        });

        const auto status = future.wait_for(1s);
        assert(status == std::future_status::ready);
        assert((order == std::vector<int>{1, 2, 3}));
    }

    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::promise<void> completed;
        auto future = completed.get_future();

        std::this_thread::sleep_for(50ms);
        loop->queueInLoop([&] {
            assert(loop->isInLoopThread());
            completed.set_value();
            loop->quit();
        });

        const auto status = future.wait_for(1s);
        assert(status == std::future_status::ready);
    }

    {
        std::promise<mini::net::EventLoop*> started;
        auto loopFuture = started.get_future();
        std::promise<void> exited;
        auto exitedFuture = exited.get_future();

        std::thread loopOwner([&] {
            mini::net::EventLoop loop;
            started.set_value(&loop);
            loop.loop();
            exited.set_value();
        });

        mini::net::EventLoop* loop = loopFuture.get();
        std::this_thread::sleep_for(50ms);
        loop->quit();

        const auto status = exitedFuture.wait_for(1s);
        assert(status == std::future_status::ready);
        loopOwner.join();
    }

    {
        mini::net::EventLoopThread loopThread;
        mini::net::EventLoop* loop = loopThread.startLoop();

        std::promise<void> nestedRan;
        auto nestedFuture = nestedRan.get_future();

        loop->queueInLoop([&] {
            loop->queueInLoop([&] { nestedRan.set_value(); });
            loop->quit();
        });

        const auto status = nestedFuture.wait_for(1s);
        assert(status == std::future_status::ready);
    }

    return 0;
}
