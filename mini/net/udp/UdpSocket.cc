#include "mini/net/udp/UdpSocket.h"

#include "mini/net/EventLoop.h"
#include "mini/net/SocketsOps.h"

#include "mini/base/Logger.h"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>

namespace mini::net::udp {

namespace {

constexpr std::size_t kUdpPacketBufferSize = 65535;

}  // namespace

UdpSocket::UdpSocket(EventLoop* loop,
                     const InetAddress& localAddr,
                     bool reusePort,
                     std::string name)
    : loop_(loop),
      name_(std::move(name)),
      socket_(sockets::createNonblockingDatagramOrDie(localAddr.family())),
      channel_(loop, socket_.fd()) {
    socket_.setReuseAddr(true);
    socket_.setReusePort(reusePort);
    if (localAddr.isIpv6()) {
        socket_.setIpv6Only(false);
    }
    socket_.bindAddress(localAddr);

    channel_.setReadCallback([this](mini::base::Timestamp receiveTime) {
        (void)receiveTime;
        handleRead();
    });
}

UdpSocket::~UdpSocket() {
    stop();
}

void UdpSocket::setPacketCallback(PacketCallback cb) {
    packetCallback_ = std::move(cb);
}

void UdpSocket::setErrorCallback(ErrorCallback cb) {
    errorCallback_ = std::move(cb);
}

void UdpSocket::start() {
    loop_->assertInLoopThread();
    if (started_) {
        return;
    }
    started_ = true;
    channel_.enableReading();
}

void UdpSocket::stop() {
    if (!loop_->isInLoopThread()) {
        LOG_WARN << "UdpSocket::stop called from non-owner loop thread; use queueInLoop";
        return;
    }
    if (!started_) {
        return;
    }
    started_ = false;
    channel_.disableAll();
    channel_.remove();
}

bool UdpSocket::started() const noexcept {
    return started_;
}

void UdpSocket::sendTo(std::string_view data, const InetAddress& peerAddr) {
    const ssize_t n = ::sendto(
        socket_.fd(),
        data.data(),
        data.size(),
        0,
        peerAddr.getSockAddr(),
        peerAddr.getSockAddrLen());

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        LOG_SYSERR << "UdpSocket::sendTo failed: " << std::strerror(errno);
        if (errorCallback_) {
            errorCallback_(errno);
        }
    }
}

EventLoop* UdpSocket::getLoop() const noexcept {
    return loop_;
}

int UdpSocket::fd() const noexcept {
    return socket_.fd();
}

std::string_view UdpSocket::name() const noexcept {
    return name_;
}

void UdpSocket::handleRead() {
    char buffer[kUdpPacketBufferSize]{};
    while (true) {
        sockaddr_storage from{};
        socklen_t fromLen = static_cast<socklen_t>(sizeof(from));
        const ssize_t n = ::recvfrom(
            socket_.fd(),
            buffer,
            sizeof(buffer),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &fromLen);

        if (n >= 0) {
            if (packetCallback_) {
                packetCallback_(std::string_view(buffer, static_cast<std::size_t>(n)), InetAddress(from));
            }
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        if (errno == EINTR) {
            continue;
        }

        if (errorCallback_) {
            errorCallback_(errno);
        }
        return;
    }
}

}  // namespace mini::net::udp
