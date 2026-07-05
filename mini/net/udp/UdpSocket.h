#pragma once

// UdpSocket 是 UDP 监听/发送的薄封装，负责 fd 与 Channel 生命周期，
// 只在 owner EventLoop 中调度读事件。读路径带有每轮 datagram budget，
// 避免突发 UDP 输入长时间占用 owner loop。

#include "mini/base/MetricsHook.h"
#include "mini/base/noncopyable.h"
#include "mini/net/Channel.h"
#include "mini/net/InetAddress.h"
#include "mini/net/Socket.h"
#include "mini/net/SocketTypes.h"
#include "mini/net/udp/PathMtuSignal.h"

#include <atomic>
#include <functional>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace mini::net {

class EventLoop;

namespace udp {

class IcmpPathMtuListener;

class UdpSocket final : private mini::base::noncopyable {
public:
    using PacketCallback = std::function<void(std::string_view packet, const InetAddress& peerAddr)>;
    using ErrorCallback = std::function<void(int errorCode)>;
    using PathMtuFailureCallback = std::function<void(const PathMtuFailure& failure)>;

    static constexpr std::size_t kDefaultMaxDatagramsPerRead = 64;

    UdpSocket(EventLoop* loop, const InetAddress& localAddr, bool reusePort = true, std::string name = "udp-socket");
    ~UdpSocket();

    void setPacketCallback(PacketCallback cb);
    void setErrorCallback(ErrorCallback cb);
    void setPathMtuFailureCallback(PathMtuFailureCallback cb);
    void setMetricCallback(UdpMetricCallback cb);

    void setMaxDatagramsPerRead(std::size_t maxDatagrams) noexcept;
    std::size_t maxDatagramsPerRead() const noexcept;
    bool enablePlatformPathMtuSignals(bool enabled);
    bool platformPathMtuSignalsEnabled() const noexcept;
    bool enablePathMtuErrorQueue(bool enabled);
    bool pathMtuErrorQueueEnabled() const noexcept;
    bool enableRawIcmpPathMtuListener(bool enabled);
    bool rawIcmpPathMtuListenerEnabled() const noexcept;

    void start();
    void stop();
    bool started() const noexcept;

    void sendTo(std::string_view data, const InetAddress& peerAddr);

    EventLoop* getLoop() const noexcept;
    SocketFd fd() const noexcept;
    std::string_view name() const noexcept;

private:
    void handleRead();
    void handleError();
    bool handlePathMtuErrorQueue();
    void emitPathMtuFailure(PathMtuFailure failure);
    void emitPathMtuFailure(const InetAddress& peerAddr,
                            std::size_t failedDatagramPayloadSize,
                            std::size_t suggestedDatagramPayloadSize,
                            int errorCode,
                            PathMtuSignalSource source,
                            std::string quotedUdpPayloadPrefix = {});
    void emitReadBatchMetric(std::size_t datagramsRead,
                             std::size_t bytesRead,
                             std::size_t maxDatagramsPerRead,
                             bool budgetExhausted,
                             UdpMetricSample::Duration readDuration);

    EventLoop* loop_;
    std::string name_;
    Socket socket_;
    Channel channel_;
    sa_family_t socketFamily_{AF_INET};
    bool started_{false};
    std::atomic<std::size_t> maxDatagramsPerRead_{kDefaultMaxDatagramsPerRead};
    bool platformPathMtuSignalsEnabled_{false};
    std::unique_ptr<IcmpPathMtuListener> rawIcmpPathMtuListener_;
    PacketCallback packetCallback_;
    ErrorCallback errorCallback_;
    PathMtuFailureCallback pathMtuFailureCallback_;
    UdpMetricCallback metricCallback_;
};

}  // namespace udp

}  // namespace mini::net
