#include "mini/net/kcp/KcpTransport.h"

#include "mini/base/Logger.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TimerId.h"
#include "mini/net/kcp/KcpSession.h"

#include <algorithm>
#include <future>
#include <limits>
#include <utility>
#include <vector>

namespace mini::net::kcp {

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::string_view kMtuProbeMagic = "MTP1";
constexpr std::string_view kMtuProbeAckMagic = "MTA1";
constexpr std::size_t kMtuProbeControlPayloadSize = 8;
constexpr std::string_view kXorParityMagic = "XRP1";
constexpr std::size_t kQuotedKcpSessionIdPrefixSize = 12;

void appendUint32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

void appendUint16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

std::uint32_t readUint32(std::string_view payload, std::size_t offset) {
    const auto byte = [&](std::size_t index) {
        return static_cast<unsigned char>(payload[index]);
    };
    return (static_cast<std::uint32_t>(byte(offset)) << 24) |
           (static_cast<std::uint32_t>(byte(offset + 1)) << 16) |
           (static_cast<std::uint32_t>(byte(offset + 2)) << 8) |
           static_cast<std::uint32_t>(byte(offset + 3));
}

std::uint16_t readUint16(std::string_view payload, std::size_t offset) {
    const auto byte = [&](std::size_t index) {
        return static_cast<unsigned char>(payload[index]);
    };
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(byte(offset)) << 8) |
        static_cast<std::uint16_t>(byte(offset + 1)));
}

std::uint64_t readUint64(std::string_view payload, std::size_t offset) {
    const auto byte = [&](std::size_t index) {
        return static_cast<unsigned char>(payload[index]);
    };
    return (static_cast<std::uint64_t>(byte(offset)) << 56) |
           (static_cast<std::uint64_t>(byte(offset + 1)) << 48) |
           (static_cast<std::uint64_t>(byte(offset + 2)) << 40) |
           (static_cast<std::uint64_t>(byte(offset + 3)) << 32) |
           (static_cast<std::uint64_t>(byte(offset + 4)) << 24) |
           (static_cast<std::uint64_t>(byte(offset + 5)) << 16) |
           (static_cast<std::uint64_t>(byte(offset + 6)) << 8) |
           static_cast<std::uint64_t>(byte(offset + 7));
}

}  // namespace

KcpTransport::KcpTransport(EventLoop* loop,
                           const InetAddress& bindAddr,
                           std::string name,
                           bool reusePort,
                           KcpTransportOptions options)
    : loop_(loop),
      name_(std::move(name)),
      options_(normalizeOptions(options)),
      socket_(std::make_unique<udp::UdpSocket>(loop, bindAddr, reusePort, name_ + "/socket")),
      lifetimeToken_(std::make_shared<int>(0)) {
    socket_->setPacketCallback([this](std::string_view packet, const InetAddress& peerAddr) {
        onPacket(packet, peerAddr);
    });
    socket_->setErrorCallback([this](int err) {
        if (errorCallback_) {
            errorCallback_(err);
        }
    });
    socket_->setPathMtuFailureCallback([this](const udp::PathMtuFailure& failure) {
        notifyPathMtuFailure(failure);
    });
    if (options_.enableMtuProbing && options_.enablePlatformPathMtuSignals) {
        socket_->enablePlatformPathMtuSignals(true);
    }
}

KcpTransport::~KcpTransport() {
    stop();
    lifetimeToken_.reset();
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
        if (options_.enableMtuProbing && options_.enableRawIcmpPathMtuSignals) {
            socket_->enableRawIcmpPathMtuListener(true);
        }
    }
    startFlushTimer();
}

void KcpTransport::stop() {
    if (!loop_) {
        return;
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

void KcpTransport::stopInLoop() {
    loop_->assertInLoopThread();
    if (started_.exchange(false, std::memory_order_acq_rel)) {
        stopFlushTimer();
    }
    if (socket_) {
        socket_->stop();
    }
    std::vector<std::shared_ptr<KcpSession>> closedSessions;
    {
        std::scoped_lock lock(mutex_);
        closedSessions.reserve(sessions_.size());
        for (const auto& [sessionId, session] : sessions_) {
            (void)sessionId;
            if (session) {
                closedSessions.push_back(session);
            }
        }
        sessions_.clear();
        sessionByAddr_.clear();
        sessionStates_.clear();
        mtuPathCache_.clear();
    }
    for (const auto& session : closedSessions) {
        session->markClosed();
    }
}

bool KcpTransport::started() const noexcept {
    return started_.load(std::memory_order_acquire);
}

std::shared_ptr<KcpSession> KcpTransport::openSession(const InetAddress& peerAddr,
                                                     transport::TransportSessionId preferredSessionId) {
    if (!loop_) {
        return nullptr;
    }
    if (!loop_->isInLoopThread()) {
        auto result = std::make_shared<std::promise<std::shared_ptr<KcpSession>>>();
        auto future = result->get_future();
        std::weak_ptr<void> lifetime = lifetimeToken_;
        loop_->queueInLoop([this, lifetime, peerAddr, preferredSessionId, result] {
            if (!lifetime.lock()) {
                result->set_value(nullptr);
                return;
            }
            result->set_value(openSession(peerAddr, preferredSessionId));
        });
        return future.get();
    }

    if (!started_.load(std::memory_order_acquire)) {
        return nullptr;
    }

    const auto peerKey = makeAddressKey(peerAddr);
    std::scoped_lock lock(mutex_);
    const auto it = sessionByAddr_.find(peerKey);
    if (it != sessionByAddr_.end()) {
        const auto session = getSessionLocked(it->second);
        if (session) {
            auto [stateIt, inserted] = sessionStates_.try_emplace(session->sessionId(), SessionFlowState{});
            if (inserted) {
                seedMtuStateFromPathCacheLocked(stateIt->second, peerAddr, Clock::now());
            }
        }
        return session;
    }

    transport::TransportSessionId sessionId = preferredSessionId;
    if (sessionId == transport::kInvalidTransportSessionId) {
        sessionId = nextSessionId_++;
    } else {
        if (const auto idIt = sessions_.find(sessionId); idIt != sessions_.end()) {
            auto [stateIt, inserted] = sessionStates_.try_emplace(idIt->first, SessionFlowState{});
            if (inserted && idIt->second) {
                seedMtuStateFromPathCacheLocked(stateIt->second, idIt->second->peerAddress(), Clock::now());
            }
            return idIt->second;
        }
        if (sessionId >= nextSessionId_) {
            nextSessionId_ = sessionId + 1;
        }
    }

    auto session = std::make_shared<KcpSession>(this, sessionId, peerAddr);
    sessions_[sessionId] = session;
    sessionByAddr_[peerKey] = sessionId;
    auto [stateIt, inserted] = sessionStates_.try_emplace(sessionId, SessionFlowState{});
    if (inserted) {
        seedMtuStateFromPathCacheLocked(stateIt->second, peerAddr, Clock::now());
    }
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
        if (!started_.load(std::memory_order_acquire) || !socket_) {
            return;
        }

        std::shared_ptr<KcpSession> session;
        std::vector<std::string> wires;
        {
            std::scoped_lock lock(mutex_);
            session = getSessionLocked(sessionId);
            if (!session) {
                return;
            }

            auto* state = getSessionStateLocked(sessionId);
            if (!state) {
                return;
            }

            if (payload.size() > options_.maxApplicationPayloadSize) {
                return;
            }

            const auto now = Clock::now();
            auto queueFrame = [&](std::uint16_t flags, std::string framePayload) {
                if (framePayload.size() > codec::kKcpMaxPayloadSize) {
                    return;
                }

                const auto seq = state->nextSendSeq++;
                codec::KcpFrame frame;
                frame.sessionId = sessionId;
                frame.seq = seq;
                frame.ack = state->lastRecvSeq;
                frame.flags = flags;
                frame.payload = std::move(framePayload);

                auto wire = codec::encodeFrame(frame);
                if (wire.empty()) {
                    return;
                }

                OutboundPacket packet;
                packet.sessionId = sessionId;
                packet.seq = seq;
                packet.flags = flags;
                packet.rto = options_.initialRto;
                packet.retryCount = 0;
                packet.payload = frame.payload;
                packet.wirePacket = wire;
                queueOrSendPacketLocked(*state, std::move(packet), now, wires);
            };

            const auto datagramPayloadSize = effectiveDatagramPayloadSize(*state);
            const auto singleFrameLimit = singleFramePayloadLimit(datagramPayloadSize);
            const auto fragmentLimit = fragmentPayloadLimit(datagramPayloadSize);
            if (payload.size() <= singleFrameLimit) {
                queueFrame(codec::kKcpFrameFlagData, std::move(payload));
            } else {
                const auto fragmentCountSize =
                    (payload.size() + fragmentLimit - 1) / fragmentLimit;
                if (fragmentCountSize > std::numeric_limits<std::uint16_t>::max()) {
                    return;
                }
                const auto fragmentCount = static_cast<std::uint16_t>(fragmentCountSize);
                auto messageId = state->nextMessageId++;
                if (state->nextMessageId == 0) {
                    state->nextMessageId = 1;
                }

                for (std::uint16_t index = 0; index < fragmentCount; ++index) {
                    const auto offset = static_cast<std::size_t>(index) * fragmentLimit;
                    const auto length = std::min(fragmentLimit, payload.size() - offset);
                    queueFrame(codec::kKcpFrameFlagData | codec::kKcpFrameFlagFragment,
                               encodeFragmentPayload(
                                   messageId,
                                   index,
                                   fragmentCount,
                                   std::string_view(payload.data() + offset, length),
                                   fragmentLimit));
                }
            }
        }

        for (const auto& wire : wires) {
            sendWirePacket(session, wire);
        }
    });
}

