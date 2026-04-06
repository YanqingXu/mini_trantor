#include "mini/net/Channel.h"
#include "mini/net/EventLoop.h"
#include "mini/net/detail/ConnectionBackpressureController.h"

#include <cassert>
#include <stdexcept>
#include <string_view>
#include <sys/eventfd.h>
#include <unistd.h>

int main() {
    mini::net::EventLoop loop;
    const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(fd >= 0);

    {
        mini::net::Channel channel(&loop, fd);
        mini::net::detail::ConnectionBackpressureController controller(&loop);

        bool rejectedMissingHigh = false;
        try {
            mini::net::detail::ConnectionBackpressureController::validateThresholds(0, 1);
        } catch (const std::invalid_argument& error) {
            rejectedMissingHigh = true;
            assert(std::string_view(error.what()) == "backpressure low water mark requires a non-zero high water mark");
        }
        assert(rejectedMissingHigh);

        bool rejectedNonHysteresis = false;
        try {
            mini::net::detail::ConnectionBackpressureController::validateThresholds(8, 8);
        } catch (const std::invalid_argument& error) {
            rejectedNonHysteresis = true;
            assert(std::string_view(error.what()) == "backpressure low water mark must be smaller than high water mark");
        }
        assert(rejectedNonHysteresis);

        channel.enableReading();
        controller.onConnectionEstablished(0, channel);
        assert(channel.isReading());
        assert(controller.readingEnabled());

        controller.configure(8, 4, 0, channel);
        assert(channel.isReading());

        controller.onBufferedBytesChanged(8, channel);
        assert(!channel.isReading());
        assert(!controller.readingEnabled());

        controller.onBufferedBytesChanged(5, channel);
        assert(!channel.isReading());

        controller.onBufferedBytesChanged(4, channel);
        assert(channel.isReading());
        assert(controller.readingEnabled());

        controller.onBufferedBytesChanged(8, channel);
        assert(!channel.isReading());
        controller.configure(0, 0, 8, channel);
        assert(channel.isReading());

        controller.onClosed();
        assert(!controller.readingEnabled());

        channel.disableAll();
        channel.remove();
    }

    ::close(fd);
    return 0;
}
