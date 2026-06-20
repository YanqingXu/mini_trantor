#include "mini/net/kcp/KcpTransport.h"

#include "mini/base/Logger.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TimerId.h"
#include "mini/net/kcp/KcpSession.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace mini::net::kcp {

namespace {

using Clock = std::chrono::steady_clock;

}  // namespace

KcpTransport::KcpTransport(EventLoop* loop,
                           const InetAddress& bindAddr,
                           std::string name,
                           bool reusePort)
    : loop_(loop),
      name_(std::move(name)),
      socket_(std::make_unique<udp::UdpSocket>(loop, bindAddr, reusePort, name_ + "/socket")) {
    socket_->setPacketCallback([this](std::string_view packet, const InetAddress& peerAddr) {
        onPacket(packet, peerAddr);
    });
    socket_->setErrorCallback([this](int err) {
        if (errorCallback_) {
            errorCallback_(err);
        }
    });
}

KcpTransport::~KcpTransport() {
    if (started_) {
        stop();
    }
}

void KcpTransport::setMessageCallback(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

void KcpTransport::setErrorCallback(ErrorCallback cb) {
    errorCallback_ = std::move(cb);
    if (socket_) {
        socket_->setErrorCallback(errorCallback_);
    }
}

void KcpTransport::start() {
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
    startFlushTimer();
}

void KcpTransport::stop() {
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

    stopFlushTimer();
    started_ = false;
    if (socket_) {
        socket_->stop();
    }
    {
        std::scoped_lock lock(mutex_);
        sessions_.clear();
        sessionByAddr_.clear();
        sessionStates_.clear();
    }
}

bool KcpTransport::started() const noexcept {
    return started_;
}

std::shared_ptr<KcpSession> KcpTransport::openSession(const InetAddress& peerAddr,
                                                     transport::TransportSessionId preferredSessionId) {
    if (!loop_ || !loop_->isInLoopThread()) {
        return nullptr;
    }

    const auto peerKey = makeAddressKey(peerAddr);
    std::scoped_lock lock(mutex_);
    const auto it = sessionByAddr_.find(peerKey);
    if (it != sessionByAddr_.end()) {
        const auto session = getSessionLocked(it->second);
        if (session) {
            sessionStates_.try_emplace(session->sessionId(), SessionFlowState{});
        }
        return session;
    }

    transport::TransportSessionId sessionId = preferredSessionId;
    if (sessionId == transport::kInvalidTransportSessionId) {
        sessionId = nextSessionId_++;
    } else {
        if (const auto idIt = sessions_.find(sessionId); idIt != sessions_.end()) {
            sessionStates_.try_emplace(idIt->first, SessionFlowState{});
            return idIt->second;
        }
        if (sessionId >= nextSessionId_) {
            nextSessionId_ = sessionId + 1;
        }
    }

    auto session = std::make_shared<KcpSession>(this, sessionId, peerAddr);
    sessions_[sessionId] = session;
    sessionByAddr_[peerKey] = sessionId;
    sessionStates_.try_emplace(sessionId, SessionFlowState{});
    return session;
}

std::shared_ptr<KcpSession> KcpTransport::getSession(transport::TransportSessionId sessionId) const {
    std::scoped_lock lock(mutex_);
    return getSessionLocked(sessionId);
}

void KcpTransport::closeSession(transport::TransportSessionId sessionId) {
    post([this, sessionId] { removeSession(sessionId); });
}

std::size_t KcpTransport::sessionCount() const {
    std::scoped_lock lock(mutex_);
    return sessions_.size();
}

void KcpTransport::sendTo(transport::TransportSessionId sessionId, std::string_view data) {
    auto payload = std::string(data);
    post([this, sessionId, payload = std::move(payload)]() mutable {
        std::scoped_lock lock(mutex_);
        auto session = getSessionLocked(sessionId);
        if (!session || !socket_) {
            return;
        }

        auto* state = getSessionStateLocked(sessionId);
        if (!state) {
            return;
        }

        if (payload.size() > codec::kKcpMaxPayloadSize) {
            return;
        }

        const auto seq = state->nextSendSeq++;
        codec::KcpFrame frame;
        frame.sessionId = sessionId;
        frame.seq = seq;
        frame.ack = state->lastRecvSeq;
        frame.flags = codec::kKcpFrameFlagData;
        frame.payload = std::move(payload);

        const auto wire = codec::encodeFrame(frame);
        if (wire.empty()) {
            return;
        }

        sendWirePacket(session, wire);

        OutboundPacket packet;
        packet.seq = seq;
        packet.lastSendAt = Clock::now();
        packet.rto = kInitialRto;
        packet.retryCount = 0;
        packet.wirePacket = wire;
        state->inFlight[seq] = std::move(packet);
    });
}

void KcpTransport::sendTo(const InetAddress& peerAddr, std::string_view data) {
    auto payload = std::string(data);
    post([this, peerAddr, payload = std::move(payload)]() mutable {
        if (!socket_) {
            return;
        }
        socket_->sendTo(payload, peerAddr);
    });
}

EventLoop* KcpTransport::getLoop() const noexcept {
    return loop_;
}

std::string_view KcpTransport::name() const noexcept {
    return name_;
}

std::string KcpTransport::makeAddressKey(const InetAddress& addr) {
    return addr.toIpPort();
}

void KcpTransport::onPacket(std::string_view packet, const InetAddress& peerAddr) {
    loop_->assertInLoopThread();

    codec::KcpFrame frame{};
    if (!codec::decodeFrame(packet, frame)) {
        LOG_WARN << "KcpTransport::onPacket decode failed, discard frame";
        return;
    }

    auto session = createOrGetSession(peerAddr, frame.sessionId);
    if (!session) {
        return;
    }

    std::vector<std::string> deliverPayloads;
    bool needAck = false;
    std::uint32_t ackSeq = 0;
    {
        std::scoped_lock lock(mutex_);
        auto* state = getSessionStateLocked(session->sessionId());
        if (!state) {
            return;
        }

        applyAck(*state, frame.ack);

        if ((frame.flags & codec::kKcpFrameFlagData) != 0) {
            processDataPayload(*state, frame, deliverPayloads);
            needAck = true;
            ackSeq = state->lastRecvSeq;
        }

        if (frame.flags == codec::kKcpFrameFlagReset) {
            state->inFlight.clear();
            state->pendingPackets.clear();
            state->nextRecvSeq = 1;
            state->lastRecvSeq = 0;
            ackSeq = state->lastRecvSeq;
        }
    }

    if (needAck || frame.flags == codec::kKcpFrameFlagReset) {
        sendAck(session, ackSeq);
    }

    for (auto& payload : deliverPayloads) {
        if (messageCallback_) {
            messageCallback_(session->sessionId(), payload, peerAddr);
        }
    }
}

void KcpTransport::removeSession(transport::TransportSessionId sessionId) {
    std::shared_ptr<KcpSession> session;
    {
        std::scoped_lock lock(mutex_);
        const auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) {
            return;
        }
        session = it->second;
        sessionByAddr_.erase(makeAddressKey(session->peerAddress()));
        sessions_.erase(it);
        removeSessionStateLocked(sessionId);
    }
    if (session) {
        session->markClosed();
    }
}

