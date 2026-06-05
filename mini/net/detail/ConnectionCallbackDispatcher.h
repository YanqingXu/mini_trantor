#pragma once

#include "mini/net/Callbacks.h"

#include <cstddef>

namespace mini::net {
class Buffer;
class EventLoop;
}

namespace mini::net::detail {

class ConnectionCallbackDispatcher {
public:
    void setConnectionCallback(ConnectionCallback cb);
    void setMessageCallback(MessageCallback cb);
    void setHighWaterMarkCallback(HighWaterMarkCallback cb, std::size_t highWaterMark);
    void setWriteCompleteCallback(WriteCompleteCallback cb);
    void setCloseCallback(CloseCallback cb);

    void notifyConnected(const TcpConnectionPtr& connection) const;
    void notifyDisconnected(const TcpConnectionPtr& connection) const;
    void notifyMessage(const TcpConnectionPtr& connection, Buffer* buffer, bool suppressed) const;
    void queueWriteComplete(EventLoop& loop, const TcpConnectionPtr& connection) const;
    void queueHighWaterMark(
        EventLoop& loop,
        const TcpConnectionPtr& connection,
        std::size_t oldLen,
        std::size_t newLen) const;
    void notifyClose(const TcpConnectionPtr& connection) const;

private:
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    HighWaterMarkCallback highWaterMarkCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    CloseCallback closeCallback_;
    std::size_t highWaterMark_{0};
};

}  // namespace mini::net::detail
