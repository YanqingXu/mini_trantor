#pragma once

// EPollPoller 是 Poller 的 epoll 后端实现。
// 它负责把 Channel 注册关系映射到内核 epoll，并回填活跃事件。

#include "mini/net/Poller.h"

#include <sys/epoll.h>

#include <vector>

namespace mini::net {

class EPollPoller : public Poller {
public:
    explicit EPollPoller(EventLoop* loop);
    ~EPollPoller() override;

    mini::base::Timestamp poll(int timeoutMs, ChannelList* activeChannels) override;
    void updateChannel(Channel* channel) override;
    void removeChannel(Channel* channel) override;

private:
    static constexpr int kNew = -1;
    static constexpr int kAdded = 1;
    static constexpr int kDeleted = 2;
    static constexpr int kInitEventListSize = 16;

    void fillActiveChannels(int numEvents, ChannelList* activeChannels) const;
    void update(int operation, Channel* channel);

    int epollfd_;
    std::vector<epoll_event> events_;
};

}  // namespace mini::net
