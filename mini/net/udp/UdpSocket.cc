#include "mini/net/udp/UdpSocket.h"

#include "mini/net/EventLoop.h"
#include "mini/net/SocketsOps.h"
#include "mini/net/udp/IcmpPathMtuListener.h"
#include "mini/net/udp/PathMtuSignalAdapter.h"

#include "mini/base/Logger.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>

namespace mini::net::udp {

namespace {

constexpr std::size_t kUdpPacketBufferSize = 65535;

std::string quotedUdpPayloadPrefix(std::string_view data) {
    const auto prefixLength = std::min(
        data.size(),
        kMaxPathMtuQuotedUdpPayloadPrefix);
    return std::string(data.substr(0, prefixLength));
}

}  // namespace

UdpSocket::UdpSocket(EventLoop* loop,
                     const InetAddress& localAddr,
                     bool reusePort,
                     std::string name)
    : loop_(loop),
      name_(std::move(name)),
      socket_(sockets::createNonblockingDatagramOrDie(localAddr.family())),
      channel_(loop, socket_.fd()),
      socketFamily_(localAddr.family()) {
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
    channel_.setErrorCallback([this] {
        handleError();
    });
}

UdpSocket::~UdpSocket() {
    if ((started_ || rawIcmpPathMtuListenerEnabled()) &&
        loop_ != nullptr &&
        !loop_->isInLoopThread()) {
        auto done = std::make_shared<std::promise<void>>();
        auto future = done->get_future();
        loop_->runInLoop([this, done] {
            stop();
            done->set_value();
        });
        future.wait();
        return;
    }
    stop();
}

void UdpSocket::setPacketCallback(PacketCallback cb) {
    packetCallback_ = std::move(cb);
}

void UdpSocket::setErrorCallback(ErrorCallback cb) {
    errorCallback_ = std::move(cb);
}

void UdpSocket::setPathMtuFailureCallback(PathMtuFailureCallback cb) {
    pathMtuFailureCallback_ = std::move(cb);
}

void UdpSocket::setMetricCallback(UdpMetricCallback cb) {
    metricCallback_ = std::move(cb);
}

void UdpSocket::setMaxDatagramsPerRead(std::size_t maxDatagrams) noexcept {
    if (maxDatagrams == 0) {
        maxDatagrams = 1;
    }
    maxDatagramsPerRead_.store(maxDatagrams, std::memory_order_release);
}

std::size_t UdpSocket::maxDatagramsPerRead() const noexcept {
    return maxDatagramsPerRead_.load(std::memory_order_acquire);
}

bool UdpSocket::enablePlatformPathMtuSignals(bool enabled) {
    const bool configured = PathMtuSignalAdapter::configurePlatformPathMtuSignals(
        socket_.fd(),
        socketFamily_,
        enabled);
    platformPathMtuSignalsEnabled_ = enabled && configured;
    if (!enabled) {
        platformPathMtuSignalsEnabled_ = false;
        return configured;
    }
    return platformPathMtuSignalsEnabled_;
}

bool UdpSocket::platformPathMtuSignalsEnabled() const noexcept {
    return platformPathMtuSignalsEnabled_;
}

bool UdpSocket::enablePathMtuErrorQueue(bool enabled) {
    return enablePlatformPathMtuSignals(enabled);
}

bool UdpSocket::pathMtuErrorQueueEnabled() const noexcept {
    return platformPathMtuSignalsEnabled();
}

bool UdpSocket::enableRawIcmpPathMtuListener(bool enabled) {
    if (!loop_ || !loop_->isInLoopThread()) {
        return false;
    }

    if (!enabled) {
        if (rawIcmpPathMtuListener_) {
            rawIcmpPathMtuListener_->stop();
            rawIcmpPathMtuListener_.reset();
        }
        return true;
    }

    if (socketFamily_ != AF_INET && socketFamily_ != AF_INET6) {
        return false;
    }

    if (!rawIcmpPathMtuListener_) {
        const auto localAddress = InetAddress(sockets::getLocalAddr(socket_.fd()));
        rawIcmpPathMtuListener_ = std::make_unique<IcmpPathMtuListener>(
            loop_,
            localAddress.family(),
            localAddress.port(),
            name_ + "/icmp-pmtu");
        rawIcmpPathMtuListener_->setPathMtuFailureCallback(
            [this](const PathMtuFailure& failure) {
                emitPathMtuFailure(failure);
            });
    }

    if (rawIcmpPathMtuListener_->started()) {
        return true;
    }
    if (!rawIcmpPathMtuListener_->start()) {
        rawIcmpPathMtuListener_.reset();
        return false;
    }
    return true;
}

bool UdpSocket::rawIcmpPathMtuListenerEnabled() const noexcept {
    return rawIcmpPathMtuListener_ && rawIcmpPathMtuListener_->started();
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
    if (rawIcmpPathMtuListener_) {
        rawIcmpPathMtuListener_->stop();
        rawIcmpPathMtuListener_.reset();
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
    const ssize_t n = sockets::sendTo(
        socket_.fd(),
        data.data(),
        data.size(),
        peerAddr.getSockAddr(),
        peerAddr.getSockAddrLen());

    if (n < 0) {
        const int error = sockets::lastError();
        if (sockets::isWouldBlock(error) || sockets::isInterrupted(error)) {
            return;
        }
        if (sockets::isMessageSize(error)) {
            emitPathMtuFailure(
                peerAddr,
                data.size(),
                0,
                error,
                PathMtuSignalSource::kLocalSend,
                quotedUdpPayloadPrefix(data));
        }
        LOG_SYSERR << "UdpSocket::sendTo failed: " << sockets::errorMessage(error);
        if (errorCallback_) {
            errorCallback_(error);
        }
    }
}

EventLoop* UdpSocket::getLoop() const noexcept {
    return loop_;
}

SocketFd UdpSocket::fd() const noexcept {
    return socket_.fd();
}

std::string_view UdpSocket::name() const noexcept {
    return name_;
}

void UdpSocket::handleRead() {
    char buffer[kUdpPacketBufferSize]{};
    const auto batchStart = std::chrono::steady_clock::now();
    const auto maxDatagrams = maxDatagramsPerRead();
    std::size_t datagramsRead = 0;
    std::size_t bytesRead = 0;

    while (datagramsRead < maxDatagrams) {
        sockaddr_storage from{};
        socklen_t fromLen = static_cast<socklen_t>(sizeof(from));
        const ssize_t n = sockets::recvFrom(
            socket_.fd(),
            buffer,
            sizeof(buffer),
            &from,
            &fromLen);

        if (n >= 0) {
            ++datagramsRead;
            bytesRead += static_cast<std::size_t>(n);
            if (packetCallback_) {
                packetCallback_(std::string_view(buffer, static_cast<std::size_t>(n)), InetAddress(from));
            }
            continue;
        }

        const int error = sockets::lastError();
        if (sockets::isWouldBlock(error)) {
            emitReadBatchMetric(
                datagramsRead,
                bytesRead,
                maxDatagrams,
                false,
                std::chrono::steady_clock::now() - batchStart);
            return;
        }
        if (sockets::isInterrupted(error)) {
            continue;
        }

        if (errorCallback_) {
            errorCallback_(error);
        }
        emitReadBatchMetric(
            datagramsRead,
            bytesRead,
            maxDatagrams,
            false,
            std::chrono::steady_clock::now() - batchStart);
        return;
    }

    emitReadBatchMetric(
        datagramsRead,
        bytesRead,
        maxDatagrams,
        true,
        std::chrono::steady_clock::now() - batchStart);
}

void UdpSocket::handleError() {
    const bool handledPathMtu = handlePathMtuErrorQueue();
    if (handledPathMtu) {
        return;
    }

    int error = 0;
    socklen_t len = static_cast<socklen_t>(sizeof(error));
    if (::getsockopt(
            socket_.fd(),
            SOL_SOCKET,
            SO_ERROR,
            reinterpret_cast<char*>(&error),
            &len) == 0 &&
        error != 0 &&
        errorCallback_) {
        errorCallback_(error);
    }
}

bool UdpSocket::handlePathMtuErrorQueue() {
    if (!platformPathMtuSignalsEnabled_) {
        return false;
    }
    return PathMtuSignalAdapter::drainPlatformPathMtuSignals(
        socket_.fd(),
        [this](const PathMtuFailure& failure) {
            emitPathMtuFailure(failure);
        },
        errorCallback_);
}

void UdpSocket::emitPathMtuFailure(PathMtuFailure failure) {
    if (!pathMtuFailureCallback_) {
        return;
    }
    pathMtuFailureCallback_(failure);
}

void UdpSocket::emitPathMtuFailure(const InetAddress& peerAddr,
                                   std::size_t failedDatagramPayloadSize,
                                   std::size_t suggestedDatagramPayloadSize,
                                   int errorCode,
                                   PathMtuSignalSource source,
                                   std::string quotedUdpPayloadPrefix) {
    PathMtuFailure failure;
    failure.peerAddr = peerAddr;
    failure.failedDatagramPayloadSize = failedDatagramPayloadSize;
    failure.suggestedDatagramPayloadSize = suggestedDatagramPayloadSize;
    failure.errorCode = errorCode;
    failure.source = source;
    failure.quotedUdpPayloadPrefix = std::move(quotedUdpPayloadPrefix);
    emitPathMtuFailure(std::move(failure));
}

void UdpSocket::emitReadBatchMetric(std::size_t datagramsRead,
                                    std::size_t bytesRead,
                                    std::size_t maxDatagramsPerRead,
                                    bool budgetExhausted,
                                    UdpMetricSample::Duration readDuration) {
    if (!metricCallback_ || (datagramsRead == 0 && !budgetExhausted)) {
        return;
    }

    UdpMetricSample sample;
    sample.event = UdpMetricEvent::ReadBatch;
    sample.loop = loop_;
    sample.socketName = name_;
    sample.datagramsRead = datagramsRead;
    sample.bytesRead = bytesRead;
    sample.maxDatagramsPerRead = maxDatagramsPerRead;
    sample.budgetExhausted = budgetExhausted;
    sample.readDuration = readDuration;
    metricCallback_(sample);
}

}  // namespace mini::net::udp
