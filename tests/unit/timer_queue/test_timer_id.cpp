#include "mini/net/EventLoop.h"

#include <cassert>
#include <chrono>

int main() {
    mini::net::TimerId invalid;
    assert(!invalid.valid());

    mini::net::EventLoop loop;
    auto timerId = loop.runAfter(std::chrono::milliseconds(20), [] {});
    assert(timerId.valid());

    loop.cancel(invalid);
    loop.cancel(timerId);
    return 0;
}
