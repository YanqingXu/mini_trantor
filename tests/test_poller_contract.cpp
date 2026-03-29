#include "mini/base/Timestamp.h"
#include "mini/net/Channel.h"
#include "mini/net/EventLoop.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

int main() {
    mini::net::EventLoop loop;
    const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(fd >= 0);

    mini::net::Channel channel(&loop, fd);
    int readCount = 0;
    std::promise<void> fired;
    auto firedFuture = fired.get_future();

    channel.setReadCallback([&](mini::base::Timestamp) {
        ++readCount;
        uint64_t value = 0;
        const ssize_t n = ::read(fd, &value, sizeof(value));
        assert(n == static_cast<ssize_t>(sizeof(value)));

        channel.disableAll();
        channel.remove();
        assert(!loop.hasChannel(&channel));
        fired.set_value();
        loop.quit();
    });

    channel.enableReading();
    assert(loop.hasChannel(&channel));

    std::thread notifier([fd] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const uint64_t one = 1;
        const ssize_t n = ::write(fd, &one, sizeof(one));
        assert(n == static_cast<ssize_t>(sizeof(one)));
    });

    loop.loop();
    notifier.join();

    assert(readCount == 1);
    assert(firedFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready);

    const uint64_t one = 1;
    const ssize_t n = ::write(fd, &one, sizeof(one));
    assert(n == static_cast<ssize_t>(sizeof(one)));

    ::close(fd);
    return 0;
}