void KcpTransport::sendTo(const InetAddress& peerAddr, std::string_view data) {
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

void KcpTransport::notifyPathMtuFailure(const InetAddress& peerAddr,
                                        std::size_t failedDatagramPayloadSize,
                                        std::size_t suggestedDatagramPayloadSize) {
    udp::PathMtuFailure failure;
    failure.peerAddr = peerAddr;
    failure.failedDatagramPayloadSize = failedDatagramPayloadSize;
    failure.suggestedDatagramPayloadSize = suggestedDatagramPayloadSize;
    post([this, failure = std::move(failure)]() mutable {
        applyPathMtuFailureSignal(std::move(failure), false);
    });
}

void KcpTransport::notifyPathMtuFailure(const udp::PathMtuFailure& failure) {
    post([this, failure]() mutable {
        applyPathMtuFailureSignal(std::move(failure), true);
    });
}

EventLoop* KcpTransport::getLoop() const noexcept {
    return loop_;
}

std::string_view KcpTransport::name() const noexcept {
    return name_;
}

const KcpTransportOptions& KcpTransport::options() const noexcept {
    return options_;
}

KcpTransportOptions KcpTransport::normalizeOptions(KcpTransportOptions options) noexcept {
    const KcpTransportOptions defaults;
    if (options.initialRto <= std::chrono::milliseconds::zero()) {
        options.initialRto = defaults.initialRto;
    }
    if (options.maxRto < options.initialRto) {
        options.maxRto = options.initialRto;
    }
    if (options.maxRetransmissions == 0) {
        options.maxRetransmissions = defaults.maxRetransmissions;
    }

    const auto minUsableDatagramPayloadSize =
        codec::kKcpFrameHeaderSize + kFragmentHeaderSize + 1;
    if (options.maxDatagramPayloadSize < minUsableDatagramPayloadSize) {
        options.maxDatagramPayloadSize = defaults.maxDatagramPayloadSize;
    }

    const auto maxCodecDatagramPayloadSize =
        codec::kKcpFrameHeaderSize + codec::kKcpMaxPayloadSize;
    if (options.maxDatagramPayloadSize > maxCodecDatagramPayloadSize) {
        options.maxDatagramPayloadSize = maxCodecDatagramPayloadSize;
    }

    if (options.minDatagramPayloadSize < minUsableDatagramPayloadSize) {
        options.minDatagramPayloadSize = defaults.minDatagramPayloadSize;
    }
    if (options.minDatagramPayloadSize > maxCodecDatagramPayloadSize) {
        options.minDatagramPayloadSize = maxCodecDatagramPayloadSize;
    }
    if (options.minDatagramPayloadSize > options.maxDatagramPayloadSize) {
        options.minDatagramPayloadSize = options.maxDatagramPayloadSize;
    }
    if (options.mtuProbeStepBytes == 0) {
        options.mtuProbeStepBytes = defaults.mtuProbeStepBytes;
    }
    if (options.mtuProbeMaxRetries == 0) {
        options.mtuProbeMaxRetries = defaults.mtuProbeMaxRetries;
    }
    if (options.mtuProbeInterval <= std::chrono::milliseconds::zero()) {
        options.mtuProbeInterval = defaults.mtuProbeInterval;
    }
    if (options.mtuProbeBlackholeCooldown <= std::chrono::milliseconds::zero()) {
        options.mtuProbeBlackholeCooldown = defaults.mtuProbeBlackholeCooldown;
    }
    if (options.minCongestionWindow == 0) {
        options.minCongestionWindow = defaults.minCongestionWindow;
    }
    if (options.maxCongestionWindow < options.minCongestionWindow) {
        options.maxCongestionWindow = options.minCongestionWindow;
    }
    if (options.initialCongestionWindow == 0) {
        options.initialCongestionWindow = defaults.initialCongestionWindow;
    }
    options.initialCongestionWindow =
        std::clamp(options.initialCongestionWindow,
                   options.minCongestionWindow,
                   options.maxCongestionWindow);
    if (options.redundantCopyCount == 0) {
        options.redundantCopyCount = defaults.redundantCopyCount;
    }
    options.redundantCopyCount =
        std::min(options.redundantCopyCount, kMaxRedundantCopyCount);
    if (options.xorParityGroupSize < 2) {
        options.xorParityGroupSize = defaults.xorParityGroupSize;
    }
    options.xorParityGroupSize =
        std::min(options.xorParityGroupSize, kMaxXorParityGroupSize);

    const auto fragmentPayloadLimit =
        options.maxDatagramPayloadSize - codec::kKcpFrameHeaderSize - kFragmentHeaderSize;
    const auto maxPayloadByFragmentCount =
        fragmentPayloadLimit * static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max());
    if (options.maxApplicationPayloadSize == 0) {
        options.maxApplicationPayloadSize = defaults.maxApplicationPayloadSize;
    }
    options.maxApplicationPayloadSize =
        std::min(options.maxApplicationPayloadSize, maxPayloadByFragmentCount);
    return options;
}

std::size_t KcpTransport::initialDatagramPayloadSize() const noexcept {
    return options_.enableMtuProbing ? options_.minDatagramPayloadSize : options_.maxDatagramPayloadSize;
}

