#include "mini/net/udp/UdpServer.h"

#include "mini/base/Logger.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/transport/UdpTransportEndpoint.h"
#include "mini/net/udp/UdpSocket.h"

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
    lifetimeToken_.reset();
    if (started_) {
        stop();
    }
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

void UdpServer::start() {
    if (started_) {
        return;
    }
    if (!loop_) {
        return;
    }

    if (!loop_->isInLoopThread()) {
        loop_->runInLoop([this] { start(); });
        return;
    }

    started_ = true;
    if (socket_) {
        socket_->start();
    }
}

void UdpServer::stop() {
    if (!started_) {
        return;
    }
    if (!loop_) {
        return;
    }

    if (!loop_->isInLoopThread()) {
        loop_->runInLoop([this] { stop(); });
        return;
    }

    started_ = false;
    if (socket_) {
        socket_->stop();
    }
    sessionByAddr_.clear();
    peerBySession_.clear();
}

void UdpServer::sendTo(transport::TransportSessionId sessionId, std::string_view data) {
    auto payload = std::string(data);
    post([this, sessionId, payload = std::move(payload)]() mutable {
        const auto it = peerBySession_.find(sessionId);
        if (it == peerBySession_.end() || !socket_) {
            return;
        }
        socket_->sendTo(payload, it->second);
    });
}

void UdpServer::sendTo(const InetAddress& peerAddr, std::string_view data) {
    auto payload = std::string(data);
    post([this, peerAddr, payload = std::move(payload)]() mutable {
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
    return started_;
}

std::size_t UdpServer::sessionCount() const {
    return peerBySession_.size();
}

bool UdpServer::hasSession(transport::TransportSessionId sessionId) const {
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

    const auto key = makeAddressKey(peerAddr);
    auto it = sessionByAddr_.find(key);

    if (it == sessionByAddr_.end()) {
        const auto sessionId = nextSessionId();
        sessionByAddr_[key] = sessionId;
        peerBySession_[sessionId] = peerAddr;
        it = sessionByAddr_.find(key);
    }

    if (it == sessionByAddr_.end()) {
        return;
    }

    if (messageCallback_) {
        messageCallback_(it->second, packet, peerAddr);
    }
}

void UdpServer::removeSession(transport::TransportSessionId sessionId) {
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

    loop_->queueInLoop(std::forward<Fn>(fn));
}

transport::TransportSessionId UdpServer::nextSessionId() {
    return nextSessionId_++;
}

}  // namespace mini::net::udp
