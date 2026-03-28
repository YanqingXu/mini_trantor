// Poller.h
#pragma once
#include <vector>

class Channel;

class Poller {
public:
    Poller();
    void poll(std::vector<Channel*>& activeChannels);
    void updateChannel(Channel* ch);

private:
    int epollfd_;
};
