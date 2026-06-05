#include "mini/base/Timestamp.h"
#include "mini/net/Channel.h"
#include "mini/net/EventLoop.h"

#include <cassert>
#include <memory>
#include <sys/eventfd.h>
#include <unistd.h>

int main() {
    mini::net::EventLoop loop;
    const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(fd >= 0);

    {
        mini::net::Channel channel(&loop, fd);

        bool readCalled = false;
        channel.setReadCallback([&](mini::base::Timestamp) { readCalled = true; });
        channel.setRevents(EPOLLIN);
        channel.handleEvent(mini::base::now());
        assert(readCalled);

        readCalled = false;
        auto owner = std::make_shared<int>(42);
        channel.tie(owner);
        owner.reset();
        channel.setRevents(EPOLLIN);
        channel.handleEvent(mini::base::now());
        assert(!readCalled);
    }

    ::close(fd);
    return 0;
}