std::size_t KcpTransport::effectiveDatagramPayloadSize(SessionFlowState& state) const noexcept {
    if (state.currentDatagramPayloadSize == 0) {
        state.currentDatagramPayloadSize = initialDatagramPayloadSize();
    }
    return state.currentDatagramPayloadSize;
}

void KcpTransport::seedMtuStateFromPathCacheLocked(SessionFlowState& state,
                                                   const InetAddress& peerAddress,
                                                   std::chrono::steady_clock::time_point now) {
    if (!options_.enableMtuProbing || !options_.enableMtuPathCache) {
        return;
    }

    auto entry = findMtuPathCacheEntryLocked(peerAddress);
    if (!entry) {
        return;
    }

    if (entry->confirmedDatagramPayloadSize != 0) {
        const auto cachedSize = std::clamp(entry->confirmedDatagramPayloadSize,
                                           initialDatagramPayloadSize(),
                                           options_.maxDatagramPayloadSize);
        const auto current = effectiveDatagramPayloadSize(state);
        if (cachedSize > current) {
            state.currentDatagramPayloadSize = cachedSize;
        }
    }

    if (entry->cooldownUntil == Clock::time_point{}) {
        return;
    }

    if (now >= entry->cooldownUntil) {
        clearMtuPathCacheCooldownLocked(peerAddress);
        return;
    }

    if (state.mtuProbeCooldownUntil == Clock::time_point{} ||
        state.mtuProbeCooldownUntil < entry->cooldownUntil) {
        state.mtuProbeCooldownUntil = entry->cooldownUntil;
    }
    state.mtuProbeBlackholeCount =
        std::max(state.mtuProbeBlackholeCount, entry->blackholeCount);
}

void KcpTransport::recordMtuPathSuccessLocked(const InetAddress& peerAddress,
                                              std::size_t confirmedDatagramPayloadSize) {
    if (!options_.enableMtuProbing || !options_.enableMtuPathCache) {
        return;
    }

    if (confirmedDatagramPayloadSize < initialDatagramPayloadSize()) {
        return;
    }

    const auto cachedSize = std::min(confirmedDatagramPayloadSize,
                                     options_.maxDatagramPayloadSize);
    if (options_.sharedMtuPathCache) {
        options_.sharedMtuPathCache->recordSuccess(peerAddress, cachedSize);
        return;
    }

    auto& entry = mtuPathCache_[makeAddressKey(peerAddress)];
    entry.confirmedDatagramPayloadSize =
        std::max(entry.confirmedDatagramPayloadSize, cachedSize);
    entry.cooldownUntil = {};
    entry.blackholeCount = 0;
}

void KcpTransport::recordMtuPathBlackholeLocked(const InetAddress& peerAddress,
                                                const SessionFlowState& state) {
    if (!options_.enableMtuProbing || !options_.enableMtuPathCache) {
        return;
    }

    const auto confirmedSize =
        state.currentDatagramPayloadSize == 0 ? initialDatagramPayloadSize()
                                              : state.currentDatagramPayloadSize;
    if (options_.sharedMtuPathCache) {
        options_.sharedMtuPathCache->recordBlackhole(
            peerAddress,
            confirmedSize,
            state.mtuProbeCooldownUntil,
            state.mtuProbeBlackholeCount);
        return;
    }

    auto& entry = mtuPathCache_[makeAddressKey(peerAddress)];
    entry.confirmedDatagramPayloadSize =
        std::max(entry.confirmedDatagramPayloadSize, confirmedSize);
    if (state.mtuProbeCooldownUntil != Clock::time_point{} &&
        (entry.cooldownUntil == Clock::time_point{} ||
         entry.cooldownUntil < state.mtuProbeCooldownUntil)) {
        entry.cooldownUntil = state.mtuProbeCooldownUntil;
    }
    entry.blackholeCount = std::max(entry.blackholeCount, state.mtuProbeBlackholeCount);
}

void KcpTransport::recordMtuPathFailureLocked(const InetAddress& peerAddress,
                                              const SessionFlowState& state,
                                              std::size_t safeDatagramPayloadSize) {
    if (!options_.enableMtuProbing || !options_.enableMtuPathCache) {
        return;
    }

    const auto cachedSize = std::clamp(safeDatagramPayloadSize,
                                       initialDatagramPayloadSize(),
                                       options_.maxDatagramPayloadSize);
    if (options_.sharedMtuPathCache) {
        options_.sharedMtuPathCache->recordFailure(
            peerAddress,
            cachedSize,
            state.mtuProbeCooldownUntil,
            state.mtuProbeBlackholeCount);
        return;
    }

    auto& entry = mtuPathCache_[makeAddressKey(peerAddress)];
    entry.confirmedDatagramPayloadSize = cachedSize;
    if (state.mtuProbeCooldownUntil != Clock::time_point{} &&
        (entry.cooldownUntil == Clock::time_point{} ||
         entry.cooldownUntil < state.mtuProbeCooldownUntil)) {
        entry.cooldownUntil = state.mtuProbeCooldownUntil;
    }
    entry.blackholeCount = std::max(entry.blackholeCount, state.mtuProbeBlackholeCount);
}

