#pragma once

#include "mini/base/Timestamp.h"
#include "mini/base/noncopyable.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace mini::net {

class Channel;
class EventLoop;

class Poller : private mini::base::noncopyable {
public:
    using ChannelList = std::vector<Channel*>;

    explicit Poller(EventLoop* loop);
    virtual ~Poller();

    virtual mini::base::Timestamp poll(int timeoutMs, ChannelList* activeChannels) = 0;
    virtual void updateChannel(Channel* channel) = 0;
    virtual void removeChannel(Channel* channel) = 0;

    bool hasChannel(Channel* channel) const;
    static std::unique_ptr<Poller> newDefaultPoller(EventLoop* loop);

protected:
    using ChannelMap = std::unordered_map<int, Channel*>;
    ChannelMap channels_;

private:
    EventLoop* ownerLoop_;
};

}  // namespace mini::net
