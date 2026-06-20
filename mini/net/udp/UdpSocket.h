#pragma once

// UdpSocket 是 UDP 监听/发送的薄封装，负责 fd 与 Channel 生命周期，
// 只在 owner EventLoop 中调度读事件。

#include "mini/base/noncopyable.h"
#include "mini/net/Channel.h"
#include "mini/net/InetAddress.h"
#include "mini/net/Socket.h"

#include <functional>
#include <string>
#include <string_view>

namespace mini::net {

class EventLoop;

namespace udp {

class UdpSocket final : private mini::base::noncopyable {
public:
    using PacketCallback = std::function<void(std::string_view packet, const InetAddress& peerAddr)>;
    using ErrorCallback = std::function<void(int errorCode)>;

    UdpSocket(EventLoop* loop, const InetAddress& localAddr, bool reusePort = true, std::string name = "udp-socket");
    ~UdpSocket();

    void setPacketCallback(PacketCallback cb);
    void setErrorCallback(ErrorCallback cb);

    void start();
    void stop();
    bool started() const noexcept;

    void sendTo(std::string_view data, const InetAddress& peerAddr);

    EventLoop* getLoop() const noexcept;
    int fd() const noexcept;
    std::string_view name() const noexcept;

private:
    void handleRead();

    EventLoop* loop_;
    std::string name_;
    Socket socket_;
    Channel channel_;
    bool started_{false};
    PacketCallback packetCallback_;
    ErrorCallback errorCallback_;
};

}  // namespace udp

}  // namespace mini::net