std::optional<transport::PathMtuCacheEntry> KcpTransport::findMtuPathCacheEntryLocked(
    const InetAddress& peerAddress) {
    if (options_.sharedMtuPathCache) {
        return options_.sharedMtuPathCache->find(peerAddress);
    }

    const auto it = mtuPathCache_.find(makeAddressKey(peerAddress));
    if (it == mtuPathCache_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void KcpTransport::clearMtuPathCacheCooldownLocked(const InetAddress& peerAddress) {
    if (options_.sharedMtuPathCache) {
        options_.sharedMtuPathCache->clearCooldown(peerAddress);
        return;
    }

    const auto it = mtuPathCache_.find(makeAddressKey(peerAddress));
    if (it == mtuPathCache_.end()) {
        return;
    }
    it->second.cooldownUntil = {};
    it->second.blackholeCount = 0;
}

std::optional<transport::TransportSessionId> KcpTransport::decodeQuotedKcpSessionId(
    std::string_view quotedUdpPayloadPrefix) {
    if (quotedUdpPayloadPrefix.size() < kQuotedKcpSessionIdPrefixSize) {
        return std::nullopt;
    }

    if (readUint16(quotedUdpPayloadPrefix, 0) != codec::kKcpFrameMagic) {
        return std::nullopt;
    }
    const auto version = static_cast<std::uint8_t>(
        static_cast<unsigned char>(quotedUdpPayloadPrefix[2]));
    if (version != codec::kKcpFrameVersion) {
        return std::nullopt;
    }

    return static_cast<transport::TransportSessionId>(
        readUint64(quotedUdpPayloadPrefix, 4));
}

bool KcpTransport::authenticatePathMtuFailureLocked(
    const udp::PathMtuFailure& failure,
    transport::TransportSessionId sessionId) const {
    if (!options_.enablePathMtuSignalAuthentication) {
        return true;
    }
    if (failure.source != udp::PathMtuSignalSource::kRawIcmp) {
        return true;
    }

    const auto quotedSessionId =
        decodeQuotedKcpSessionId(failure.quotedUdpPayloadPrefix);
    return quotedSessionId.has_value() && *quotedSessionId == sessionId;
}

void KcpTransport::applyPathMtuFailureSignal(udp::PathMtuFailure failure,
                                             bool authenticate) {
    if (!started_.load(std::memory_order_acquire) || !options_.enableMtuProbing) {
        return;
    }

    std::scoped_lock lock(mutex_);
    const auto sessionIt = sessionByAddr_.find(makeAddressKey(failure.peerAddr));
    if (sessionIt == sessionByAddr_.end()) {
        return;
    }

    auto* state = getSessionStateLocked(sessionIt->second);
    if (!state) {
        return;
    }

    if (authenticate &&
        !authenticatePathMtuFailureLocked(failure, sessionIt->second)) {
        return;
    }

    applyPathMtuFailureLocked(
        *state,
        failure.peerAddr,
        failure.failedDatagramPayloadSize,
        failure.suggestedDatagramPayloadSize,
        Clock::now());
}

std::size_t KcpTransport::safeDatagramPayloadSizeAfterMtuFailure(
    std::size_t failedDatagramPayloadSize,
    std::size_t suggestedDatagramPayloadSize) const noexcept {
    if (failedDatagramPayloadSize == 0 && suggestedDatagramPayloadSize == 0) {
        return 0;
    }

    const auto minPayload = initialDatagramPayloadSize();
    std::size_t safePayload = suggestedDatagramPayloadSize;
    if (safePayload == 0) {
        safePayload = failedDatagramPayloadSize > options_.mtuProbeStepBytes
                          ? failedDatagramPayloadSize - options_.mtuProbeStepBytes
                          : minPayload;
    }

    if (failedDatagramPayloadSize != 0 &&
        safePayload >= failedDatagramPayloadSize &&
        failedDatagramPayloadSize > minPayload) {
        safePayload = failedDatagramPayloadSize > options_.mtuProbeStepBytes
                          ? failedDatagramPayloadSize - options_.mtuProbeStepBytes
                          : minPayload;
    }

    return std::clamp(safePayload, minPayload, options_.maxDatagramPayloadSize);
}

void KcpTransport::applyPathMtuFailureLocked(SessionFlowState& state,
                                             const InetAddress& peerAddress,
                                             std::size_t failedDatagramPayloadSize,
                                             std::size_t suggestedDatagramPayloadSize,
                                             std::chrono::steady_clock::time_point now) {
    if (!options_.enableMtuProbing) {
        return;
    }

    const auto safePayload = safeDatagramPayloadSizeAfterMtuFailure(
        failedDatagramPayloadSize,
        suggestedDatagramPayloadSize);
    if (safePayload == 0) {
        return;
    }

    const auto current = effectiveDatagramPayloadSize(state);
    if (safePayload < current) {
        state.currentDatagramPayloadSize = safePayload;
    }

    if (state.mtuProbeInFlight &&
        (failedDatagramPayloadSize == 0 ||
         state.mtuProbeTargetDatagramPayloadSize >= failedDatagramPayloadSize ||
         state.mtuProbeTargetDatagramPayloadSize > safePayload)) {
        state.mtuProbeTargetDatagramPayloadSize = 0;
        state.mtuProbeRetryCount = 0;
        state.mtuProbeWirePacket.clear();
        state.mtuProbeInFlight = false;
    }

    ++state.mtuProbeBlackholeCount;
    state.lastMtuProbeAt = now;
    state.mtuProbeCooldownUntil = now + options_.mtuProbeBlackholeCooldown;
    recordMtuPathFailureLocked(peerAddress, state, safePayload);
}

std::size_t KcpTransport::singleFramePayloadLimit(std::size_t datagramPayloadSize) noexcept {
    return datagramPayloadSize - codec::kKcpFrameHeaderSize;
}

std::size_t KcpTransport::fragmentPayloadLimit(std::size_t datagramPayloadSize) noexcept {
    return datagramPayloadSize - codec::kKcpFrameHeaderSize - kFragmentHeaderSize;
}

std::size_t KcpTransport::effectiveCongestionWindow(SessionFlowState& state) const noexcept {
    if (!options_.enableCongestionWindow) {
        return std::numeric_limits<std::size_t>::max();
    }
    if (state.congestionWindow == 0) {
        state.congestionWindow = options_.initialCongestionWindow;
    }
    return state.congestionWindow;
}

void KcpTransport::onPacketsAckedLocked(SessionFlowState& state, std::size_t ackedPackets) const noexcept {
    if (!options_.enableCongestionWindow || ackedPackets == 0) {
        return;
    }

    auto window = effectiveCongestionWindow(state);
    if (window >= options_.maxCongestionWindow) {
        state.congestionWindow = options_.maxCongestionWindow;
        return;
    }

    if (state.congestionSlowStartThreshold == 0 ||
        window < state.congestionSlowStartThreshold) {
        window = std::min(options_.maxCongestionWindow, window + ackedPackets);
        state.congestionWindow = window;
        return;
    }

    state.congestionAckCount += ackedPackets;
    while (state.congestionAckCount >= window &&
           window < options_.maxCongestionWindow) {
        state.congestionAckCount -= window;
        ++window;
    }
    state.congestionWindow = std::min(window, options_.maxCongestionWindow);
}

void KcpTransport::onRetransmissionTimeoutLocked(SessionFlowState& state) const noexcept {
    if (!options_.enableCongestionWindow) {
        return;
    }

    const auto window = effectiveCongestionWindow(state);
    state.congestionSlowStartThreshold =
        std::max(options_.minCongestionWindow, window / 2);
    state.congestionWindow = options_.minCongestionWindow;
    state.congestionAckCount = 0;
}

void KcpTransport::queueOrSendPacketLocked(SessionFlowState& state,
                                           OutboundPacket packet,
                                           std::chrono::steady_clock::time_point now,
                                           std::vector<std::string>& toSend) {
    const auto window = effectiveCongestionWindow(state);
    if (options_.enableCongestionWindow &&
        (!state.sendQueue.empty() || state.inFlight.size() >= window)) {
        state.sendQueue.push_back(std::move(packet));
        return;
    }

    packet.lastSendAt = now;
    appendInitialWireCopies(packet, toSend);
    appendXorParityIfReadyLocked(state, packet, toSend);
    state.inFlight[packet.seq] = std::move(packet);
}

void KcpTransport::appendInitialWireCopies(const OutboundPacket& packet,
                                           std::vector<std::string>& toSend) const {
    toSend.push_back(packet.wirePacket);
    if (!options_.enableRedundantCopies) {
        return;
    }
    for (std::size_t i = 0; i < options_.redundantCopyCount; ++i) {
        toSend.push_back(packet.wirePacket);
    }
}

void KcpTransport::appendXorParityIfReadyLocked(SessionFlowState& state,
                                                const OutboundPacket& packet,
                                                std::vector<std::string>& toSend) {
    if (!options_.enableXorParityRecovery ||
        (packet.flags & codec::kKcpFrameFlagData) == 0 ||
        packet.sessionId == transport::kInvalidTransportSessionId) {
        return;
    }

    if (!state.xorParitySendGroup.empty() &&
        packet.seq != state.xorParitySendGroup.back().seq + 1) {
        state.xorParitySendGroup.clear();
    }

    state.xorParitySendGroup.push_back(
        ParityPacket{packet.seq, packet.flags, packet.payload});
    if (state.xorParitySendGroup.size() < options_.xorParityGroupSize) {
        return;
    }

    auto parityPayload = encodeXorParityPayload(state.xorParitySendGroup);
    state.xorParitySendGroup.clear();
    if (parityPayload.empty()) {
        return;
    }

    codec::KcpFrame frame;
    frame.sessionId = packet.sessionId;
    frame.flags = codec::kKcpFrameFlagXorParity;
    frame.payload = std::move(parityPayload);

    auto wire = codec::encodeFrame(frame);
    if (wire.empty() || wire.size() > effectiveDatagramPayloadSize(state)) {
        return;
    }

    toSend.push_back(std::move(wire));
}

void KcpTransport::drainSendQueueLocked(SessionFlowState& state,
                                        std::chrono::steady_clock::time_point now,
                                        std::vector<std::string>& toSend) {
    while (!state.sendQueue.empty()) {
        const auto window = effectiveCongestionWindow(state);
        if (state.inFlight.size() >= window) {
            return;
        }

        auto packet = std::move(state.sendQueue.front());
        state.sendQueue.pop_front();
        packet.lastSendAt = now;
        packet.rto = options_.initialRto;
        packet.retryCount = 0;
        appendInitialWireCopies(packet, toSend);
        appendXorParityIfReadyLocked(state, packet, toSend);
        state.inFlight[packet.seq] = std::move(packet);
    }
}

std::string KcpTransport::makeAddressKey(const InetAddress& addr) {
    return transport::PathMtuCache::keyForPeer(addr);
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
    std::vector<std::string> queuedWires;
    bool needAck = false;
    bool needMtuProbeAck = false;
    std::uint32_t ackSeq = 0;
    std::string selectiveAckPayload;
    std::string mtuProbeAckPayload;
    {
        std::scoped_lock lock(mutex_);
        auto* state = getSessionStateLocked(session->sessionId());
        if (!state) {
            return;
        }

        const auto ackedPackets = applyAck(*state, frame.ack, frame.flags, frame.payload);
        if (ackedPackets != 0) {
            onPacketsAckedLocked(*state, ackedPackets);
            drainSendQueueLocked(*state, Clock::now(), queuedWires);
        }

        if ((frame.flags & codec::kKcpFrameFlagMtuProbe) != 0) {
            if ((frame.flags & codec::kKcpFrameFlagAck) != 0) {
                applyMtuProbeAck(*state, peerAddr, frame.payload);
            } else {
                std::size_t probeTarget = 0;
                if (decodeMtuProbePayload(frame.payload, probeTarget)) {
                    needMtuProbeAck = true;
                    ackSeq = state->lastRecvSeq;
                    mtuProbeAckPayload = encodeMtuProbeAckPayload(probeTarget);
                }
            }
        } else if ((frame.flags & codec::kKcpFrameFlagXorParity) != 0) {
            if (processXorParityPayload(*state, frame.payload, deliverPayloads)) {
                needAck = true;
                ackSeq = state->lastRecvSeq;
                selectiveAckPayload = encodeSelectiveAckPayload(*state);
            }
        } else if ((frame.flags & codec::kKcpFrameFlagData) != 0) {
            processDataPayload(*state, frame, deliverPayloads);
            needAck = true;
            ackSeq = state->lastRecvSeq;
            selectiveAckPayload = encodeSelectiveAckPayload(*state);
        }

        if (frame.flags == codec::kKcpFrameFlagReset) {
            state->inFlight.clear();
            state->sendQueue.clear();
            state->pendingPackets.clear();
            state->fragmentAssemblies.clear();
            state->congestionWindow = 0;
            state->congestionSlowStartThreshold = 0;
            state->congestionAckCount = 0;
            state->mtuProbeTargetDatagramPayloadSize = 0;
            state->mtuProbeRetryCount = 0;
            state->lastMtuProbeAt = {};
            state->mtuProbeCooldownUntil = {};
            state->mtuProbeBlackholeCount = 0;
            state->mtuProbeWirePacket.clear();
            state->mtuProbeInFlight = false;
            state->mtuProbeDisabled = false;
            state->xorParitySendGroup.clear();
            state->xorParityHistory.clear();
            state->nextRecvSeq = 1;
            state->lastRecvSeq = 0;
            ackSeq = state->lastRecvSeq;
            selectiveAckPayload.clear();
        }
    }

    if (needAck || frame.flags == codec::kKcpFrameFlagReset) {
        sendAck(session, ackSeq, std::move(selectiveAckPayload));
    }
    if (needMtuProbeAck) {
        sendMtuProbeAck(session, ackSeq, std::move(mtuProbeAckPayload));
    }
    for (const auto& wire : queuedWires) {
        sendWirePacket(session, wire);
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
                auto [stateIt, inserted] = sessionStates_.try_emplace(sessionId, SessionFlowState{});
                if (inserted) {
                    seedMtuStateFromPathCacheLocked(stateIt->second, peerAddr, Clock::now());
                }
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
    auto [stateIt, inserted] = sessionStates_.try_emplace(sessionId, SessionFlowState{});
    if (inserted) {
        seedMtuStateFromPathCacheLocked(stateIt->second, peerAddr, Clock::now());
    }
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

void KcpTransport::sendAck(const std::shared_ptr<KcpSession>& session,
                           std::uint32_t ackSeq,
                           std::string selectiveAckPayload) {
    if (!session) {
        return;
    }

    codec::KcpFrame frame;
    frame.sessionId = session->sessionId();
    frame.flags = codec::kKcpFrameFlagAck;
    if (!selectiveAckPayload.empty()) {
        frame.flags |= codec::kKcpFrameFlagSelectiveAck;
    }
    frame.ack = ackSeq;
    frame.payload = std::move(selectiveAckPayload);

    const auto wire = codec::encodeFrame(frame);
    if (!wire.empty()) {
        sendWirePacket(session, wire);
    }
}

void KcpTransport::sendMtuProbeAck(const std::shared_ptr<KcpSession>& session,
                                   std::uint32_t ackSeq,
                                   std::string ackPayload) {
    if (!session || ackPayload.empty()) {
        return;
    }

    codec::KcpFrame frame;
    frame.sessionId = session->sessionId();
    frame.flags = codec::kKcpFrameFlagAck | codec::kKcpFrameFlagMtuProbe;
    frame.ack = ackSeq;
    frame.payload = std::move(ackPayload);

    const auto wire = codec::encodeFrame(frame);
    if (!wire.empty()) {
        sendWirePacket(session, wire);
    }
}

std::size_t KcpTransport::applyAck(SessionFlowState& state,
                                   std::uint32_t ackSeq,
                                   std::uint16_t flags,
                                   std::string_view ackPayload) {
    std::size_t ackedPackets = 0;
    if (ackSeq != 0) {
        for (auto it = state.inFlight.begin(); it != state.inFlight.end();) {
            if (it->first <= ackSeq) {
                ++ackedPackets;
                it = state.inFlight.erase(it);
            } else {
                ++it;
            }
        }
    }

    if ((flags & codec::kKcpFrameFlagSelectiveAck) == 0) {
        return ackedPackets;
    }

    for (const auto seq : decodeSelectiveAckPayload(ackPayload)) {
        if (seq <= ackSeq) {
            continue;
        }
        if (auto it = state.inFlight.find(seq); it != state.inFlight.end()) {
            ++ackedPackets;
            it = state.inFlight.erase(it);
        }
    }
    return ackedPackets;
}

void KcpTransport::applyMtuProbeAck(SessionFlowState& state,
                                    const InetAddress& peerAddress,
                                    std::string_view ackPayload) {
    if (!state.mtuProbeInFlight) {
        return;
    }

    std::size_t acknowledgedTarget = 0;
    if (!decodeMtuProbeAckPayload(ackPayload, acknowledgedTarget)) {
        return;
    }
    if (acknowledgedTarget != state.mtuProbeTargetDatagramPayloadSize) {
        return;
    }
    if (acknowledgedTarget > options_.maxDatagramPayloadSize) {
        return;
    }

    const auto current = effectiveDatagramPayloadSize(state);
    if (acknowledgedTarget > current) {
        state.currentDatagramPayloadSize = acknowledgedTarget;
    }
    recordMtuPathSuccessLocked(peerAddress, acknowledgedTarget);
    state.mtuProbeTargetDatagramPayloadSize = 0;
    state.mtuProbeRetryCount = 0;
    state.mtuProbeCooldownUntil = {};
    state.mtuProbeBlackholeCount = 0;
    state.mtuProbeWirePacket.clear();
    state.mtuProbeInFlight = false;
}

void KcpTransport::rememberXorParityPacketLocked(SessionFlowState& state,
                                                 std::uint32_t seq,
                                                 std::uint16_t flags,
                                                 const std::string& payload) const {
    if (!options_.enableXorParityRecovery) {
        return;
    }

    state.xorParityHistory[seq] = PendingPacket{flags, payload};
    if (state.nextRecvSeq <= kMaxXorParityHistoryPackets) {
        return;
    }

    const auto pruneBefore =
        state.nextRecvSeq - static_cast<std::uint32_t>(kMaxXorParityHistoryPackets);
    for (auto it = state.xorParityHistory.begin(); it != state.xorParityHistory.end();) {
        if (it->first < pruneBefore) {
            it = state.xorParityHistory.erase(it);
        } else {
            ++it;
        }
    }
}

bool KcpTransport::processXorParityPayload(SessionFlowState& state,
                                           std::string_view payload,
                                           std::vector<std::string>& deliverPayloads) {
    if (!options_.enableXorParityRecovery) {
        return false;
    }

    XorParityPayload group;
    if (!decodeXorParityPayload(payload, group)) {
        return false;
    }

    std::size_t missingIndex = group.flags.size();
    auto recovered = group.parityPayload;
    for (std::size_t index = 0; index < group.flags.size(); ++index) {
        const auto seq = group.baseSeq + static_cast<std::uint32_t>(index);
        const auto it = state.xorParityHistory.find(seq);
        if (it == state.xorParityHistory.end()) {
            if (missingIndex != group.flags.size()) {
                return false;
            }
            missingIndex = index;
            continue;
        }

        if (it->second.flags != group.flags[index] ||
            it->second.payload.size() != group.payloadSizes[index] ||
            it->second.payload.size() > recovered.size()) {
            return false;
        }

        for (std::size_t byteIndex = 0; byteIndex < it->second.payload.size(); ++byteIndex) {
            recovered[byteIndex] =
                static_cast<char>(static_cast<unsigned char>(recovered[byteIndex]) ^
                                  static_cast<unsigned char>(it->second.payload[byteIndex]));
        }
    }

    if (missingIndex == group.flags.size()) {
        return false;
    }

    const auto missingSeq = group.baseSeq + static_cast<std::uint32_t>(missingIndex);
    const auto missingFlags = group.flags[missingIndex];
    if (missingSeq < state.nextRecvSeq ||
        (missingFlags & codec::kKcpFrameFlagData) == 0) {
        return false;
    }

    recovered.resize(group.payloadSizes[missingIndex]);
    codec::KcpFrame recoveredFrame;
    recoveredFrame.seq = missingSeq;
    recoveredFrame.flags = missingFlags;
    recoveredFrame.payload = std::move(recovered);
    processDataPayload(state, recoveredFrame, deliverPayloads);
    return true;
}

void KcpTransport::processDataPayload(SessionFlowState& state,
                                    const codec::KcpFrame& frame,
                                    std::vector<std::string>& deliverPayloads) {
    rememberXorParityPacketLocked(state, frame.seq, frame.flags, frame.payload);

    if (frame.seq < state.nextRecvSeq) {
        return;
    }

    if (frame.seq == state.nextRecvSeq) {
        processReliablePayload(state, frame.flags, frame.payload, deliverPayloads);
        state.lastRecvSeq = frame.seq;
        ++state.nextRecvSeq;

        auto it = state.pendingPackets.find(state.nextRecvSeq);
        while (it != state.pendingPackets.end()) {
            auto pending = std::move(it->second);
            state.lastRecvSeq = it->first;
            ++state.nextRecvSeq;
            state.pendingPackets.erase(it);
            processReliablePayload(
                state,
                pending.flags,
                std::move(pending.payload),
                deliverPayloads);
            it = state.pendingPackets.find(state.nextRecvSeq);
        }
        return;
    }

    state.pendingPackets[frame.seq] = PendingPacket{frame.flags, frame.payload};
}

void KcpTransport::processReliablePayload(SessionFlowState& state,
                                          std::uint16_t flags,
                                          std::string payload,
                                          std::vector<std::string>& deliverPayloads) {
    if ((flags & codec::kKcpFrameFlagFragment) != 0) {
        processFragmentPayload(state, std::move(payload), deliverPayloads);
        return;
    }
    if (payload.size() > options_.maxApplicationPayloadSize) {
        return;
    }
    deliverPayloads.push_back(std::move(payload));
}

void KcpTransport::processFragmentPayload(SessionFlowState& state,
                                          std::string payload,
                                          std::vector<std::string>& deliverPayloads) {
    std::uint32_t messageId = 0;
    std::uint16_t fragmentIndex = 0;
    std::uint16_t fragmentCount = 0;
    std::string_view fragmentPayload;
    if (!decodeFragmentPayload(
            payload,
            messageId,
            fragmentIndex,
            fragmentCount,
            fragmentPayload)) {
        return;
    }

    if (fragmentCount == 1) {
        if (fragmentPayload.size() > options_.maxApplicationPayloadSize) {
            return;
        }
        deliverPayloads.emplace_back(fragmentPayload);
        return;
    }

    auto& assembly = state.fragmentAssemblies[messageId];
    if (assembly.fragmentCount != fragmentCount) {
        assembly = FragmentAssembly{};
        assembly.fragmentCount = fragmentCount;
        assembly.fragments.resize(fragmentCount);
        assembly.received.resize(fragmentCount, false);
    }

    if (assembly.received[fragmentIndex]) {
        return;
    }

    if (assembly.bytes + fragmentPayload.size() > options_.maxApplicationPayloadSize) {
        state.fragmentAssemblies.erase(messageId);
        return;
    }

    assembly.fragments[fragmentIndex] = std::string(fragmentPayload);
    assembly.received[fragmentIndex] = true;
    ++assembly.receivedCount;
    assembly.bytes += fragmentPayload.size();

    if (assembly.receivedCount != assembly.fragmentCount) {
        return;
    }

    std::string assembled;
    assembled.reserve(assembly.bytes);
    for (const auto& fragment : assembly.fragments) {
        assembled.append(fragment);
    }
    state.fragmentAssemblies.erase(messageId);
    deliverPayloads.push_back(std::move(assembled));
}

std::string KcpTransport::encodeFragmentPayload(std::uint32_t messageId,
                                                std::uint16_t fragmentIndex,
                                                std::uint16_t fragmentCount,
                                                std::string_view payload,
                                                std::size_t maxFragmentPayloadSize) {
    if (fragmentCount == 0 || fragmentIndex >= fragmentCount ||
        payload.size() > maxFragmentPayloadSize) {
        return {};
    }

    std::string out;
    out.reserve(kFragmentHeaderSize + payload.size());
    out.push_back(static_cast<char>((messageId >> 24) & 0xFF));
    out.push_back(static_cast<char>((messageId >> 16) & 0xFF));
    out.push_back(static_cast<char>((messageId >> 8) & 0xFF));
    out.push_back(static_cast<char>(messageId & 0xFF));
    out.push_back(static_cast<char>((fragmentIndex >> 8) & 0xFF));
    out.push_back(static_cast<char>(fragmentIndex & 0xFF));
    out.push_back(static_cast<char>((fragmentCount >> 8) & 0xFF));
    out.push_back(static_cast<char>(fragmentCount & 0xFF));
    out.append(payload);
    return out;
}

bool KcpTransport::decodeFragmentPayload(std::string_view payload,
                                         std::uint32_t& messageId,
                                         std::uint16_t& fragmentIndex,
                                         std::uint16_t& fragmentCount,
                                         std::string_view& fragmentPayload) {
    if (payload.size() < kFragmentHeaderSize) {
        return false;
    }

    const auto byte = [&](std::size_t index) {
        return static_cast<unsigned char>(payload[index]);
    };

    messageId = (static_cast<std::uint32_t>(byte(0)) << 24) |
                (static_cast<std::uint32_t>(byte(1)) << 16) |
                (static_cast<std::uint32_t>(byte(2)) << 8) |
                static_cast<std::uint32_t>(byte(3));
    fragmentIndex = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(byte(4)) << 8) |
        static_cast<std::uint16_t>(byte(5)));
    fragmentCount = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(byte(6)) << 8) |
        static_cast<std::uint16_t>(byte(7)));
    if (messageId == 0 || fragmentCount == 0 || fragmentIndex >= fragmentCount) {
        return false;
    }

    fragmentPayload = payload.substr(kFragmentHeaderSize);
    return true;
}

