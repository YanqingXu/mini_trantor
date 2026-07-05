#include "mini/net/Poller.h"

#include "mini/net/Channel.h"
#ifdef _WIN32
#include "mini/net/SelectPoller.h"
#else
#include "mini/net/EPollPoller.h"
#endif

namespace mini::net {

Poller::Poller(EventLoop* loop) : ownerLoop_(loop) {
}

Poller::~Poller() = default;

bool Poller::hasChannel(Channel* channel) const {
    auto it = channels_.find(channel->fd());
    return it != channels_.end() && it->second == channel;
}

std::unique_ptr<Poller> Poller::newDefaultPoller(EventLoop* loop) {
#ifdef _WIN32
    return std::make_unique<SelectPoller>(loop);
#else
    return std::make_unique<EPollPoller>(loop);
#endif
}

}  // namespace mini::net
