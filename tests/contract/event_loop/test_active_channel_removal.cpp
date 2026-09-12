// Intent: event_loop.intent.md + channel.intent.md.
// Removing another ready Channel invalidates this poll batch's borrowed pointer.
#include "mini/net/Channel.h"
#include "mini/net/EventLoop.h"
#include "mini/net/platform/Wakeup.h"

#include <array>
#include <cassert>
#include <memory>
#include <stdexcept>
#ifndef _WIN32
#include <filesystem>
#endif

int main() {
    using namespace mini::net;
    EventLoop loop;
#ifndef _WIN32
    const auto fdCount = [] {
        return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                             std::filesystem::directory_iterator{});
    };
    const auto before = fdCount();
    for (int i = 0; i != 8; ++i) {
        bool rejected = false;
        try { EventLoop duplicate; }
        catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
    }
    assert(fdCount() == before);
#endif
    std::array<platform::WakeupFdPair, 2> fds{
        platform::createWakeupFds(), platform::createWakeupFds()};
    std::array<std::unique_ptr<Channel>, 2> channels;
    int calls = 0;
    for (int i = 0; i != 2; ++i) {
        channels[i] = std::make_unique<Channel>(&loop, fds[i].readFd);
        channels[i]->setReadCallback([&, i](mini::base::Timestamp) {
            ++calls;
            platform::drainWakeup(fds[i].readFd);
            auto& other = channels[1 - i];
            assert(other);
            other->disableAll();
            other->remove();
            other.reset();
            loop.quit();
        });
        channels[i]->enableReading();
        const auto written = platform::writeWakeup(fds[i].writeFd);
        assert(written > 0);
    }
    loop.loop();
    assert(calls == 1);
    for (int i = 0; i != 2; ++i) {
        if (channels[i]) {
            channels[i]->disableAll();
            channels[i]->remove();
            channels[i].reset();
        }
        platform::closeWakeupFds(fds[i]);
    }
}
