#include "mini/net/Poller.h"

#ifdef _WIN32
#include "mini/net/poller/SelectPoller.h"
#else
#include "mini/net/poller/EPollPoller.h"
#endif

namespace mini::net {

std::unique_ptr<Poller> Poller::newDefaultPoller(EventLoop* loop) {
#ifdef _WIN32
    return std::make_unique<SelectPoller>(loop);
#else
    return std::make_unique<EPollPoller>(loop);
#endif
}

}  // namespace mini::net
