#include "mini/net/poller/SelectPoller.h"

#ifdef _WIN32

#include "mini/base/Logger.h"
#include "mini/base/Timestamp.h"
#include "mini/net/Channel.h"
#include "mini/net/SocketsOps.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace mini::net {

namespace {

timeval toTimeval(int timeoutMs) noexcept {
    if (timeoutMs < 0) {
        timeoutMs = 0;
    }
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return tv;
}

}  // namespace

SelectPoller::SelectPoller(EventLoop* loop) : Poller(loop) {
    sockets::ensureInitialized();
}

SelectPoller::~SelectPoller() = default;

mini::base::Timestamp SelectPoller::poll(int timeoutMs, ChannelList* activeChannels) {
    if (channels_.empty()) {
        if (timeoutMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        }
        return mini::base::now();
    }

    if (channels_.size() > FD_SETSIZE) {
        LOG_SYSFATAL << "SelectPoller supports at most FD_SETSIZE channels";
    }

    fd_set readSet;
    fd_set writeSet;
    fd_set exceptSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    FD_ZERO(&exceptSet);

    for (const auto& [fd, channel] : channels_) {
        if (channel->index() != kAdded || channel->isNoneEvent()) {
            continue;
        }
        if (channel->events() & Channel::kReadEvent) {
            FD_SET(fd, &readSet);
        }
        if (channel->events() & Channel::kWriteEvent) {
            FD_SET(fd, &writeSet);
        }
        FD_SET(fd, &exceptSet);
    }

    timeval tv = toTimeval(timeoutMs);
    const int numEvents = ::select(0, &readSet, &writeSet, &exceptSet, &tv);
    const auto now = mini::base::now();
    if (numEvents > 0) {
        fillActiveChannels(readSet, writeSet, exceptSet, activeChannels);
    } else if (numEvents == SOCKET_ERROR) {
        const int error = sockets::lastError();
        if (!sockets::isInterrupted(error)) {
            LOG_SYSFATAL << "select: " << sockets::errorMessage(error);
        }
    }
    return now;
}

void SelectPoller::updateChannel(Channel* channel) {
    const auto fd = channel->fd();
    const int index = channel->index();

    if (index == kNew) {
        channels_[fd] = channel;
    }

    if (channel->isNoneEvent()) {
        channel->setIndex(kDeleted);
    } else {
        channel->setIndex(kAdded);
    }
}

void SelectPoller::removeChannel(Channel* channel) {
    channels_.erase(channel->fd());
    channel->setIndex(kNew);
}

void SelectPoller::fillActiveChannels(const fd_set& readSet,
                                      const fd_set& writeSet,
                                      const fd_set& exceptSet,
                                      ChannelList* activeChannels) const {
    for (const auto& [fd, channel] : channels_) {
        uint32_t revents = Channel::kNoneEvent;
        if (FD_ISSET(fd, const_cast<fd_set*>(&readSet))) {
            revents |= Channel::kReadEvent;
        }
        if (FD_ISSET(fd, const_cast<fd_set*>(&writeSet))) {
            revents |= Channel::kWriteEvent;
        }
        if (FD_ISSET(fd, const_cast<fd_set*>(&exceptSet))) {
            revents |= Channel::kErrorEvent;
        }
        if (revents != Channel::kNoneEvent) {
            channel->setRevents(revents);
            activeChannels->push_back(channel);
        }
    }
}

}  // namespace mini::net

#endif