std::string KcpTransport::encodeSelectiveAckPayload(const SessionFlowState& state) {
    if (state.pendingPackets.empty()) {
        return {};
    }

    std::vector<std::uint32_t> sequences;
    sequences.reserve(std::min(state.pendingPackets.size(), kMaxSelectiveAckEntries));
    for (const auto& [seq, packet] : state.pendingPackets) {
        (void)packet;
        if (seq > state.lastRecvSeq) {
            sequences.push_back(seq);
        }
    }
    if (sequences.empty()) {
        return {};
    }

    std::sort(sequences.begin(), sequences.end());
    if (sequences.size() > kMaxSelectiveAckEntries) {
        sequences.resize(kMaxSelectiveAckEntries);
    }

    std::string out;
    out.reserve(6 + sequences.size() * sizeof(std::uint32_t));
    out.append("SAK1", 4);
    const auto count = static_cast<std::uint16_t>(sequences.size());
    out.push_back(static_cast<char>((count >> 8) & 0xFF));
    out.push_back(static_cast<char>(count & 0xFF));
    for (const auto seq : sequences) {
        out.push_back(static_cast<char>((seq >> 24) & 0xFF));
        out.push_back(static_cast<char>((seq >> 16) & 0xFF));
        out.push_back(static_cast<char>((seq >> 8) & 0xFF));
        out.push_back(static_cast<char>(seq & 0xFF));
    }
    return out;
}

