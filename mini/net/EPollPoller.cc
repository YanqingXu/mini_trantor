#include "mini/net/EPollPoller.h"

#include "mini/base/Timestamp.h"
#include "mini/net/Channel.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace mini::net {

namespace {

[[noreturn]] void epollDie(const char* what) {
    std::perror(what);
    std::abort();
}

}  // namespace

EPollPoller::EPollPoller(EventLoop* loop)
    : Poller(loop), epollfd_(::epoll_create1(EPOLL_CLOEXEC)), events_(kInitEventListSize) {
    if (epollfd_ < 0) {
        epollDie("epoll_create1");
    }
}

EPollPoller::~EPollPoller() {
    ::close(epollfd_);
}

mini::base::Timestamp EPollPoller::poll(int timeoutMs, ChannelList* activeChannels) {
    const int numEvents =
        ::epoll_wait(epollfd_, events_.data(), static_cast<int>(events_.size()), timeoutMs);
    const auto now = mini::base::now();
    if (numEvents > 0) {
        fillActiveChannels(numEvents, activeChannels);
        if (static_cast<std::size_t>(numEvents) == events_.size()) {
            events_.resize(events_.size() * 2);
        }
    } else if (numEvents < 0 && errno != EINTR) {
        epollDie("epoll_wait");
    }
    return now;
}

void EPollPoller::updateChannel(Channel* channel) {
    const int index = channel->index();
    const int fd = channel->fd();

    if (index == kNew || index == kDeleted) {
        if (index == kNew) {
            channels_[fd] = channel;
        }

        channel->setIndex(kAdded);
        update(EPOLL_CTL_ADD, channel);
        return;
    }

    if (channel->isNoneEvent()) {
        update(EPOLL_CTL_DEL, channel);
        channel->setIndex(kDeleted);
    } else {
        update(EPOLL_CTL_MOD, channel);
    }
}

void EPollPoller::removeChannel(Channel* channel) {
    const int fd = channel->fd();
    channels_.erase(fd);

    if (channel->index() == kAdded) {
        update(EPOLL_CTL_DEL, channel);
    }
    channel->setIndex(kNew);
}

void EPollPoller::fillActiveChannels(int numEvents, ChannelList* activeChannels) const {
    for (int i = 0; i < numEvents; ++i) {
        auto* channel = static_cast<Channel*>(events_[i].data.ptr);
        channel->setRevents(events_[i].events);
        activeChannels->push_back(channel);
    }
}

void EPollPoller::update(int operation, Channel* channel) {
    epoll_event event{};
    event.events = channel->events();
    event.data.ptr = channel;
    if (::epoll_ctl(epollfd_, operation, channel->fd(), &event) < 0) {
        epollDie("epoll_ctl");
    }
}

}  // namespace mini::net
