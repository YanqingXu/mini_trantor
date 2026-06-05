#include "mini/net/detail/ConnectionCallbackDispatcher.h"

#include "mini/net/EventLoop.h"

#include <utility>

namespace mini::net::detail {

void ConnectionCallbackDispatcher::setConnectionCallback(ConnectionCallback cb) {
    connectionCallback_ = std::move(cb);
}

void ConnectionCallbackDispatcher::setMessageCallback(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

void ConnectionCallbackDispatcher::setHighWaterMarkCallback(
    HighWaterMarkCallback cb,
    std::size_t highWaterMark) {
    highWaterMarkCallback_ = std::move(cb);
    highWaterMark_ = highWaterMark;
}

void ConnectionCallbackDispatcher::setWriteCompleteCallback(WriteCompleteCallback cb) {
    writeCompleteCallback_ = std::move(cb);
}

void ConnectionCallbackDispatcher::setCloseCallback(CloseCallback cb) {
    closeCallback_ = std::move(cb);
}

void ConnectionCallbackDispatcher::notifyConnected(const TcpConnectionPtr& connection) const {
    if (connectionCallback_) {
        connectionCallback_(connection);
    }
}

void ConnectionCallbackDispatcher::notifyDisconnected(const TcpConnectionPtr& connection) const {
    if (connectionCallback_) {
        connectionCallback_(connection);
    }
}

void ConnectionCallbackDispatcher::notifyMessage(
    const TcpConnectionPtr& connection,
    Buffer* buffer,
    bool suppressed) const {
    if (messageCallback_ && !suppressed) {
        messageCallback_(connection, buffer);
    }
}

void ConnectionCallbackDispatcher::queueWriteComplete(
    EventLoop& loop,
    const TcpConnectionPtr& connection) const {
    if (!writeCompleteCallback_) {
        return;
    }
    loop.queueInLoop([connection, cb = writeCompleteCallback_] { cb(connection); });
}

void ConnectionCallbackDispatcher::queueHighWaterMark(
    EventLoop& loop,
    const TcpConnectionPtr& connection,
    std::size_t oldLen,
    std::size_t newLen) const {
    if (!highWaterMarkCallback_ || highWaterMark_ == 0) {
        return;
    }
    if (oldLen >= highWaterMark_ || newLen < highWaterMark_) {
        return;
    }
    loop.queueInLoop([connection, cb = highWaterMarkCallback_, newLen] { cb(connection, newLen); });
}

void ConnectionCallbackDispatcher::notifyClose(const TcpConnectionPtr& connection) const {
    if (closeCallback_) {
        closeCallback_(connection);
    }
}

}  // namespace mini::net::detail
