#include "mini/net/Channel.h"

#include "mini/net/EventLoop.h"

namespace mini::net {

Channel::Channel(EventLoop* loop, int fd)
    : loop_(loop),
      fd_(fd),
      events_(kNoneEvent),
      revents_(0),
      index_(-1),
      eventHandling_(false),
      addedToLoop_(false),
      tied_(false) {
}

Channel::~Channel() = default;

void Channel::handleEvent(mini::base::Timestamp receiveTime) {
    std::shared_ptr<void> guard;
    if (tied_) {
        guard = tie_.lock();
        if (guard) {
            handleEventWithGuard(receiveTime);
        }
        return;
    }
    handleEventWithGuard(receiveTime);
}

void Channel::setReadCallback(ReadEventCallback cb) {
    readCallback_ = std::move(cb);
}

void Channel::setWriteCallback(EventCallback cb) {
    writeCallback_ = std::move(cb);
}

void Channel::setCloseCallback(EventCallback cb) {
    closeCallback_ = std::move(cb);
}

void Channel::setErrorCallback(EventCallback cb) {
    errorCallback_ = std::move(cb);
}

void Channel::tie(const std::shared_ptr<void>& object) {
    tie_ = object;
    tied_ = true;
}

int Channel::fd() const noexcept {
    return fd_;
}

uint32_t Channel::events() const noexcept {
    return events_;
}

void Channel::setRevents(uint32_t revents) noexcept {
    revents_ = revents;
}

bool Channel::isNoneEvent() const noexcept {
    return events_ == kNoneEvent;
}

bool Channel::isWriting() const noexcept {
    return (events_ & kWriteEvent) != 0;
}

bool Channel::isReading() const noexcept {
    return (events_ & kReadEvent) != 0;
}

void Channel::enableReading() {
    events_ |= kReadEvent;
    update();
}

void Channel::disableReading() {
    events_ &= ~kReadEvent;
    update();
}

void Channel::enableWriting() {
    events_ |= kWriteEvent;
    update();
}

void Channel::disableWriting() {
    events_ &= ~kWriteEvent;
    update();
}

void Channel::disableAll() {
    events_ = kNoneEvent;
    update();
}

void Channel::remove() {
    addedToLoop_ = false;
    loop_->removeChannel(this);
}

int Channel::index() const noexcept {
    return index_;
}

void Channel::setIndex(int index) noexcept {
    index_ = index;
}

EventLoop* Channel::ownerLoop() noexcept {
    return loop_;
}

void Channel::update() {
    addedToLoop_ = true;
    loop_->updateChannel(this);
}

void Channel::handleEventWithGuard(mini::base::Timestamp receiveTime) {
    eventHandling_ = true;

    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) {
        if (closeCallback_) {
            closeCallback_();
        }
    }
    if (revents_ & EPOLLERR) {
        if (errorCallback_) {
            errorCallback_();
        }
    }
    if (revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
        if (readCallback_) {
            readCallback_(receiveTime);
        }
    }
    if (revents_ & EPOLLOUT) {
        if (writeCallback_) {
            writeCallback_();
        }
    }

    eventHandling_ = false;
}

}  // namespace mini::net
