#include "mini/net/udp/UdpServer.h"

#include "mini/base/Logger.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/transport/UdpTransportEndpoint.h"
#include "mini/net/udp/UdpSocket.h"

#include <future>

namespace mini::net::udp {

namespace {

std::string makeAddressKey(const InetAddress& addr) {
    return addr.toIpPort();
}

}  // namespace

UdpServer::UdpServer(EventLoop* loop,
                     const InetAddress& listenAddr,
                     std::string name,
                     bool reusePort)
    : loop_(loop),
      name_(std::move(name)),
      socket_(std::make_unique<UdpSocket>(loop, listenAddr, reusePort, name_ + "/socket")),
      lifetimeToken_(std::make_shared<int>(0)) {
    socket_->setPacketCallback([this](std::string_view packet, const InetAddress& peerAddr) {
        onPacket(packet, peerAddr);
    });
}

UdpServer::~UdpServer() {
    stop();
    lifetimeToken_.reset();
}

void UdpServer::setMessageCallback(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

void UdpServer::setErrorCallback(ErrorCallback cb) {
    errorCallback_ = std::move(cb);
    if (socket_) {
        socket_->setErrorCallback(errorCallback_);
    }
}

void UdpServer::setMetricCallback(UdpMetricCallback cb) {
    if (socket_) {
        socket_->setMetricCallback(std::move(cb));
    }
}

void UdpServer::setMaxDatagramsPerRead(std::size_t maxDatagrams) noexcept {
    if (socket_) {
        socket_->setMaxDatagramsPerRead(maxDatagrams);
    }
}

std::size_t UdpServer::maxDatagramsPerRead() const noexcept {
    if (!socket_) {
        return 0;
    }
    return socket_->maxDatagramsPerRead();
}

void UdpServer::start() {
    if (started_.load(std::memory_order_acquire)) {
        return;
    }
    if (!loop_) {
        return;
    }

    if (!loop_->isInLoopThread()) {
        std::weak_ptr<void> lifetime = lifetimeToken_;
        loop_->runInLoop([this, lifetime] {
            if (!lifetime.lock()) {
                return;
            }
            start();
        });
        return;
    }

    if (started_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (socket_) {
        socket_->start();
    }
}

void UdpServer::stop() {
    if (!loop_) {
        return;
    }
    if (!started_.load(std::memory_order_acquire)) {
        std::scoped_lock lock(mutex_);
        if (sessionByAddr_.empty() && peerBySession_.empty()) {
            return;
        }
    }

    if (!loop_->isInLoopThread()) {
        auto done = std::make_shared<std::promise<void>>();
        auto future = done->get_future();
        std::weak_ptr<void> lifetime = lifetimeToken_;
        loop_->runInLoop([this, lifetime, done] {
            if (!lifetime.lock()) {
                done->set_value();
                return;
            }
            stopInLoop();
            done->set_value();
        });
        future.wait();
        return;
    }

    stopInLoop();
}

void UdpServer::sendTo(transport::TransportSessionId sessionId, std::string_view data) {
    auto payload = std::string(data);
    post([this, sessionId, payload = std::move(payload)]() mutable {
        if (!started_.load(std::memory_order_acquire)) {
            return;
        }
        InetAddress peer;
        {
            std::scoped_lock lock(mutex_);
            const auto it = peerBySession_.find(sessionId);
            if (it == peerBySession_.end()) {
                return;
            }
            peer = it->second;
        }
        if (!socket_) {
            return;
        }
        socket_->sendTo(payload, peer);
    });
}

void UdpServer::sendTo(const InetAddress& peerAddr, std::string_view data) {
    auto payload = std::string(data);
    post([this, peerAddr, payload = std::move(payload)]() mutable {
        if (!started_.load(std::memory_order_acquire)) {
            return;
        }
        if (!socket_) {
            return;
        }
        socket_->sendTo(payload, peerAddr);
    });
}

void UdpServer::closeSession(transport::TransportSessionId sessionId) {
    auto sid = sessionId;
    post([this, sid]() { removeSession(sid); });
}

std::shared_ptr<transport::ITransportEndpoint>
UdpServer::getTransportEndpoint(transport::TransportSessionId sessionId) const {
    if (sessionId == transport::kInvalidTransportSessionId) {
        return nullptr;
    }
    if (!hasSession(sessionId)) {
        return nullptr;
    }
    return std::make_shared<transport::UdpTransportEndpoint>(
        const_cast<UdpServer*>(this),
        lifetimeToken_,
        sessionId);
}

bool UdpServer::started() const noexcept {
    return started_.load(std::memory_order_acquire);
}

std::size_t UdpServer::sessionCount() const {
    std::scoped_lock lock(mutex_);
    return peerBySession_.size();
}

bool UdpServer::hasSession(transport::TransportSessionId sessionId) const {
    std::scoped_lock lock(mutex_);
    return peerBySession_.find(sessionId) != peerBySession_.end();
}

EventLoop* UdpServer::getLoop() const noexcept {
    return loop_;
}

std::string_view UdpServer::name() const noexcept {
    return name_;
}

void UdpServer::onPacket(std::string_view packet, const InetAddress& peerAddr) {
    loop_->assertInLoopThread();

    transport::TransportSessionId sessionId = transport::kInvalidTransportSessionId;
    {
        std::scoped_lock lock(mutex_);
        const auto key = makeAddressKey(peerAddr);
        auto it = sessionByAddr_.find(key);

        if (it == sessionByAddr_.end()) {
            sessionId = nextSessionId();
            sessionByAddr_[key] = sessionId;
            peerBySession_[sessionId] = peerAddr;
        } else {
            sessionId = it->second;
        }

        if (sessionId == transport::kInvalidTransportSessionId) {
            return;
        }
    }

    if (messageCallback_) {
        messageCallback_(sessionId, packet, peerAddr);
    }
}

void UdpServer::removeSession(transport::TransportSessionId sessionId) {
    std::scoped_lock lock(mutex_);
    const auto it = peerBySession_.find(sessionId);
    if (it == peerBySession_.end()) {
        return;
    }
    sessionByAddr_.erase(makeAddressKey(it->second));
    peerBySession_.erase(it);
}

template <typename Fn>
void UdpServer::post(Fn&& fn) {
    if (!loop_) {
        return;
    }

    if (loop_->isInLoopThread()) {
        fn();
        return;
    }

    std::weak_ptr<void> lifetime = lifetimeToken_;
    loop_->queueInLoop([lifetime, fn = std::forward<Fn>(fn)]() mutable {
        if (!lifetime.lock()) {
            return;
        }
        fn();
    });
}

void UdpServer::stopInLoop() {
    loop_->assertInLoopThread();
    if (started_.exchange(false, std::memory_order_acq_rel) && socket_) {
        socket_->stop();
    }
    std::scoped_lock lock(mutex_);
    sessionByAddr_.clear();
    peerBySession_.clear();
}

transport::TransportSessionId UdpServer::nextSessionId() {
    return nextSessionId_++;
}

}  // namespace mini::net::udp
