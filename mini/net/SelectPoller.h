#pragma once

// SelectPoller 是 Windows 的保守 reactor 后端。
// 它用 WinSock select() 维持与 Poller 相同的 Channel 活跃事件语义。

#include "mini/net/Poller.h"

#ifdef _WIN32

namespace mini::net {

class SelectPoller : public Poller {
public:
    explicit SelectPoller(EventLoop* loop);
    ~SelectPoller() override;

    mini::base::Timestamp poll(int timeoutMs, ChannelList* activeChannels) override;
    void updateChannel(Channel* channel) override;
    void removeChannel(Channel* channel) override;

private:
    static constexpr int kNew = -1;
    static constexpr int kAdded = 1;
    static constexpr int kDeleted = 2;

    void fillActiveChannels(const fd_set& readSet,
                            const fd_set& writeSet,
                            const fd_set& exceptSet,
                            ChannelList* activeChannels) const;
};

}  // namespace mini::net

#endif