std::vector<std::uint32_t> KcpTransport::decodeSelectiveAckPayload(std::string_view payload) {
    if (payload.size() < 6 || payload.substr(0, 4) != "SAK1") {
        return {};
    }

    const auto byte = [&](std::size_t index) {
        return static_cast<unsigned char>(payload[index]);
    };

    const auto count = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(byte(4)) << 8) |
        static_cast<std::uint16_t>(byte(5)));
    if (count == 0 || count > kMaxSelectiveAckEntries ||
        payload.size() != 6 + static_cast<std::size_t>(count) * sizeof(std::uint32_t)) {
        return {};
    }

    std::vector<std::uint32_t> sequences;
    sequences.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        const auto offset = 6 + static_cast<std::size_t>(i) * sizeof(std::uint32_t);
        const auto seq = (static_cast<std::uint32_t>(byte(offset)) << 24) |
                         (static_cast<std::uint32_t>(byte(offset + 1)) << 16) |
                         (static_cast<std::uint32_t>(byte(offset + 2)) << 8) |
                         static_cast<std::uint32_t>(byte(offset + 3));
        if (seq != 0) {
            sequences.push_back(seq);
        }
    }
    return sequences;
}

std::string KcpTransport::encodeXorParityPayload(const std::vector<ParityPacket>& packets) {
    if (packets.size() < 2 || packets.size() > kMaxXorParityGroupSize) {
        return {};
    }

    const auto baseSeq = packets.front().seq;
    std::size_t maxPayloadSize = 0;
    for (std::size_t index = 0; index < packets.size(); ++index) {
        const auto expectedSeq = baseSeq + static_cast<std::uint32_t>(index);
        if (packets[index].seq != expectedSeq ||
            (packets[index].flags & codec::kKcpFrameFlagData) == 0 ||
            packets[index].payload.size() > codec::kKcpMaxPayloadSize) {
            return {};
        }
        maxPayloadSize = std::max(maxPayloadSize, packets[index].payload.size());
    }

    std::string parity(maxPayloadSize, '\0');
    for (const auto& packet : packets) {
        for (std::size_t index = 0; index < packet.payload.size(); ++index) {
            parity[index] =
                static_cast<char>(static_cast<unsigned char>(parity[index]) ^
                                  static_cast<unsigned char>(packet.payload[index]));
        }
    }

    const auto metadataSize = 12 + packets.size() * 4;
    if (metadataSize + parity.size() > codec::kKcpMaxPayloadSize) {
        return {};
    }

    std::string out;
    out.reserve(metadataSize + parity.size());
    out.append(kXorParityMagic.data(), kXorParityMagic.size());
    appendUint32(out, baseSeq);
    appendUint16(out, static_cast<std::uint16_t>(packets.size()));
    appendUint16(out, static_cast<std::uint16_t>(maxPayloadSize));
    for (const auto& packet : packets) {
        appendUint16(out, packet.flags);
        appendUint16(out, static_cast<std::uint16_t>(packet.payload.size()));
    }
    out.append(parity);
    return out;
}