void KcpTransport::removeSessionByAddress(const InetAddress& peerAddr) {
    std::shared_ptr<KcpSession> session;
    const auto key = makeAddressKey(peerAddr);
    {
        std::scoped_lock lock(mutex_);
        const auto it = sessionByAddr_.find(key);
        if (it == sessionByAddr_.end()) {
            return;
        }
        const auto sid = it->second;
        sessionByAddr_.erase(it);
        const auto sessionIt = sessions_.find(sid);
        if (sessionIt != sessions_.end()) {
            session = sessionIt->second;
            sessions_.erase(sessionIt);
            removeSessionStateLocked(sid);
        }
    }
    if (session) {
        session->markClosed();
    }
}

std::shared_ptr<KcpSession> KcpTransport::createOrGetSession(const InetAddress& peerAddr,
                                                           transport::TransportSessionId preferredSessionId) {
    const auto key = makeAddressKey(peerAddr);
    std::scoped_lock lock(mutex_);

    const auto it = sessionByAddr_.find(key);
    if (it != sessionByAddr_.end()) {
        return getSessionLocked(it->second);
    }

    transport::TransportSessionId sessionId = preferredSessionId;
    if (sessionId == transport::kInvalidTransportSessionId) {
        sessionId = nextSessionId_++;
    } else {
        if (const auto idIt = sessions_.find(sessionId); idIt != sessions_.end()) {
            const auto& existing = idIt->second;
            if (existing && makeAddressKey(existing->peerAddress()) == key) {
                sessionByAddr_[key] = sessionId;
                sessionStates_.try_emplace(sessionId, SessionFlowState{});
                return idIt->second;
            }
            return nullptr;
        }
        if (sessionId >= nextSessionId_) {
            nextSessionId_ = sessionId + 1;
        }
    }

    auto session = std::make_shared<KcpSession>(this, sessionId, peerAddr);
    sessions_[sessionId] = session;
    sessionByAddr_[key] = sessionId;
    sessionStates_.try_emplace(sessionId, SessionFlowState{});
    return session;
}

std::shared_ptr<KcpSession> KcpTransport::getSessionLocked(transport::TransportSessionId sessionId) const {
    const auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return nullptr;
    }
    return it->second;
}

