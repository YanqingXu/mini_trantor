// EventLoop.h
#pragma once
#include <vector>

class Channel;

class EventLoop {
public:
    void loop();

    void updateChannel(Channel* ch);

private:
    int epollfd_;
    std::vector<Channel*> activeChannels_;
};