bool KcpTransport::decodeXorParityPayload(std::string_view payload, XorParityPayload& out) {
    if (payload.size() < 12 || payload.substr(0, kXorParityMagic.size()) != kXorParityMagic) {
        return false;
    }

    const auto baseSeq = readUint32(payload, kXorParityMagic.size());
    const auto count = readUint16(payload, kXorParityMagic.size() + sizeof(std::uint32_t));
    const auto maxPayloadSize = readUint16(
        payload,
        kXorParityMagic.size() + sizeof(std::uint32_t) + sizeof(std::uint16_t));
    if (baseSeq == 0 || count < 2 || count > kMaxXorParityGroupSize) {
        return false;
    }

    const auto metadataSize = 12 + static_cast<std::size_t>(count) * 4;
    if (payload.size() != metadataSize + maxPayloadSize) {
        return false;
    }

    XorParityPayload decoded;
    decoded.baseSeq = baseSeq;
    decoded.flags.reserve(count);
    decoded.payloadSizes.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        const auto offset = 12 + static_cast<std::size_t>(index) * 4;
        const auto flags = readUint16(payload, offset);
        const auto payloadSize = readUint16(payload, offset + sizeof(std::uint16_t));
        if ((flags & codec::kKcpFrameFlagData) == 0 ||
            payloadSize > maxPayloadSize) {
            return false;
        }
        decoded.flags.push_back(flags);
        decoded.payloadSizes.push_back(payloadSize);
    }
    decoded.parityPayload = std::string(payload.substr(metadataSize));
    out = std::move(decoded);
    return true;
}