KcpTransport::SessionFlowState* KcpTransport::getSessionStateLocked(transport::TransportSessionId sessionId) {
    const auto it = sessionStates_.find(sessionId);
    if (it == sessionStates_.end()) {
        return nullptr;
    }
    return &it->second;
}

const KcpTransport::SessionFlowState* KcpTransport::getSessionStateLocked(transport::TransportSessionId sessionId) const {
    const auto it = sessionStates_.find(sessionId);
    if (it == sessionStates_.end()) {
        return nullptr;
    }
    return &it->second;
}

void KcpTransport::removeSessionStateLocked(transport::TransportSessionId sessionId) {
    sessionStates_.erase(sessionId);
}

void KcpTransport::sendWirePacket(const std::shared_ptr<KcpSession>& session, std::string_view packet) {
    if (!socket_ || !session) {
        return;
    }
    socket_->sendTo(packet, session->peerAddress());
}

void KcpTransport::sendAck(const std::shared_ptr<KcpSession>& session, std::uint32_t ackSeq) {
    if (!session) {
        return;
    }

    codec::KcpFrame frame;
    frame.sessionId = session->sessionId();
    frame.flags = codec::kKcpFrameFlagAck;
    frame.ack = ackSeq;

    const auto wire = codec::encodeFrame(frame);
    if (!wire.empty()) {
        sendWirePacket(session, wire);
    }
}

void KcpTransport::applyAck(SessionFlowState& state, std::uint32_t ackSeq) {
    if (ackSeq == 0) {
        return;
    }

    for (auto it = state.inFlight.begin(); it != state.inFlight.end();) {
        if (it->first <= ackSeq) {
            it = state.inFlight.erase(it);
        } else {
            ++it;
        }
    }
}

void KcpTransport::processDataPayload(SessionFlowState& state,
                                    const codec::KcpFrame& frame,
                                    std::vector<std::string>& deliverPayloads) {
    if (frame.seq < state.nextRecvSeq) {
        return;
    }

    if (frame.seq == state.nextRecvSeq) {
        deliverPayloads.push_back(frame.payload);
        state.lastRecvSeq = frame.seq;
        ++state.nextRecvSeq;

        auto it = state.pendingPackets.find(state.nextRecvSeq);
        while (it != state.pendingPackets.end()) {
            deliverPayloads.push_back(std::move(it->second));
            state.lastRecvSeq = it->first;
            ++state.nextRecvSeq;
            state.pendingPackets.erase(it);
            it = state.pendingPackets.find(state.nextRecvSeq);
        }
        return;
    }

    state.pendingPackets[frame.seq] = frame.payload;
}

template <typename Fn>
void KcpTransport::post(Fn&& fn) {
    if (!loop_) {
        return;
    }
    if (loop_->isInLoopThread()) {
        fn();
        return;
    }
    loop_->queueInLoop(std::forward<Fn>(fn));
}

void KcpTransport::startFlushTimer() {
    if (!loop_ || !loop_->isInLoopThread() || hasFlushTimer_) {
        return;
    }
    flushTimerId_ = loop_->runEvery(kFlushInterval, [this] { handleFlushTick(); });
    hasFlushTimer_ = true;
}

void KcpTransport::stopFlushTimer() {
    if (!loop_ || !hasFlushTimer_ || !flushTimerId_.valid()) {
        return;
    }
    loop_->cancel(flushTimerId_);
    flushTimerId_ = {};
    hasFlushTimer_ = false;
}

void KcpTransport::handleFlushTick() {
    if (!started_ || !loop_ || !loop_->isInLoopThread()) {
        return;
    }

    std::vector<std::pair<std::string, InetAddress>> toResend;
    std::vector<transport::TransportSessionId> toClose;
    const auto now = Clock::now();

    {
        std::scoped_lock lock(mutex_);
        for (auto& [sessionId, state] : sessionStates_) {
            const auto sessionIt = sessions_.find(sessionId);
            if (sessionIt == sessions_.end() || !sessionIt->second) {
                continue;
            }

            const auto& peerAddress = sessionIt->second->peerAddress();
            for (auto& [seq, packet] : state.inFlight) {
                if (packet.retryCount >= kMaxRetransmissions) {
                    toClose.push_back(sessionId);
                    break;
                }

                if (now - packet.lastSendAt < packet.rto) {
                    continue;
                }

                packet.retryCount += 1;
                packet.lastSendAt = now;
                packet.rto = std::min(packet.rto * 2, kMaxRto);
                toResend.emplace_back(packet.wirePacket, peerAddress);
            }
        }
    }

    for (auto& item : toResend) {
        if (socket_) {
            socket_->sendTo(item.first, item.second);
        }
    }

    for (const auto sid : toClose) {
        LOG_WARN << "KcpTransport::handleFlushTick close session due to retransmission timeout: "
                 << sid;
        closeSession(sid);
    }
}

}  // namespace mini::net::kcp
