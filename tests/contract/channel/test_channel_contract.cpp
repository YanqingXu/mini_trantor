#include "mini/base/Timestamp.h"
#include "mini/net/Channel.h"
#include "mini/net/EventLoop.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

int main() {
    // Contract 1: enableReading registers channel; disableAll + remove deregisters
    {
        mini::net::EventLoop loop;
        const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        assert(fd >= 0);

        mini::net::Channel channel(&loop, fd);
        assert(!loop.hasChannel(&channel));

        channel.enableReading();
        assert(loop.hasChannel(&channel));
        assert(channel.isReading());
        assert(!channel.isWriting());

        channel.enableWriting();
        assert(channel.isWriting());
        assert(channel.isReading());

        channel.disableAll();
        assert(channel.isNoneEvent());
        channel.remove();
        assert(!loop.hasChannel(&channel));

        ::close(fd);
    }

    // Contract 2: handleEvent dispatches correct callback by revents
    {
        mini::net::EventLoop loop;
        const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        assert(fd >= 0);

        mini::net::Channel channel(&loop, fd);
        bool readFired = false;
        bool writeFired = false;
        bool errorFired = false;
        bool closeFired = false;

        channel.setReadCallback([&](mini::base::Timestamp) { readFired = true; });
        channel.setWriteCallback([&] { writeFired = true; });
        channel.setErrorCallback([&] { errorFired = true; });
        channel.setCloseCallback([&] { closeFired = true; });

        // simulate EPOLLIN
        channel.setRevents(EPOLLIN);
        channel.handleEvent(mini::base::now());
        assert(readFired);
        assert(!writeFired);

        // simulate EPOLLOUT
        channel.setRevents(EPOLLOUT);
        channel.handleEvent(mini::base::now());
        assert(writeFired);

        // simulate EPOLLERR
        channel.setRevents(EPOLLERR);
        channel.handleEvent(mini::base::now());
        assert(errorFired);

        // simulate EPOLLHUP without EPOLLIN
        channel.setRevents(EPOLLHUP);
        channel.handleEvent(mini::base::now());
        assert(closeFired);

        ::close(fd);
    }

    // Contract 3: tie blocks callback when owner has expired
    {
        mini::net::EventLoop loop;
        const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        assert(fd >= 0);

        mini::net::Channel channel(&loop, fd);
        bool readFired = false;
        channel.setReadCallback([&](mini::base::Timestamp) { readFired = true; });

        {
            auto owner = std::make_shared<int>(42);
            channel.tie(owner);

            channel.setRevents(EPOLLIN);
            channel.handleEvent(mini::base::now());
            assert(readFired);  // owner alive, callback fires
        }
        // owner expired
        readFired = false;
        channel.setRevents(EPOLLIN);
        channel.handleEvent(mini::base::now());
        assert(!readFired);  // owner expired, callback blocked

        ::close(fd);
    }

    // Contract 4: enable/disable cycle with real EventLoop poll
    {
        mini::net::EventLoop loop;
        const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        assert(fd >= 0);

        mini::net::Channel channel(&loop, fd);
        int readCount = 0;

        channel.setReadCallback([&](mini::base::Timestamp) {
            uint64_t val = 0;
            ::read(fd, &val, sizeof(val));
            ++readCount;
            if (readCount == 1) {
                // disable reading, write again — should not fire
                channel.disableReading();
                const uint64_t one = 1;
                ::write(fd, &one, sizeof(one));
                // schedule re-enable and quit
                loop.runAfter(30ms, [&] {
                    channel.enableReading();
                    loop.runAfter(30ms, [&loop] { loop.quit(); });
                });
            } else {
                channel.disableAll();
                channel.remove();
                loop.quit();
            }
        });

        channel.enableReading();
        const uint64_t one = 1;
        ::write(fd, &one, sizeof(one));

        loop.loop();
        assert(readCount == 2);

        ::close(fd);
    }

    return 0;
}
