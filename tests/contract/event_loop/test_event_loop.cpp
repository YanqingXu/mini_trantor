#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"

#include <cassert>
#include <chrono>
#include <future>
#include <thread>

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

    return 0;
}
