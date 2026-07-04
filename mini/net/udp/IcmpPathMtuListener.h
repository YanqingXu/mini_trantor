#pragma once

// IcmpPathMtuListener listens for ICMP path MTU failure messages and converts
// them into value-semantic UDP path MTU failure samples.
// It is optional and best-effort: raw sockets may be unavailable without
// CAP_NET_RAW, and callers must continue to rely on normal UDP/KCP probing.

#include "mini/base/noncopyable.h"
#include "mini/net/udp/PathMtuSignal.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <string_view>

namespace mini::net {

class Channel;
class EventLoop;
class Socket;

namespace udp {

class IcmpPathMtuListener final : private mini::base::noncopyable {
public:
    using PathMtuFailureCallback = std::function<void(const PathMtuFailure& failure)>;

    IcmpPathMtuListener(EventLoop* loop,
                        sa_family_t addressFamily,
                        std::uint16_t localUdpPort,
                        std::string name);
    ~IcmpPathMtuListener();

    void setPathMtuFailureCallback(PathMtuFailureCallback cb);

    bool start();
    void stop();
    bool started() const noexcept;
    bool available() const noexcept;

    static bool parseIpv4PacketTooBig(std::string_view packet,
                                      std::uint16_t localUdpPort,
                                      PathMtuFailure& out);
    static bool parseIpv6PacketTooBig(std::string_view packet,
                                      std::uint16_t localUdpPort,
                                      PathMtuFailure& out);

private:
    void handleRead();

    EventLoop* loop_;
    sa_family_t addressFamily_{AF_INET};
    std::uint16_t localUdpPort_{0};
    std::string name_;
    bool started_{false};
    bool available_{false};
    std::unique_ptr<Socket> socket_;
    std::unique_ptr<Channel> channel_;
    PathMtuFailureCallback pathMtuFailureCallback_;
};

}  // namespace udp

}  // namespace mini::net
