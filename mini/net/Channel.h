#pragma once

// Channel 表示一个 fd 在所属 EventLoop 中的事件订阅与回调分发实体。
// 它不拥有 EventLoop，也不默认拥有 fd，但必须遵守 remove-before-destroy。

#include "mini/base/Timestamp.h"
#include "mini/base/noncopyable.h"

#include <functional>
#include <memory>

#include <sys/epoll.h>

namespace mini::net {

class EventLoop;

class Channel : private mini::base::noncopyable {
public:
    using EventCallback = std::function<void()>;
    using ReadEventCallback = std::function<void(mini::base::Timestamp)>;

    Channel(EventLoop* loop, int fd);
    ~Channel();

    void handleEvent(mini::base::Timestamp receiveTime);
    void setReadCallback(ReadEventCallback cb);
    void setWriteCallback(EventCallback cb);
    void setCloseCallback(EventCallback cb);
    void setErrorCallback(EventCallback cb);

    void tie(const std::shared_ptr<void>& object);

    int fd() const noexcept;
    uint32_t events() const noexcept;
    void setRevents(uint32_t revents) noexcept;
    bool isNoneEvent() const noexcept;
    bool isWriting() const noexcept;
    bool isReading() const noexcept;

    void enableReading();
    void disableReading();
    void enableWriting();
    void disableWriting();
    void disableAll();
    void remove();

    int index() const noexcept;
    void setIndex(int index) noexcept;

    EventLoop* ownerLoop() noexcept;

    static constexpr uint32_t kNoneEvent = 0;
    static constexpr uint32_t kReadEvent = EPOLLIN | EPOLLPRI;
    static constexpr uint32_t kWriteEvent = EPOLLOUT;

private:
    void update();
    void handleEventWithGuard(mini::base::Timestamp receiveTime);

    EventLoop* loop_;
    const int fd_;
    uint32_t events_;
    uint32_t revents_;
    int index_;
    bool eventHandling_;
    bool addedToLoop_;
    bool tied_;
    std::weak_ptr<void> tie_;
    ReadEventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};

}  // namespace mini::net
