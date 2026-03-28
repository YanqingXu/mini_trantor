#include "mini/net/Poller.h"

#include "mini/net/Channel.h"
#include "mini/net/EPollPoller.h"

namespace mini::net {

Poller::Poller(EventLoop* loop) : ownerLoop_(loop) {
}

Poller::~Poller() = default;

bool Poller::hasChannel(Channel* channel) const {
    auto it = channels_.find(channel->fd());
    return it != channels_.end() && it->second == channel;
}

std::unique_ptr<Poller> Poller::newDefaultPoller(EventLoop* loop) {
    return std::make_unique<EPollPoller>(loop);
}

}  // namespace mini::net
