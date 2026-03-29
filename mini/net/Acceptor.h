#pragma once

// Acceptor 负责监听 socket 的注册与 accept 路径，是 TcpServer 的薄适配层。
// 它只在所属 EventLoop 线程中修改监听 Channel 和回调交付。

#include "mini/base/noncopyable.h"
#include "mini/net/Channel.h"
#include "mini/net/InetAddress.h"
#include "mini/net/Socket.h"

#include <functional>

namespace mini::net {

class EventLoop;

class Acceptor : private mini::base::noncopyable {
public:
    using NewConnectionCallback = std::function<void(int sockfd, const InetAddress&)>;

    Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reusePort);
    ~Acceptor();

    void setNewConnectionCallback(NewConnectionCallback cb);
    bool listening() const noexcept;
    void listen();

private:
    void handleRead(mini::base::Timestamp receiveTime);

    EventLoop* loop_;
    Socket acceptSocket_;
    Channel acceptChannel_;
    NewConnectionCallback newConnectionCallback_;
    bool listening_;
};

}  // namespace mini::net