std::string KcpTransport::encodeMtuProbePayload(std::size_t targetDatagramPayloadSize) {
    if (targetDatagramPayloadSize > std::numeric_limits<std::uint32_t>::max()) {
        return {};
    }

    std::string out;
    out.reserve(kMtuProbeControlPayloadSize);
    out.append(kMtuProbeMagic.data(), kMtuProbeMagic.size());
    appendUint32(out, static_cast<std::uint32_t>(targetDatagramPayloadSize));
    return out;
}

std::string KcpTransport::encodeMtuProbeAckPayload(std::size_t targetDatagramPayloadSize) {
    if (targetDatagramPayloadSize > std::numeric_limits<std::uint32_t>::max()) {
        return {};
    }

    std::string out;
    out.reserve(kMtuProbeControlPayloadSize);
    out.append(kMtuProbeAckMagic.data(), kMtuProbeAckMagic.size());
    appendUint32(out, static_cast<std::uint32_t>(targetDatagramPayloadSize));
    return out;
}

bool KcpTransport::decodeMtuProbePayload(std::string_view payload,
                                         std::size_t& targetDatagramPayloadSize) {
    if (payload.size() < kMtuProbeControlPayloadSize ||
        payload.substr(0, kMtuProbeMagic.size()) != kMtuProbeMagic) {
        return false;
    }
    const auto target = readUint32(payload, kMtuProbeMagic.size());
    if (target < codec::kKcpFrameHeaderSize + kMtuProbeControlPayloadSize ||
        target > codec::kKcpFrameHeaderSize + codec::kKcpMaxPayloadSize ||
        payload.size() + codec::kKcpFrameHeaderSize != target) {
        return false;
    }
    targetDatagramPayloadSize = target;
    return true;
}

bool KcpTransport::decodeMtuProbeAckPayload(std::string_view payload,
                                            std::size_t& targetDatagramPayloadSize) {
    if (payload.size() != kMtuProbeControlPayloadSize ||
        payload.substr(0, kMtuProbeAckMagic.size()) != kMtuProbeAckMagic) {
        return false;
    }
    const auto target = readUint32(payload, kMtuProbeAckMagic.size());
    if (target < codec::kKcpFrameHeaderSize + kMtuProbeControlPayloadSize ||
        target > codec::kKcpFrameHeaderSize + codec::kKcpMaxPayloadSize) {
        return false;
    }
    targetDatagramPayloadSize = target;
    return true;
}

std::string KcpTransport::makeMtuProbeFramePayload(std::size_t targetDatagramPayloadSize) {
    if (targetDatagramPayloadSize < codec::kKcpFrameHeaderSize + kMtuProbeControlPayloadSize ||
        targetDatagramPayloadSize > codec::kKcpFrameHeaderSize + codec::kKcpMaxPayloadSize) {
        return {};
    }

    auto payload = encodeMtuProbePayload(targetDatagramPayloadSize);
    const auto targetPayloadSize = targetDatagramPayloadSize - codec::kKcpFrameHeaderSize;
    if (payload.empty() || payload.size() > targetPayloadSize) {
        return {};
    }
    payload.resize(targetPayloadSize, '\0');
    return payload;
}

void KcpTransport::maybeQueueMtuProbeLocked(transport::TransportSessionId sessionId,
                                            SessionFlowState& state,
                                            const InetAddress& peerAddress,
                                            std::chrono::steady_clock::time_point now,
                                            std::vector<std::pair<std::string, InetAddress>>& toSend) {
    if (!options_.enableMtuProbing || state.mtuProbeDisabled) {
        return;
    }

    seedMtuStateFromPathCacheLocked(state, peerAddress, now);

    if (state.mtuProbeCooldownUntil != Clock::time_point{} &&
        now < state.mtuProbeCooldownUntil) {
        return;
    }
    if (state.mtuProbeCooldownUntil != Clock::time_point{} &&
        now >= state.mtuProbeCooldownUntil) {
        state.mtuProbeCooldownUntil = {};
    }

    const auto current = effectiveDatagramPayloadSize(state);
    if (current >= options_.maxDatagramPayloadSize) {
        return;
    }

    if (state.mtuProbeInFlight) {
        if (now - state.lastMtuProbeAt < options_.maxRto) {
            return;
        }
        if (state.mtuProbeRetryCount >= options_.mtuProbeMaxRetries) {
            state.mtuProbeTargetDatagramPayloadSize = 0;
            state.mtuProbeRetryCount = 0;
            state.mtuProbeWirePacket.clear();
            state.mtuProbeInFlight = false;
            ++state.mtuProbeBlackholeCount;
            state.lastMtuProbeAt = now;
            state.mtuProbeCooldownUntil = now + options_.mtuProbeBlackholeCooldown;
            recordMtuPathBlackholeLocked(peerAddress, state);
            return;
        }

        ++state.mtuProbeRetryCount;
        state.lastMtuProbeAt = now;
        if (!state.mtuProbeWirePacket.empty()) {
            toSend.emplace_back(state.mtuProbeWirePacket, peerAddress);
        }
        return;
    }

    if (now - state.lastMtuProbeAt < options_.mtuProbeInterval) {
        return;
    }

    const auto target = std::min(
        options_.maxDatagramPayloadSize,
        current + options_.mtuProbeStepBytes);
    if (target <= current) {
        return;
    }

    codec::KcpFrame frame;
    frame.sessionId = sessionId;
    frame.flags = codec::kKcpFrameFlagMtuProbe;
    frame.ack = state.lastRecvSeq;
    frame.payload = makeMtuProbeFramePayload(target);
    if (frame.payload.empty()) {
        state.mtuProbeDisabled = true;
        return;
    }

    auto wire = codec::encodeFrame(frame);
    if (wire.size() != target) {
        state.mtuProbeDisabled = true;
        return;
    }

    state.mtuProbeTargetDatagramPayloadSize = target;
    state.mtuProbeRetryCount = 0;
    state.lastMtuProbeAt = now;
    state.mtuProbeWirePacket = wire;
    state.mtuProbeInFlight = true;
    toSend.emplace_back(std::move(wire), peerAddress);
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
    std::weak_ptr<void> lifetime = lifetimeToken_;
    loop_->queueInLoop([lifetime, fn = std::forward<Fn>(fn)]() mutable {
        if (!lifetime.lock()) {
            return;
        }
        fn();
    });
}

void KcpTransport::startFlushTimer() {
    if (!loop_ || !loop_->isInLoopThread() || hasFlushTimer_) {
        return;
    }
    std::weak_ptr<void> lifetime = lifetimeToken_;
    flushTimerId_ = loop_->runEvery(kFlushInterval, [this, lifetime] {
        if (!lifetime.lock()) {
            return;
        }
        handleFlushTick();
    });
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
    if (!started_.load(std::memory_order_acquire) || !loop_ || !loop_->isInLoopThread()) {
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
            bool shouldClose = false;
            bool congestionReduced = false;
            for (auto& [seq, packet] : state.inFlight) {
                (void)seq;
                if (packet.retryCount >= options_.maxRetransmissions) {
                    toClose.push_back(sessionId);
                    shouldClose = true;
                    break;
                }

                if (now - packet.lastSendAt < packet.rto) {
                    continue;
                }

                if (!congestionReduced) {
                    onRetransmissionTimeoutLocked(state);
                    congestionReduced = true;
                }
                packet.retryCount += 1;
                packet.lastSendAt = now;
                packet.rto = std::min(packet.rto * 2, options_.maxRto);
                toResend.emplace_back(packet.wirePacket, peerAddress);
            }
            if (!shouldClose) {
                maybeQueueMtuProbeLocked(sessionId, state, peerAddress, now, toResend);
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
