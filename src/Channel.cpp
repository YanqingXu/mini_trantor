// Channel.h
#pragma once
#include <functional>

class Channel {
public:
    using Callback = std::function<void()>;

    Channel(int fd);

    void setReadCallback(Callback cb);
    void handleEvent();

    int fd() const;

private:
    int fd_;
    Callback readCallback_;
};
