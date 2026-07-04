#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/kcp/KcpCodec.h"
#include "mini/net/kcp/KcpSession.h"
#include "mini/net/kcp/KcpTransport.h"
#include "mini/net/transport/PathMtuCache.h"
#include "mini/net/transport/TransportTypes.h"

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

uint16_t allocatePort() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    assert(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    assert(::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    const auto port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

sockaddr_in loopbackAddress(uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    return addr;
}

class ImpairingUdpProxy {
public:
    ImpairingUdpProxy(uint16_t endpointAPort, uint16_t endpointBPort)
        : endpointAPort_(endpointAPort),
          endpointBPort_(endpointBPort),
          endpointA_(loopbackAddress(endpointAPort)),
          endpointB_(loopbackAddress(endpointBPort)) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
        assert(fd_ >= 0);

        sockaddr_in bindAddr = loopbackAddress(0);
        assert(::bind(fd_, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) == 0);

        socklen_t len = static_cast<socklen_t>(sizeof(bindAddr));
        assert(::getsockname(fd_, reinterpret_cast<sockaddr*>(&bindAddr), &len) == 0);
        proxyPort_ = ntohs(bindAddr.sin_port);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 5000;
        assert(::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    }

    ~ImpairingUdpProxy() {
        stop();
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    void start() {
        worker_ = std::thread([this] { run(); });
    }

    void stop() {
        stop_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    uint16_t proxyPort() const noexcept {
        return proxyPort_;
    }

    std::size_t droppedPackets() const noexcept {
        return droppedPackets_.load(std::memory_order_acquire);
    }

    std::size_t duplicatedPackets() const noexcept {
        return duplicatedPackets_.load(std::memory_order_acquire);
    }

    std::size_t reorderedPackets() const noexcept {
        return reorderedPackets_.load(std::memory_order_acquire);
    }

    std::size_t delayedPackets() const noexcept {
        return delayedPackets_.load(std::memory_order_acquire);
    }

private:
    struct ScheduledPacket {
        std::chrono::steady_clock::time_point dueAt;
        sockaddr_in target{};
        std::string payload;
    };

    void run() {
        while (!stop_.load(std::memory_order_acquire)) {
            drainDuePackets();

            char buffer[2048]{};
            sockaddr_in from{};
            socklen_t fromLen = static_cast<socklen_t>(sizeof(from));
            const auto n = ::recvfrom(fd_,
                                      buffer,
                                      sizeof(buffer),
                                      0,
                                      reinterpret_cast<sockaddr*>(&from),
                                      &fromLen);
            if (n < 0) {
                continue;
            }
            handlePacket(std::string(buffer, static_cast<std::size_t>(n)), from);
        }
        drainDuePackets();
    }

    void handlePacket(std::string payload, const sockaddr_in& from) {
        const auto sourcePort = ntohs(from.sin_port);
        if (sourcePort != endpointAPort_ && sourcePort != endpointBPort_) {
            return;
        }

        const bool fromA = sourcePort == endpointAPort_;
        const auto& target = fromA ? endpointB_ : endpointA_;

        mini::net::kcp::codec::KcpFrame frame;
        const bool decoded = mini::net::kcp::codec::decodeFrame(payload, frame);
        if (!decoded) {
            schedule(std::move(payload), target, 1ms);
            return;
        }

        const bool isData =
            (frame.flags & mini::net::kcp::codec::kKcpFrameFlagData) != 0;
        if (fromA && isData) {
            const auto seen = ++seenDataFromA_[frame.seq];
            if (frame.seq == 1 && seen == 1) {
                ++droppedPackets_;
                return;
            }
            if (frame.seq == 2 && seen == 1) {
                ++delayedPackets_;
                schedule(std::move(payload), target, 80ms);
                return;
            }
            if (frame.seq == 3 && seen == 1) {
                ++duplicatedPackets_;
                ++reorderedPackets_;
                delayedPackets_.fetch_add(2, std::memory_order_acq_rel);
                schedule(payload, target, 5ms);
                schedule(std::move(payload), target, 12ms);
                return;
            }
            if (frame.seq == 4 && seen == 1) {
                ++delayedPackets_;
                schedule(std::move(payload), target, 35ms);
                return;
            }
        }

        if (!fromA && isData) {
            const auto seen = ++seenDataFromB_[frame.seq];
            if (frame.seq == 1 && seen == 1) {
                ++delayedPackets_;
                schedule(std::move(payload), target, 20ms);
                return;
            }
        }

        schedule(std::move(payload), target, 1ms);
    }

    void schedule(std::string payload, const sockaddr_in& target, std::chrono::milliseconds delay) {
        scheduled_.push_back(ScheduledPacket{
            std::chrono::steady_clock::now() + delay,
            target,
            std::move(payload)});
    }

    void drainDuePackets() {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = scheduled_.begin(); it != scheduled_.end();) {
            if (it->dueAt > now) {
                ++it;
                continue;
            }

            const auto sent = ::sendto(fd_,
                                       it->payload.data(),
                                       it->payload.size(),
                                       0,
                                       reinterpret_cast<const sockaddr*>(&it->target),
                                       sizeof(it->target));
            assert(sent == static_cast<ssize_t>(it->payload.size()));
            it = scheduled_.erase(it);
        }
    }

    int fd_{-1};
    uint16_t endpointAPort_{0};
    uint16_t endpointBPort_{0};
    uint16_t proxyPort_{0};
    sockaddr_in endpointA_{};
    sockaddr_in endpointB_{};
    std::atomic<bool> stop_{false};
    std::thread worker_;
    std::vector<ScheduledPacket> scheduled_;
    std::unordered_map<std::uint32_t, std::size_t> seenDataFromA_;
    std::unordered_map<std::uint32_t, std::size_t> seenDataFromB_;
    std::atomic<std::size_t> droppedPackets_{0};
    std::atomic<std::size_t> duplicatedPackets_{0};
    std::atomic<std::size_t> reorderedPackets_{0};
    std::atomic<std::size_t> delayedPackets_{0};
};

class LongRunLossUdpProxy {
public:
    LongRunLossUdpProxy(uint16_t endpointAPort, uint16_t endpointBPort)
        : endpointAPort_(endpointAPort),
          endpointBPort_(endpointBPort),
          endpointA_(loopbackAddress(endpointAPort)),
          endpointB_(loopbackAddress(endpointBPort)) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
        assert(fd_ >= 0);

        sockaddr_in bindAddr = loopbackAddress(0);
        assert(::bind(fd_, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) == 0);

        socklen_t len = static_cast<socklen_t>(sizeof(bindAddr));
        assert(::getsockname(fd_, reinterpret_cast<sockaddr*>(&bindAddr), &len) == 0);
        proxyPort_ = ntohs(bindAddr.sin_port);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 5000;
        assert(::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    }

    ~LongRunLossUdpProxy() {
        stop();
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    void start() {
        worker_ = std::thread([this] { run(); });
    }

    void stop() {
        stop_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    uint16_t proxyPort() const noexcept {
        return proxyPort_;
    }

    std::size_t droppedPackets() const noexcept {
        return droppedPackets_.load(std::memory_order_acquire);
    }

    std::size_t duplicatedPackets() const noexcept {
        return duplicatedPackets_.load(std::memory_order_acquire);
    }

    std::size_t delayedPackets() const noexcept {
        return delayedPackets_.load(std::memory_order_acquire);
    }

private:
    struct ScheduledPacket {
        std::chrono::steady_clock::time_point dueAt;
        sockaddr_in target{};
        std::string payload;
    };

    static std::size_t dataDropBudget(std::uint32_t seq) noexcept {
        if (seq % 11 == 0) {
            return 3;
        }
        if (seq % 3 == 0) {
            return 2;
        }
        if (seq % 2 == 0) {
            return 1;
        }
        return 0;
    }

    void run() {
        while (!stop_.load(std::memory_order_acquire)) {
            drainDuePackets();

            char buffer[4096]{};
            sockaddr_in from{};
            socklen_t fromLen = static_cast<socklen_t>(sizeof(from));
            const auto n = ::recvfrom(fd_,
                                      buffer,
                                      sizeof(buffer),
                                      0,
                                      reinterpret_cast<sockaddr*>(&from),
                                      &fromLen);
            if (n < 0) {
                continue;
            }
            handlePacket(std::string(buffer, static_cast<std::size_t>(n)), from);
        }
        drainDuePackets();
    }

    void handlePacket(std::string payload, const sockaddr_in& from) {
        const auto sourcePort = ntohs(from.sin_port);
        if (sourcePort != endpointAPort_ && sourcePort != endpointBPort_) {
            return;
        }

        const bool fromA = sourcePort == endpointAPort_;
        const auto& target = fromA ? endpointB_ : endpointA_;

        mini::net::kcp::codec::KcpFrame frame;
        const bool decoded = mini::net::kcp::codec::decodeFrame(payload, frame);
        if (!decoded) {
            schedule(std::move(payload), target, 1ms);
            return;
        }

        const bool isData =
            (frame.flags & mini::net::kcp::codec::kKcpFrameFlagData) != 0;
        if (isData) {
            auto& seenData = fromA ? seenDataFromA_ : seenDataFromB_;
            const auto seen = ++seenData[frame.seq];
            const auto budget = dataDropBudget(frame.seq);
            if (seen <= budget) {
                ++droppedPackets_;
                return;
            }
            if (frame.seq % 7 == 0 && seen == budget + 1) {
                ++duplicatedPackets_;
                schedule(payload, target, 2ms);
                schedule(std::move(payload), target, 5ms);
                return;
            }
            if (frame.seq % 5 == 0 && seen == budget + 1) {
                ++delayedPackets_;
                schedule(std::move(payload), target, 25ms);
                return;
            }
        } else if ((frame.flags & mini::net::kcp::codec::kKcpFrameFlagAck) != 0 &&
                   frame.ack != 0) {
            auto& seenAck = fromA ? seenAckFromA_ : seenAckFromB_;
            const auto seen = ++seenAck[frame.ack];
            if (frame.ack % 6 == 0 && seen == 1) {
                ++droppedPackets_;
                return;
            }
        }

        schedule(std::move(payload), target, 1ms);
    }

    void schedule(std::string payload, const sockaddr_in& target, std::chrono::milliseconds delay) {
        scheduled_.push_back(ScheduledPacket{
            std::chrono::steady_clock::now() + delay,
            target,
            std::move(payload)});
    }

    void drainDuePackets() {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = scheduled_.begin(); it != scheduled_.end();) {
            if (it->dueAt > now) {
                ++it;
                continue;
            }

            const auto sent = ::sendto(fd_,
                                       it->payload.data(),
                                       it->payload.size(),
                                       0,
                                       reinterpret_cast<const sockaddr*>(&it->target),
                                       sizeof(it->target));
            assert(sent == static_cast<ssize_t>(it->payload.size()));
            it = scheduled_.erase(it);
        }
    }

    int fd_{-1};
    uint16_t endpointAPort_{0};
    uint16_t endpointBPort_{0};
    uint16_t proxyPort_{0};
    sockaddr_in endpointA_{};
    sockaddr_in endpointB_{};
    std::atomic<bool> stop_{false};
    std::thread worker_;
    std::vector<ScheduledPacket> scheduled_;
    std::unordered_map<std::uint32_t, std::size_t> seenDataFromA_;
    std::unordered_map<std::uint32_t, std::size_t> seenDataFromB_;
    std::unordered_map<std::uint32_t, std::size_t> seenAckFromA_;
    std::unordered_map<std::uint32_t, std::size_t> seenAckFromB_;
    std::atomic<std::size_t> droppedPackets_{0};
    std::atomic<std::size_t> duplicatedPackets_{0};
    std::atomic<std::size_t> delayedPackets_{0};
};

class SelectiveAckGapUdpProxy {
public:
    SelectiveAckGapUdpProxy(uint16_t endpointAPort, uint16_t endpointBPort)
        : endpointAPort_(endpointAPort),
          endpointBPort_(endpointBPort),
          endpointA_(loopbackAddress(endpointAPort)),
          endpointB_(loopbackAddress(endpointBPort)) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
        assert(fd_ >= 0);

        sockaddr_in bindAddr = loopbackAddress(0);
        assert(::bind(fd_, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) == 0);

        socklen_t len = static_cast<socklen_t>(sizeof(bindAddr));
        assert(::getsockname(fd_, reinterpret_cast<sockaddr*>(&bindAddr), &len) == 0);
        proxyPort_ = ntohs(bindAddr.sin_port);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 5000;
        assert(::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    }

    ~SelectiveAckGapUdpProxy() {
        stop();
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    void start() {
        worker_ = std::thread([this] { run(); });
    }

    void stop() {
        stop_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    uint16_t proxyPort() const noexcept {
        return proxyPort_;
    }

    std::size_t droppedPackets() const noexcept {
        return droppedPackets_.load(std::memory_order_acquire);
    }

    std::size_t gapTransmissions() const noexcept {
        return gapTransmissions_.load(std::memory_order_acquire);
    }

    std::size_t nonGapRetransmissions() const noexcept {
        return nonGapRetransmissions_.load(std::memory_order_acquire);
    }

    std::size_t selectiveAckFrames() const noexcept {
        return selectiveAckFrames_.load(std::memory_order_acquire);
    }

private:
    struct ScheduledPacket {
        std::chrono::steady_clock::time_point dueAt;
        sockaddr_in target{};
        std::string payload;
    };

    void run() {
        while (!stop_.load(std::memory_order_acquire)) {
            drainDuePackets();

            char buffer[4096]{};
            sockaddr_in from{};
            socklen_t fromLen = static_cast<socklen_t>(sizeof(from));
            const auto n = ::recvfrom(fd_,
                                      buffer,
                                      sizeof(buffer),
                                      0,
                                      reinterpret_cast<sockaddr*>(&from),
                                      &fromLen);
            if (n < 0) {
                continue;
            }
            handlePacket(std::string(buffer, static_cast<std::size_t>(n)), from);
        }
        drainDuePackets();
    }

    void handlePacket(std::string payload, const sockaddr_in& from) {
        const auto sourcePort = ntohs(from.sin_port);
        if (sourcePort != endpointAPort_ && sourcePort != endpointBPort_) {
            return;
        }

        const bool fromA = sourcePort == endpointAPort_;
        const auto& target = fromA ? endpointB_ : endpointA_;

        mini::net::kcp::codec::KcpFrame frame;
        const bool decoded = mini::net::kcp::codec::decodeFrame(payload, frame);
        if (!decoded) {
            schedule(std::move(payload), target, 1ms);
            return;
        }

        const bool isData =
            (frame.flags & mini::net::kcp::codec::kKcpFrameFlagData) != 0;
        const bool isSelectiveAck =
            (frame.flags & mini::net::kcp::codec::kKcpFrameFlagSelectiveAck) != 0;

        if (fromA && isData) {
            const auto seen = ++seenDataFromA_[frame.seq];
            if (frame.seq == 2) {
                ++gapTransmissions_;
                if (seen <= 2) {
                    ++droppedPackets_;
                    return;
                }
            } else if (frame.seq > 2 && seen > 1) {
                ++nonGapRetransmissions_;
            }
        } else if (!fromA && isSelectiveAck) {
            ++selectiveAckFrames_;
        }

        schedule(std::move(payload), target, 1ms);
    }

    void schedule(std::string payload, const sockaddr_in& target, std::chrono::milliseconds delay) {
        scheduled_.push_back(ScheduledPacket{
            std::chrono::steady_clock::now() + delay,
            target,
            std::move(payload)});
    }

    void drainDuePackets() {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = scheduled_.begin(); it != scheduled_.end();) {
            if (it->dueAt > now) {
                ++it;
                continue;
            }

            const auto sent = ::sendto(fd_,
                                       it->payload.data(),
                                       it->payload.size(),
                                       0,
                                       reinterpret_cast<const sockaddr*>(&it->target),
                                       sizeof(it->target));
            assert(sent == static_cast<ssize_t>(it->payload.size()));
            it = scheduled_.erase(it);
        }
    }

    int fd_{-1};
    uint16_t endpointAPort_{0};
    uint16_t endpointBPort_{0};
    uint16_t proxyPort_{0};
    sockaddr_in endpointA_{};
    sockaddr_in endpointB_{};
    std::atomic<bool> stop_{false};
    std::thread worker_;
    std::vector<ScheduledPacket> scheduled_;
    std::unordered_map<std::uint32_t, std::size_t> seenDataFromA_;
    std::atomic<std::size_t> droppedPackets_{0};
    std::atomic<std::size_t> gapTransmissions_{0};
    std::atomic<std::size_t> nonGapRetransmissions_{0};
    std::atomic<std::size_t> selectiveAckFrames_{0};
};

class MtuProbeUdpProxy {
public:
    MtuProbeUdpProxy(uint16_t endpointAPort,
                     uint16_t endpointBPort,
                     std::size_t pathLimitBytes,
                     std::size_t initialLimitBytes)
        : endpointAPort_(endpointAPort),
          endpointBPort_(endpointBPort),
          endpointA_(loopbackAddress(endpointAPort)),
          endpointB_(loopbackAddress(endpointBPort)),
          pathLimitBytes_(pathLimitBytes),
          initialLimitBytes_(initialLimitBytes) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
        assert(fd_ >= 0);

        sockaddr_in bindAddr = loopbackAddress(0);
        assert(::bind(fd_, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) == 0);

        socklen_t len = static_cast<socklen_t>(sizeof(bindAddr));
        assert(::getsockname(fd_, reinterpret_cast<sockaddr*>(&bindAddr), &len) == 0);
        proxyPort_ = ntohs(bindAddr.sin_port);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 5000;
        assert(::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    }

    ~MtuProbeUdpProxy() {
        stop();
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    void start() {
        worker_ = std::thread([this] { run(); });
    }

    void stop() {
        stop_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    uint16_t proxyPort() const noexcept {
        return proxyPort_;
    }

    std::size_t probeFrames() const noexcept {
        return probeFrames_.load(std::memory_order_acquire);
    }

    std::size_t droppedProbeFrames() const noexcept {
        return droppedProbeFrames_.load(std::memory_order_acquire);
    }

    std::size_t probeAckFrames() const noexcept {
        return probeAckFrames_.load(std::memory_order_acquire);
    }

    std::size_t dataAboveInitialFrames() const noexcept {
        return dataAboveInitialFrames_.load(std::memory_order_acquire);
    }

    std::size_t oversizedDataFrames() const noexcept {
        return oversizedDataFrames_.load(std::memory_order_acquire);
    }

private:
    struct ScheduledPacket {
        std::chrono::steady_clock::time_point dueAt;
        sockaddr_in target{};
        std::string payload;
    };

    void run() {
        while (!stop_.load(std::memory_order_acquire)) {
            drainDuePackets();

            char buffer[4096]{};
            sockaddr_in from{};
            socklen_t fromLen = static_cast<socklen_t>(sizeof(from));
            const auto n = ::recvfrom(fd_,
                                      buffer,
                                      sizeof(buffer),
                                      0,
                                      reinterpret_cast<sockaddr*>(&from),
                                      &fromLen);
            if (n < 0) {
                continue;
            }
            handlePacket(std::string(buffer, static_cast<std::size_t>(n)), from);
        }
        drainDuePackets();
    }

    void handlePacket(std::string payload, const sockaddr_in& from) {
        const auto sourcePort = ntohs(from.sin_port);
        if (sourcePort != endpointAPort_ && sourcePort != endpointBPort_) {
            return;
        }

        const bool fromA = sourcePort == endpointAPort_;
        const auto& target = fromA ? endpointB_ : endpointA_;

        mini::net::kcp::codec::KcpFrame frame;
        const bool decoded = mini::net::kcp::codec::decodeFrame(payload, frame);
        if (!decoded) {
            schedule(std::move(payload), target, 1ms);
            return;
        }

        const bool isData =
            (frame.flags & mini::net::kcp::codec::kKcpFrameFlagData) != 0;
        const bool isAck =
            (frame.flags & mini::net::kcp::codec::kKcpFrameFlagAck) != 0;
        const bool isMtuProbe =
            (frame.flags & mini::net::kcp::codec::kKcpFrameFlagMtuProbe) != 0;

        if (fromA && isMtuProbe && !isAck) {
            ++probeFrames_;
            if (payload.size() > pathLimitBytes_) {
                ++droppedProbeFrames_;
                return;
            }
        } else if (!fromA && isMtuProbe && isAck) {
            ++probeAckFrames_;
        } else if (fromA && isData) {
            if (payload.size() > initialLimitBytes_) {
                ++dataAboveInitialFrames_;
            }
            if (payload.size() > pathLimitBytes_) {
                ++oversizedDataFrames_;
                return;
            }
        }

        schedule(std::move(payload), target, 1ms);
    }

    void schedule(std::string payload, const sockaddr_in& target, std::chrono::milliseconds delay) {
        scheduled_.push_back(ScheduledPacket{
            std::chrono::steady_clock::now() + delay,
            target,
            std::move(payload)});
    }

    void drainDuePackets() {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = scheduled_.begin(); it != scheduled_.end();) {
            if (it->dueAt > now) {
                ++it;
                continue;
            }

            const auto sent = ::sendto(fd_,
                                       it->payload.data(),
                                       it->payload.size(),
                                       0,
                                       reinterpret_cast<const sockaddr*>(&it->target),
                                       sizeof(it->target));
            assert(sent == static_cast<ssize_t>(it->payload.size()));
            it = scheduled_.erase(it);
        }
    }

    int fd_{-1};
    uint16_t endpointAPort_{0};
    uint16_t endpointBPort_{0};
    uint16_t proxyPort_{0};
    sockaddr_in endpointA_{};
    sockaddr_in endpointB_{};
    std::size_t pathLimitBytes_{0};
    std::size_t initialLimitBytes_{0};
    std::atomic<bool> stop_{false};
    std::thread worker_;
    std::vector<ScheduledPacket> scheduled_;
    std::atomic<std::size_t> probeFrames_{0};
    std::atomic<std::size_t> droppedProbeFrames_{0};
    std::atomic<std::size_t> probeAckFrames_{0};
    std::atomic<std::size_t> dataAboveInitialFrames_{0};
    std::atomic<std::size_t> oversizedDataFrames_{0};
};

class CongestionWindowUdpProxy {
public:
    CongestionWindowUdpProxy(uint16_t endpointAPort,
                             uint16_t endpointBPort,
                             std::chrono::milliseconds ackDelay)
        : endpointAPort_(endpointAPort),
          endpointBPort_(endpointBPort),
          endpointA_(loopbackAddress(endpointAPort)),
          endpointB_(loopbackAddress(endpointBPort)),
          ackDelay_(ackDelay) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
        assert(fd_ >= 0);

        sockaddr_in bindAddr = loopbackAddress(0);
        assert(::bind(fd_, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) == 0);

        socklen_t len = static_cast<socklen_t>(sizeof(bindAddr));
        assert(::getsockname(fd_, reinterpret_cast<sockaddr*>(&bindAddr), &len) == 0);
        proxyPort_ = ntohs(bindAddr.sin_port);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 5000;
        assert(::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    }

    ~CongestionWindowUdpProxy() {
        stop();
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    void start() {
        worker_ = std::thread([this] { run(); });
    }

    void stop() {
        stop_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    uint16_t proxyPort() const noexcept {
        return proxyPort_;
    }

    std::size_t dataFramesBeforeFirstAck() const noexcept {
        return dataFramesBeforeFirstAck_.load(std::memory_order_acquire);
    }

    std::size_t maxOutstandingFromA() const noexcept {
        return maxOutstandingFromA_.load(std::memory_order_acquire);
    }

    std::size_t dataFramesFromA() const noexcept {
        return dataFramesFromA_.load(std::memory_order_acquire);
    }

private:
    struct ScheduledPacket {
        std::chrono::steady_clock::time_point dueAt;
        sockaddr_in target{};
        std::string payload;
        bool ackFromB{false};
        std::uint32_t ackSeq{0};
    };

    void run() {
        while (!stop_.load(std::memory_order_acquire)) {
            drainDuePackets();

            char buffer[4096]{};
            sockaddr_in from{};
            socklen_t fromLen = static_cast<socklen_t>(sizeof(from));
            const auto n = ::recvfrom(fd_,
                                      buffer,
                                      sizeof(buffer),
                                      0,
                                      reinterpret_cast<sockaddr*>(&from),
                                      &fromLen);
            if (n < 0) {
                continue;
            }
            handlePacket(std::string(buffer, static_cast<std::size_t>(n)), from);
        }
        drainDuePackets();
    }

    void handlePacket(std::string payload, const sockaddr_in& from) {
        const auto sourcePort = ntohs(from.sin_port);
        if (sourcePort != endpointAPort_ && sourcePort != endpointBPort_) {
            return;
        }

        const bool fromA = sourcePort == endpointAPort_;
        const auto& target = fromA ? endpointB_ : endpointA_;

        mini::net::kcp::codec::KcpFrame frame;
        const bool decoded = mini::net::kcp::codec::decodeFrame(payload, frame);
        if (!decoded) {
            schedule(std::move(payload), target, 1ms);
            return;
        }

        const bool isData =
            (frame.flags & mini::net::kcp::codec::kKcpFrameFlagData) != 0;
        const bool isAck =
            (frame.flags & mini::net::kcp::codec::kKcpFrameFlagAck) != 0;

        if (fromA && isData) {
            ++dataFramesFromA_;
            if (!firstAckDelivered_.load(std::memory_order_acquire)) {
                ++dataFramesBeforeFirstAck_;
            }
            if (seenDataFromA_.emplace(frame.seq, true).second) {
                outstandingFromA_[frame.seq] = true;
                updateMaxOutstanding();
            }
            schedule(std::move(payload), target, 1ms);
            return;
        }

        if (!fromA && isAck && frame.ack != 0) {
            schedule(std::move(payload), target, ackDelay_, true, frame.ack);
            return;
        }

        schedule(std::move(payload), target, 1ms);
    }

    void schedule(std::string payload,
                  const sockaddr_in& target,
                  std::chrono::milliseconds delay,
                  bool ackFromB = false,
                  std::uint32_t ackSeq = 0) {
        scheduled_.push_back(ScheduledPacket{
            std::chrono::steady_clock::now() + delay,
            target,
            std::move(payload),
            ackFromB,
            ackSeq});
    }

    void drainDuePackets() {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = scheduled_.begin(); it != scheduled_.end();) {
            if (it->dueAt > now) {
                ++it;
                continue;
            }

            const auto sent = ::sendto(fd_,
                                       it->payload.data(),
                                       it->payload.size(),
                                       0,
                                       reinterpret_cast<const sockaddr*>(&it->target),
                                       sizeof(it->target));
            assert(sent == static_cast<ssize_t>(it->payload.size()));
            if (it->ackFromB) {
                firstAckDelivered_.store(true, std::memory_order_release);
                eraseAcked(it->ackSeq);
            }
            it = scheduled_.erase(it);
        }
    }

    void eraseAcked(std::uint32_t ackSeq) {
        for (auto it = outstandingFromA_.begin(); it != outstandingFromA_.end();) {
            if (it->first <= ackSeq) {
                it = outstandingFromA_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void updateMaxOutstanding() {
        const auto outstanding = outstandingFromA_.size();
        auto current = maxOutstandingFromA_.load(std::memory_order_acquire);
        while (outstanding > current &&
               !maxOutstandingFromA_.compare_exchange_weak(
                   current,
                   outstanding,
                   std::memory_order_acq_rel,
                   std::memory_order_acquire)) {
        }
    }

    int fd_{-1};
    uint16_t endpointAPort_{0};
    uint16_t endpointBPort_{0};
    uint16_t proxyPort_{0};
    sockaddr_in endpointA_{};
    sockaddr_in endpointB_{};
    std::chrono::milliseconds ackDelay_{0};
    std::atomic<bool> stop_{false};
    std::atomic<bool> firstAckDelivered_{false};
    std::thread worker_;
    std::vector<ScheduledPacket> scheduled_;
    std::unordered_map<std::uint32_t, bool> seenDataFromA_;
    std::unordered_map<std::uint32_t, bool> outstandingFromA_;
    std::atomic<std::size_t> dataFramesBeforeFirstAck_{0};
    std::atomic<std::size_t> dataFramesFromA_{0};
    std::atomic<std::size_t> maxOutstandingFromA_{0};
};

class RedundantCopyUdpProxy {
public:
    RedundantCopyUdpProxy(uint16_t endpointAPort,
                          uint16_t endpointBPort,
                          std::chrono::milliseconds retransmissionThreshold)
        : endpointAPort_(endpointAPort),
          endpointBPort_(endpointBPort),
          endpointA_(loopbackAddress(endpointAPort)),
          endpointB_(loopbackAddress(endpointBPort)),
          retransmissionThreshold_(retransmissionThreshold) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
        assert(fd_ >= 0);

        sockaddr_in bindAddr = loopbackAddress(0);
        assert(::bind(fd_, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) == 0);

        socklen_t len = static_cast<socklen_t>(sizeof(bindAddr));
        assert(::getsockname(fd_, reinterpret_cast<sockaddr*>(&bindAddr), &len) == 0);
        proxyPort_ = ntohs(bindAddr.sin_port);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 5000;
        assert(::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    }

    ~RedundantCopyUdpProxy() {
        stop();
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    void start() {
        worker_ = std::thread([this] { run(); });
    }

    void stop() {
        stop_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    uint16_t proxyPort() const noexcept {
        return proxyPort_;
    }

    std::size_t dataFramesFromA() const noexcept {
        return dataFramesFromA_.load(std::memory_order_acquire);
    }

    std::size_t droppedFirstDataFrames() const noexcept {
        return droppedFirstDataFrames_.load(std::memory_order_acquire);
    }

    std::size_t redundantDataFrames() const noexcept {
        return redundantDataFrames_.load(std::memory_order_acquire);
    }

    std::size_t lateRetransmissionDataFrames() const noexcept {
        return lateRetransmissionDataFrames_.load(std::memory_order_acquire);
    }

private:
    struct ScheduledPacket {
        std::chrono::steady_clock::time_point dueAt;
        sockaddr_in target{};
        std::string payload;
    };

    void run() {
        while (!stop_.load(std::memory_order_acquire)) {
            drainDuePackets();

            char buffer[4096]{};
            sockaddr_in from{};
            socklen_t fromLen = static_cast<socklen_t>(sizeof(from));
            const auto n = ::recvfrom(fd_,
                                      buffer,
                                      sizeof(buffer),
                                      0,
                                      reinterpret_cast<sockaddr*>(&from),
                                      &fromLen);
            if (n < 0) {
                continue;
            }
            handlePacket(std::string(buffer, static_cast<std::size_t>(n)), from);
        }
        drainDuePackets();
    }

    void handlePacket(std::string payload, const sockaddr_in& from) {
        const auto sourcePort = ntohs(from.sin_port);
        if (sourcePort != endpointAPort_ && sourcePort != endpointBPort_) {
            return;
        }

        const bool fromA = sourcePort == endpointAPort_;
        const auto& target = fromA ? endpointB_ : endpointA_;

        mini::net::kcp::codec::KcpFrame frame;
        const bool decoded = mini::net::kcp::codec::decodeFrame(payload, frame);
        if (!decoded) {
            schedule(std::move(payload), target, 1ms);
            return;
        }

        const bool isData =
            (frame.flags & mini::net::kcp::codec::kKcpFrameFlagData) != 0;
        if (fromA && isData) {
            ++dataFramesFromA_;
            const auto now = std::chrono::steady_clock::now();
            auto& seen = seenDataFromA_[frame.seq];
            if (seen == 0) {
                seen = 1;
                firstSeenAt_[frame.seq] = now;
                ++droppedFirstDataFrames_;
                return;
            }

            ++seen;
            const auto firstSeenIt = firstSeenAt_.find(frame.seq);
            const auto elapsed = firstSeenIt == firstSeenAt_.end()
                ? retransmissionThreshold_
                : std::chrono::duration_cast<std::chrono::milliseconds>(now - firstSeenIt->second);
            if (elapsed < retransmissionThreshold_) {
                ++redundantDataFrames_;
            } else {
                ++lateRetransmissionDataFrames_;
            }
        }

        schedule(std::move(payload), target, 1ms);
    }

    void schedule(std::string payload, const sockaddr_in& target, std::chrono::milliseconds delay) {
        scheduled_.push_back(ScheduledPacket{
            std::chrono::steady_clock::now() + delay,
            target,
            std::move(payload)});
    }

    void drainDuePackets() {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = scheduled_.begin(); it != scheduled_.end();) {
            if (it->dueAt > now) {
                ++it;
                continue;
            }

            const auto sent = ::sendto(fd_,
                                       it->payload.data(),
                                       it->payload.size(),
                                       0,
                                       reinterpret_cast<const sockaddr*>(&it->target),
                                       sizeof(it->target));
            assert(sent == static_cast<ssize_t>(it->payload.size()));
            it = scheduled_.erase(it);
        }
    }

    int fd_{-1};
    uint16_t endpointAPort_{0};
    uint16_t endpointBPort_{0};
    uint16_t proxyPort_{0};
    sockaddr_in endpointA_{};
    sockaddr_in endpointB_{};
    std::chrono::milliseconds retransmissionThreshold_{0};
    std::atomic<bool> stop_{false};
    std::thread worker_;
    std::vector<ScheduledPacket> scheduled_;
    std::unordered_map<std::uint32_t, std::size_t> seenDataFromA_;
    std::unordered_map<std::uint32_t, std::chrono::steady_clock::time_point> firstSeenAt_;
    std::atomic<std::size_t> dataFramesFromA_{0};
    std::atomic<std::size_t> droppedFirstDataFrames_{0};
    std::atomic<std::size_t> redundantDataFrames_{0};
    std::atomic<std::size_t> lateRetransmissionDataFrames_{0};
};

class XorParityUdpProxy {
public:
    XorParityUdpProxy(uint16_t endpointAPort,
                      uint16_t endpointBPort,
                      std::size_t groupSize,
                      std::size_t dropIndexInGroup,
                      std::chrono::milliseconds retransmissionThreshold)
        : endpointAPort_(endpointAPort),
          endpointBPort_(endpointBPort),
          endpointA_(loopbackAddress(endpointAPort)),
          endpointB_(loopbackAddress(endpointBPort)),
          groupSize_(groupSize),
          dropIndexInGroup_(dropIndexInGroup),
          retransmissionThreshold_(retransmissionThreshold) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
        assert(fd_ >= 0);

        sockaddr_in bindAddr = loopbackAddress(0);
        assert(::bind(fd_, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) == 0);

        socklen_t len = static_cast<socklen_t>(sizeof(bindAddr));
        assert(::getsockname(fd_, reinterpret_cast<sockaddr*>(&bindAddr), &len) == 0);
        proxyPort_ = ntohs(bindAddr.sin_port);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 5000;
        assert(::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    }

    ~XorParityUdpProxy() {
        stop();
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    void start() {
        worker_ = std::thread([this] { run(); });
    }

    void stop() {
        stop_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    uint16_t proxyPort() const noexcept {
        return proxyPort_;
    }

    std::size_t dataFramesFromA() const noexcept {
        return dataFramesFromA_.load(std::memory_order_acquire);
    }

    std::size_t droppedDataFrames() const noexcept {
        return droppedDataFrames_.load(std::memory_order_acquire);
    }

    std::size_t parityFramesFromA() const noexcept {
        return parityFramesFromA_.load(std::memory_order_acquire);
    }

    std::size_t lateRetransmissionDataFrames() const noexcept {
        return lateRetransmissionDataFrames_.load(std::memory_order_acquire);
    }

private:
    struct ScheduledPacket {
        std::chrono::steady_clock::time_point dueAt;
        sockaddr_in target{};
        std::string payload;
    };

    void run() {
        while (!stop_.load(std::memory_order_acquire)) {
            drainDuePackets();

            char buffer[4096]{};
            sockaddr_in from{};
            socklen_t fromLen = static_cast<socklen_t>(sizeof(from));
            const auto n = ::recvfrom(fd_,
                                      buffer,
                                      sizeof(buffer),
                                      0,
                                      reinterpret_cast<sockaddr*>(&from),
                                      &fromLen);
            if (n < 0) {
                continue;
            }
            handlePacket(std::string(buffer, static_cast<std::size_t>(n)), from);
        }
        drainDuePackets();
    }

    void handlePacket(std::string payload, const sockaddr_in& from) {
        const auto sourcePort = ntohs(from.sin_port);
        if (sourcePort != endpointAPort_ && sourcePort != endpointBPort_) {
            return;
        }

        const bool fromA = sourcePort == endpointAPort_;
        const auto& target = fromA ? endpointB_ : endpointA_;

        mini::net::kcp::codec::KcpFrame frame;
        const bool decoded = mini::net::kcp::codec::decodeFrame(payload, frame);
        if (!decoded) {
            schedule(std::move(payload), target, 1ms);
            return;
        }

        const bool isData =
            (frame.flags & mini::net::kcp::codec::kKcpFrameFlagData) != 0;
        const bool isParity =
            (frame.flags & mini::net::kcp::codec::kKcpFrameFlagXorParity) != 0;
        if (fromA && isParity) {
            ++parityFramesFromA_;
        }
        if (fromA && isData) {
            ++dataFramesFromA_;
            const auto now = std::chrono::steady_clock::now();
            const auto groupIndex = static_cast<std::size_t>((frame.seq - 1) % groupSize_);
            auto& seen = seenDataFromA_[frame.seq];
            if (seen == 0) {
                seen = 1;
                firstSeenAt_[frame.seq] = now;
                if (groupIndex == dropIndexInGroup_) {
                    ++droppedDataFrames_;
                    return;
                }
            } else {
                ++seen;
                const auto firstSeenIt = firstSeenAt_.find(frame.seq);
                const auto elapsed = firstSeenIt == firstSeenAt_.end()
                    ? retransmissionThreshold_
                    : std::chrono::duration_cast<std::chrono::milliseconds>(now - firstSeenIt->second);
                if (elapsed >= retransmissionThreshold_) {
                    ++lateRetransmissionDataFrames_;
                }
            }
        }

        schedule(std::move(payload), target, 1ms);
    }

    void schedule(std::string payload, const sockaddr_in& target, std::chrono::milliseconds delay) {
        scheduled_.push_back(ScheduledPacket{
            std::chrono::steady_clock::now() + delay,
            target,
            std::move(payload)});
    }

    void drainDuePackets() {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = scheduled_.begin(); it != scheduled_.end();) {
            if (it->dueAt > now) {
                ++it;
                continue;
            }

            const auto sent = ::sendto(fd_,
                                       it->payload.data(),
                                       it->payload.size(),
                                       0,
                                       reinterpret_cast<const sockaddr*>(&it->target),
                                       sizeof(it->target));
            assert(sent == static_cast<ssize_t>(it->payload.size()));
            it = scheduled_.erase(it);
        }
    }

    int fd_{-1};
    uint16_t endpointAPort_{0};
    uint16_t endpointBPort_{0};
    uint16_t proxyPort_{0};
    sockaddr_in endpointA_{};
    sockaddr_in endpointB_{};
    std::size_t groupSize_{0};
    std::size_t dropIndexInGroup_{0};
    std::chrono::milliseconds retransmissionThreshold_{0};
    std::atomic<bool> stop_{false};
    std::thread worker_;
    std::vector<ScheduledPacket> scheduled_;
    std::unordered_map<std::uint32_t, std::size_t> seenDataFromA_;
    std::unordered_map<std::uint32_t, std::chrono::steady_clock::time_point> firstSeenAt_;
    std::atomic<std::size_t> dataFramesFromA_{0};
    std::atomic<std::size_t> droppedDataFrames_{0};
    std::atomic<std::size_t> parityFramesFromA_{0};
    std::atomic<std::size_t> lateRetransmissionDataFrames_{0};
};

struct ReceivedState {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> onB;
    std::vector<std::string> onA;
};

template <typename Predicate>
bool wait_for_predicate(Predicate&& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

std::string makeQuotedKcpFramePrefix(mini::net::transport::TransportSessionId sessionId) {
    mini::net::kcp::codec::KcpFrame frame;
    frame.sessionId = sessionId;
    frame.seq = 1;
    frame.flags = mini::net::kcp::codec::kKcpFrameFlagData;
    return mini::net::kcp::codec::encodeFrame(frame);
}

void test_kcp_survives_loss_reorder_duplicate_and_jitter() {
    const auto portA = allocatePort();
    const auto portB = allocatePort();
    ImpairingUdpProxy proxy(portA, portB);
    proxy.start();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::net::kcp::KcpTransport transportA(
        loop,
        mini::net::InetAddress(portA, true),
        "contract-kcp-stress-a");
    mini::net::kcp::KcpTransport transportB(
        loop,
        mini::net::InetAddress(portB, true),
        "contract-kcp-stress-b");

    const auto largeMessage = std::string("large-state:") +
        std::string(mini::net::kcp::KcpTransport::kMaxFragmentPayloadSize * 3 + 137, 'L');
    assert(largeMessage.size() > mini::net::kcp::KcpTransport::kMaxSingleFramePayloadSize);
    assert(largeMessage.size() <= mini::net::kcp::KcpTransport::kMaxApplicationPayloadSize);

    const std::vector<std::string> messages{
        largeMessage,
        "move-1",
        "move-2",
        "move-3",
        "move-4",
        "move-5",
    };

    ReceivedState received;
    std::promise<mini::net::transport::TransportSessionId> sessionPromise;
    auto sessionFuture = sessionPromise.get_future();

    loop->queueInLoop([&] {
        transportA.setMessageCallback(
            [&](mini::net::transport::TransportSessionId,
                std::string_view packet,
                const mini::net::InetAddress&) {
                {
                    std::lock_guard lock(received.mutex);
                    received.onA.emplace_back(packet);
                }
                received.cv.notify_all();
            });

        transportB.setMessageCallback(
            [&](mini::net::transport::TransportSessionId sessionId,
                std::string_view packet,
                const mini::net::InetAddress&) {
                {
                    std::lock_guard lock(received.mutex);
                    received.onB.emplace_back(packet);
                }
                transportB.sendTo(sessionId, "echo:" + std::string(packet));
                received.cv.notify_all();
            });

        transportA.start();
        transportB.start();

        auto session = transportA.openSession(
            mini::net::InetAddress("127.0.0.1", proxy.proxyPort()));
        if (!session) {
            sessionPromise.set_value(mini::net::transport::kInvalidTransportSessionId);
            return;
        }
        for (const auto& message : messages) {
            session->send(message);
        }
        sessionPromise.set_value(session->sessionId());
    });

    const auto sessionId = sessionFuture.get();
    assert(sessionId != mini::net::transport::kInvalidTransportSessionId);

    {
        std::unique_lock lock(received.mutex);
        assert(received.cv.wait_for(lock, 3s, [&] {
            return received.onB.size() >= messages.size() &&
                   received.onA.size() >= messages.size();
        }));

        assert(received.onB.size() == messages.size());
        assert(received.onB == messages);

        std::vector<std::string> echoes;
        echoes.reserve(messages.size());
        for (const auto& message : messages) {
            echoes.push_back("echo:" + message);
        }
        assert(received.onA.size() == echoes.size());
        assert(received.onA == echoes);
    }

    assert(proxy.droppedPackets() >= 1);
    assert(proxy.duplicatedPackets() >= 1);
    assert(proxy.reorderedPackets() >= 1);
    assert(proxy.delayedPackets() >= 3);

    std::promise<void> stoppedPromise;
    auto stoppedFuture = stoppedPromise.get_future();
    loop->queueInLoop([&] {
        transportA.closeSession(sessionId);
        transportA.stop();
        transportB.stop();
        stoppedPromise.set_value();
    });
    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);
    proxy.stop();
}

void test_kcp_long_run_survives_periodic_high_loss_with_tuned_retry_budget() {
    const auto portA = allocatePort();
    const auto portB = allocatePort();
    LongRunLossUdpProxy proxy(portA, portB);
    proxy.start();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::net::kcp::KcpTransportOptions options;
    options.initialRto = 8ms;
    options.maxRto = 40ms;
    options.maxRetransmissions = 10;

    mini::net::kcp::KcpTransport transportA(
        loop,
        mini::net::InetAddress(portA, true),
        "contract-kcp-long-run-a",
        true,
        options);
    mini::net::kcp::KcpTransport transportB(
        loop,
        mini::net::InetAddress(portB, true),
        "contract-kcp-long-run-b",
        true,
        options);

    std::vector<std::string> messages;
    for (int i = 0; i < 18; ++i) {
        if (i % 9 == 0) {
            messages.push_back("state-" + std::to_string(i) + ":" +
                               std::string(mini::net::kcp::KcpTransport::kMaxSingleFramePayloadSize + 137,
                                           static_cast<char>('A' + i)));
        } else {
            messages.push_back("tick-" + std::to_string(i));
        }
    }

    ReceivedState received;
    std::promise<mini::net::transport::TransportSessionId> sessionPromise;
    auto sessionFuture = sessionPromise.get_future();

    loop->queueInLoop([&] {
        transportA.setMessageCallback(
            [&](mini::net::transport::TransportSessionId,
                std::string_view packet,
                const mini::net::InetAddress&) {
                {
                    std::lock_guard lock(received.mutex);
                    received.onA.emplace_back(packet);
                }
                received.cv.notify_all();
            });

        transportB.setMessageCallback(
            [&](mini::net::transport::TransportSessionId sessionId,
                std::string_view packet,
                const mini::net::InetAddress&) {
                {
                    std::lock_guard lock(received.mutex);
                    received.onB.emplace_back(packet);
                }
                transportB.sendTo(sessionId, "echo:" + std::string(packet));
                received.cv.notify_all();
            });

        transportA.start();
        transportB.start();

        auto session = transportA.openSession(
            mini::net::InetAddress("127.0.0.1", proxy.proxyPort()));
        if (!session) {
            sessionPromise.set_value(mini::net::transport::kInvalidTransportSessionId);
            return;
        }
        for (const auto& message : messages) {
            session->send(message);
        }
        sessionPromise.set_value(session->sessionId());
    });

    const auto sessionId = sessionFuture.get();
    assert(sessionId != mini::net::transport::kInvalidTransportSessionId);

    {
        std::unique_lock lock(received.mutex);
        assert(received.cv.wait_for(lock, 6s, [&] {
            return received.onB.size() >= messages.size() &&
                   received.onA.size() >= messages.size();
        }));

        assert(received.onB.size() == messages.size());
        assert(received.onB == messages);

        std::vector<std::string> echoes;
        echoes.reserve(messages.size());
        for (const auto& message : messages) {
            echoes.push_back("echo:" + message);
        }
        assert(received.onA.size() == echoes.size());
        assert(received.onA == echoes);
    }

    assert(transportA.sessionCount() == 1);
    assert(transportB.sessionCount() == 1);
    assert(proxy.droppedPackets() >= 16);
    assert(proxy.duplicatedPackets() >= 2);
    assert(proxy.delayedPackets() >= 2);

    std::promise<void> stoppedPromise;
    auto stoppedFuture = stoppedPromise.get_future();
    loop->queueInLoop([&] {
        transportA.closeSession(sessionId);
        transportA.stop();
        transportB.stop();
        stoppedPromise.set_value();
    });
    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);
    proxy.stop();
}

void test_kcp_selective_ack_suppresses_retransmit_for_out_of_order_packets() {
    const auto portA = allocatePort();
    const auto portB = allocatePort();
    SelectiveAckGapUdpProxy proxy(portA, portB);
    proxy.start();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::net::kcp::KcpTransportOptions options;
    options.initialRto = 12ms;
    options.maxRto = 40ms;
    options.maxRetransmissions = 6;

    mini::net::kcp::KcpTransport transportA(
        loop,
        mini::net::InetAddress(portA, true),
        "contract-kcp-sack-a",
        true,
        options);
    mini::net::kcp::KcpTransport transportB(
        loop,
        mini::net::InetAddress(portB, true),
        "contract-kcp-sack-b",
        true,
        options);

    const std::vector<std::string> messages{
        "sack-1",
        "sack-2",
        "sack-3",
        "sack-4",
        "sack-5",
        "sack-6",
        "sack-7",
        "sack-8",
    };

    ReceivedState received;
    std::promise<mini::net::transport::TransportSessionId> sessionPromise;
    auto sessionFuture = sessionPromise.get_future();

    loop->queueInLoop([&] {
        transportB.setMessageCallback(
            [&](mini::net::transport::TransportSessionId,
                std::string_view packet,
                const mini::net::InetAddress&) {
                {
                    std::lock_guard lock(received.mutex);
                    received.onB.emplace_back(packet);
                }
                received.cv.notify_all();
            });

        transportA.start();
        transportB.start();

        auto session = transportA.openSession(
            mini::net::InetAddress("127.0.0.1", proxy.proxyPort()));
        if (!session) {
            sessionPromise.set_value(mini::net::transport::kInvalidTransportSessionId);
            return;
        }
        for (const auto& message : messages) {
            session->send(message);
        }
        sessionPromise.set_value(session->sessionId());
    });

    const auto sessionId = sessionFuture.get();
    assert(sessionId != mini::net::transport::kInvalidTransportSessionId);

    {
        std::unique_lock lock(received.mutex);
        assert(received.cv.wait_for(lock, 4s, [&] {
            return received.onB.size() >= messages.size();
        }));

        assert(received.onB.size() == messages.size());
        assert(received.onB == messages);
    }

    assert(proxy.droppedPackets() == 2);
    assert(proxy.gapTransmissions() >= 3);
    assert(proxy.selectiveAckFrames() >= 1);
    assert(proxy.nonGapRetransmissions() == 0);

    std::promise<void> stoppedPromise;
    auto stoppedFuture = stoppedPromise.get_future();
    loop->queueInLoop([&] {
        transportA.closeSession(sessionId);
        transportA.stop();
        transportB.stop();
        stoppedPromise.set_value();
    });
    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);
    proxy.stop();
}

void test_kcp_mtu_probe_success_allows_larger_single_frame() {
    const auto portA = allocatePort();
    const auto portB = allocatePort();
    MtuProbeUdpProxy proxy(portA, portB, 1300, 900);
    proxy.start();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::net::kcp::KcpTransportOptions options;
    options.initialRto = 10ms;
    options.maxRto = 30ms;
    options.maxRetransmissions = 4;
    options.enableMtuProbing = true;
    options.minDatagramPayloadSize = 900;
    options.maxDatagramPayloadSize = 1200;
    options.mtuProbeStepBytes = 300;
    options.mtuProbeMaxRetries = 1;
    options.mtuProbeInterval = 10ms;

    mini::net::kcp::KcpTransport transportA(
        loop,
        mini::net::InetAddress(portA, true),
        "contract-kcp-mtu-success-a",
        true,
        options);
    mini::net::kcp::KcpTransport transportB(
        loop,
        mini::net::InetAddress(portB, true),
        "contract-kcp-mtu-success-b");

    ReceivedState received;
    std::promise<std::shared_ptr<mini::net::kcp::KcpSession>> sessionPromise;
    auto sessionFuture = sessionPromise.get_future();

    loop->queueInLoop([&] {
        transportB.setMessageCallback(
            [&](mini::net::transport::TransportSessionId,
                std::string_view packet,
                const mini::net::InetAddress&) {
                {
                    std::lock_guard lock(received.mutex);
                    received.onB.emplace_back(packet);
                }
                received.cv.notify_all();
            });

        transportA.start();
        transportB.start();

        sessionPromise.set_value(
            transportA.openSession(mini::net::InetAddress("127.0.0.1", proxy.proxyPort())));
    });

    const auto session = sessionFuture.get();
    assert(session != nullptr);
    assert(wait_for_predicate([&] { return proxy.probeAckFrames() >= 1; }, 2s));
    std::this_thread::sleep_for(50ms);

    const auto payload = std::string(950, 'M');
    session->send(payload);

    {
        std::unique_lock lock(received.mutex);
        assert(received.cv.wait_for(lock, 2s, [&] {
            return received.onB.size() >= 1;
        }));
        assert(received.onB.size() == 1);
        assert(received.onB.front() == payload);
    }

    assert(proxy.probeFrames() >= 1);
    assert(proxy.probeAckFrames() >= 1);
    assert(proxy.dataAboveInitialFrames() >= 1);
    assert(proxy.oversizedDataFrames() == 0);

    std::promise<void> stoppedPromise;
    auto stoppedFuture = stoppedPromise.get_future();
    loop->queueInLoop([&] {
        transportA.closeSession(session->sessionId());
        transportA.stop();
        transportB.stop();
        stoppedPromise.set_value();
    });
    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);
    proxy.stop();
}

void test_kcp_mtu_path_cache_reuses_confirmed_size_for_reopened_session() {
    const auto portA = allocatePort();
    const auto portB = allocatePort();
    MtuProbeUdpProxy proxy(portA, portB, 1300, 900);
    proxy.start();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::net::kcp::KcpTransportOptions options;
    options.initialRto = 10ms;
    options.maxRto = 30ms;
    options.maxRetransmissions = 4;
    options.enableMtuProbing = true;
    options.enableMtuPathCache = true;
    options.minDatagramPayloadSize = 900;
    options.maxDatagramPayloadSize = 1200;
    options.mtuProbeStepBytes = 300;
    options.mtuProbeMaxRetries = 1;
    options.mtuProbeInterval = 10ms;

    mini::net::kcp::KcpTransport transportA(
        loop,
        mini::net::InetAddress(portA, true),
        "contract-kcp-mtu-path-cache-success-a",
        true,
        options);
    mini::net::kcp::KcpTransport transportB(
        loop,
        mini::net::InetAddress(portB, true),
        "contract-kcp-mtu-path-cache-success-b");

    ReceivedState received;
    std::promise<std::shared_ptr<mini::net::kcp::KcpSession>> firstSessionPromise;
    auto firstSessionFuture = firstSessionPromise.get_future();

    loop->queueInLoop([&] {
        transportB.setMessageCallback(
            [&](mini::net::transport::TransportSessionId,
                std::string_view packet,
                const mini::net::InetAddress&) {
                {
                    std::lock_guard lock(received.mutex);
                    received.onB.emplace_back(packet);
                }
                received.cv.notify_all();
            });

        transportA.start();
        transportB.start();

        firstSessionPromise.set_value(
            transportA.openSession(mini::net::InetAddress("127.0.0.1", proxy.proxyPort())));
    });

    const auto firstSession = firstSessionFuture.get();
    assert(firstSession != nullptr);
    assert(wait_for_predicate([&] { return proxy.probeAckFrames() >= 1; }, 2s));
    std::this_thread::sleep_for(50ms);

    std::promise<void> closePromise;
    auto closeFuture = closePromise.get_future();
    loop->queueInLoop([&] {
        transportA.closeSession(firstSession->sessionId());
        closePromise.set_value();
    });
    assert(closeFuture.wait_for(2s) == std::future_status::ready);

    const auto probeFramesAfterFirstSession = proxy.probeFrames();

    std::promise<std::shared_ptr<mini::net::kcp::KcpSession>> secondSessionPromise;
    auto secondSessionFuture = secondSessionPromise.get_future();
    loop->queueInLoop([&] {
        secondSessionPromise.set_value(
            transportA.openSession(mini::net::InetAddress("127.0.0.1", proxy.proxyPort())));
    });

    const auto secondSession = secondSessionFuture.get();
    assert(secondSession != nullptr);

    const auto payload = std::string(950, 'C');
    secondSession->send(payload);

    {
        std::unique_lock lock(received.mutex);
        assert(received.cv.wait_for(lock, 2s, [&] {
            return received.onB.size() >= 1;
        }));
        assert(received.onB.size() == 1);
        assert(received.onB.front() == payload);
    }

    std::this_thread::sleep_for(80ms);
    assert(proxy.probeFrames() == probeFramesAfterFirstSession);
    assert(proxy.dataAboveInitialFrames() >= 1);
    assert(proxy.oversizedDataFrames() == 0);

    std::promise<void> stoppedPromise;
    auto stoppedFuture = stoppedPromise.get_future();
    loop->queueInLoop([&] {
        transportA.closeSession(secondSession->sessionId());
        transportA.stop();
        transportB.stop();
        stoppedPromise.set_value();
    });
    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);
    proxy.stop();
}

void test_kcp_shared_mtu_path_cache_survives_transport_restart() {
    const auto portA = allocatePort();
    const auto portB = allocatePort();
    MtuProbeUdpProxy proxy(portA, portB, 1300, 900);
    proxy.start();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    auto sharedCache = std::make_shared<mini::net::transport::PathMtuCache>();
    mini::net::kcp::KcpTransportOptions options;
    options.initialRto = 10ms;
    options.maxRto = 30ms;
    options.maxRetransmissions = 4;
    options.enableMtuProbing = true;
    options.enableMtuPathCache = true;
    options.sharedMtuPathCache = sharedCache;
    options.minDatagramPayloadSize = 900;
    options.maxDatagramPayloadSize = 1200;
    options.mtuProbeStepBytes = 300;
    options.mtuProbeMaxRetries = 1;
    options.mtuProbeInterval = 10ms;

    mini::net::kcp::KcpTransport transportB(
        loop,
        mini::net::InetAddress(portB, true),
        "contract-kcp-shared-mtu-path-cache-b");

    ReceivedState received;
    const mini::net::InetAddress peerAddress("127.0.0.1", proxy.proxyPort());

    std::promise<void> receiverStartedPromise;
    auto receiverStartedFuture = receiverStartedPromise.get_future();
    loop->queueInLoop([&] {
        transportB.setMessageCallback(
            [&](mini::net::transport::TransportSessionId,
                std::string_view packet,
                const mini::net::InetAddress&) {
                {
                    std::lock_guard lock(received.mutex);
                    received.onB.emplace_back(packet);
                }
                received.cv.notify_all();
            });
        transportB.start();
        receiverStartedPromise.set_value();
    });
    assert(receiverStartedFuture.wait_for(2s) == std::future_status::ready);

    {
        mini::net::kcp::KcpTransport firstTransportA(
            loop,
            mini::net::InetAddress(portA, true),
            "contract-kcp-shared-mtu-path-cache-a1",
            true,
            options);

        std::promise<std::shared_ptr<mini::net::kcp::KcpSession>> firstSessionPromise;
        auto firstSessionFuture = firstSessionPromise.get_future();
        loop->queueInLoop([&] {
            firstTransportA.start();
            firstSessionPromise.set_value(firstTransportA.openSession(peerAddress));
        });

        const auto firstSession = firstSessionFuture.get();
        assert(firstSession != nullptr);
        assert(wait_for_predicate([&] {
            const auto entry = sharedCache->find(peerAddress);
            return entry.has_value() && entry->confirmedDatagramPayloadSize >= 1200;
        }, 2s));

        std::promise<void> stoppedPromise;
        auto stoppedFuture = stoppedPromise.get_future();
        loop->queueInLoop([&] {
            firstTransportA.closeSession(firstSession->sessionId());
            firstTransportA.stop();
            stoppedPromise.set_value();
        });
        assert(stoppedFuture.wait_for(2s) == std::future_status::ready);
    }

    const auto probeFramesAfterFirstTransport = proxy.probeFrames();
    const auto sharedEntry = sharedCache->find(peerAddress);
    assert(sharedEntry.has_value());
    assert(sharedEntry->confirmedDatagramPayloadSize >= 1200);

    {
        mini::net::kcp::KcpTransport secondTransportA(
            loop,
            mini::net::InetAddress(portA, true),
            "contract-kcp-shared-mtu-path-cache-a2",
            true,
            options);

        std::promise<std::shared_ptr<mini::net::kcp::KcpSession>> secondSessionPromise;
        auto secondSessionFuture = secondSessionPromise.get_future();
        loop->queueInLoop([&] {
            secondTransportA.start();
            secondSessionPromise.set_value(secondTransportA.openSession(peerAddress));
        });

        const auto secondSession = secondSessionFuture.get();
        assert(secondSession != nullptr);

        const auto payload = std::string(950, 'G');
        secondSession->send(payload);

        {
            std::unique_lock lock(received.mutex);
            assert(received.cv.wait_for(lock, 2s, [&] {
                return received.onB.size() >= 1;
            }));
            assert(received.onB.size() == 1);
            assert(received.onB.front() == payload);
        }

        std::this_thread::sleep_for(80ms);
        assert(proxy.probeFrames() == probeFramesAfterFirstTransport);
        assert(proxy.dataAboveInitialFrames() >= 1);
        assert(proxy.oversizedDataFrames() == 0);

        std::promise<void> stoppedPromise;
        auto stoppedFuture = stoppedPromise.get_future();
        loop->queueInLoop([&] {
            secondTransportA.closeSession(secondSession->sessionId());
            secondTransportA.stop();
            transportB.stop();
            stoppedPromise.set_value();
        });
        assert(stoppedFuture.wait_for(2s) == std::future_status::ready);
    }

    proxy.stop();
}

void test_kcp_shared_mtu_path_cache_carries_blackhole_cooldown_to_replacement_transport() {
    const auto portA = allocatePort();
    const auto portB = allocatePort();
    MtuProbeUdpProxy proxy(portA, portB, 950, 900);
    proxy.start();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    auto sharedCache = std::make_shared<mini::net::transport::PathMtuCache>();
    mini::net::kcp::KcpTransportOptions options;
    options.initialRto = 10ms;
    options.maxRto = 30ms;
    options.maxRetransmissions = 4;
    options.enableMtuProbing = true;
    options.enableMtuPathCache = true;
    options.sharedMtuPathCache = sharedCache;
    options.minDatagramPayloadSize = 900;
    options.maxDatagramPayloadSize = 1200;
    options.mtuProbeStepBytes = 300;
    options.mtuProbeMaxRetries = 1;
    options.mtuProbeInterval = 10ms;
    options.mtuProbeBlackholeCooldown = 1200ms;

    mini::net::kcp::KcpTransport transportB(
        loop,
        mini::net::InetAddress(portB, true),
        "contract-kcp-shared-mtu-path-cache-blackhole-b");

    ReceivedState received;
    const mini::net::InetAddress peerAddress("127.0.0.1", proxy.proxyPort());

    std::promise<void> receiverStartedPromise;
    auto receiverStartedFuture = receiverStartedPromise.get_future();
    loop->queueInLoop([&] {
        transportB.setMessageCallback(
            [&](mini::net::transport::TransportSessionId,
                std::string_view packet,
                const mini::net::InetAddress&) {
                {
                    std::lock_guard lock(received.mutex);
                    received.onB.emplace_back(packet);
                }
                received.cv.notify_all();
            });
        transportB.start();
        receiverStartedPromise.set_value();
    });
    assert(receiverStartedFuture.wait_for(2s) == std::future_status::ready);

    {
        mini::net::kcp::KcpTransport firstTransportA(
            loop,
            mini::net::InetAddress(portA, true),
            "contract-kcp-shared-mtu-path-cache-blackhole-a1",
            true,
            options);

        std::promise<std::shared_ptr<mini::net::kcp::KcpSession>> firstSessionPromise;
        auto firstSessionFuture = firstSessionPromise.get_future();
        loop->queueInLoop([&] {
            firstTransportA.start();
            firstSessionPromise.set_value(firstTransportA.openSession(peerAddress));
        });

        const auto firstSession = firstSessionFuture.get();
        assert(firstSession != nullptr);
        assert(wait_for_predicate([&] {
            const auto entry = sharedCache->find(peerAddress);
            return entry.has_value() &&
                   entry->cooldownUntil > mini::net::transport::PathMtuCache::Clock::now() &&
                   entry->blackholeCount >= 1;
        }, 2s));

        std::promise<void> stoppedPromise;
        auto stoppedFuture = stoppedPromise.get_future();
        loop->queueInLoop([&] {
            firstTransportA.closeSession(firstSession->sessionId());
            firstTransportA.stop();
            stoppedPromise.set_value();
        });
        assert(stoppedFuture.wait_for(2s) == std::future_status::ready);
    }

    const auto droppedAfterFirstTransport = proxy.droppedProbeFrames();

    {
        mini::net::kcp::KcpTransport secondTransportA(
            loop,
            mini::net::InetAddress(portA, true),
            "contract-kcp-shared-mtu-path-cache-blackhole-a2",
            true,
            options);

        std::promise<std::shared_ptr<mini::net::kcp::KcpSession>> secondSessionPromise;
        auto secondSessionFuture = secondSessionPromise.get_future();
        loop->queueInLoop([&] {
            secondTransportA.start();
            secondSessionPromise.set_value(secondTransportA.openSession(peerAddress));
        });

        const auto secondSession = secondSessionFuture.get();
        assert(secondSession != nullptr);
        std::this_thread::sleep_for(150ms);
        assert(proxy.droppedProbeFrames() == droppedAfterFirstTransport);

        const auto payload = std::string(950, 'H');
        secondSession->send(payload);

        {
            std::unique_lock lock(received.mutex);
            assert(received.cv.wait_for(lock, 2s, [&] {
                return received.onB.size() >= 1;
            }));
            assert(received.onB.size() == 1);
            assert(received.onB.front() == payload);
        }

        assert(proxy.probeAckFrames() == 0);
        assert(proxy.dataAboveInitialFrames() == 0);
        assert(proxy.oversizedDataFrames() == 0);

        std::promise<void> stoppedPromise;
        auto stoppedFuture = stoppedPromise.get_future();
        loop->queueInLoop([&] {
            secondTransportA.closeSession(secondSession->sessionId());
            secondTransportA.stop();
            transportB.stop();
            stoppedPromise.set_value();
        });
        assert(stoppedFuture.wait_for(2s) == std::future_status::ready);
    }

    proxy.stop();
}

void test_kcp_path_mtu_failure_signal_downgrades_cached_size_for_reopened_session() {
    const auto portA = allocatePort();
    const auto portB = allocatePort();
    MtuProbeUdpProxy proxy(portA, portB, 1300, 900);
    proxy.start();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::net::kcp::KcpTransportOptions options;
    options.initialRto = 100ms;
    options.maxRto = 200ms;
    options.maxRetransmissions = 4;
    options.enableMtuProbing = true;
    options.enableMtuPathCache = true;
    options.minDatagramPayloadSize = 900;
    options.maxDatagramPayloadSize = 1200;
    options.mtuProbeStepBytes = 300;
    options.mtuProbeMaxRetries = 1;
    options.mtuProbeInterval = 10ms;
    options.mtuProbeBlackholeCooldown = 1200ms;

    mini::net::kcp::KcpTransport transportA(
        loop,
        mini::net::InetAddress(portA, true),
        "contract-kcp-path-mtu-failure-signal-a",
        true,
        options);
    mini::net::kcp::KcpTransport transportB(
        loop,
        mini::net::InetAddress(portB, true),
        "contract-kcp-path-mtu-failure-signal-b");

    ReceivedState received;
    std::promise<std::shared_ptr<mini::net::kcp::KcpSession>> firstSessionPromise;
    auto firstSessionFuture = firstSessionPromise.get_future();
    const mini::net::InetAddress peerAddress("127.0.0.1", proxy.proxyPort());

    loop->queueInLoop([&] {
        transportB.setMessageCallback(
            [&](mini::net::transport::TransportSessionId,
                std::string_view packet,
                const mini::net::InetAddress&) {
                {
                    std::lock_guard lock(received.mutex);
                    received.onB.emplace_back(packet);
                }
                received.cv.notify_all();
            });

        transportA.start();
        transportB.start();

        firstSessionPromise.set_value(transportA.openSession(peerAddress));
    });

    const auto firstSession = firstSessionFuture.get();
    assert(firstSession != nullptr);
    assert(wait_for_predicate([&] { return proxy.probeAckFrames() >= 1; }, 2s));
    std::this_thread::sleep_for(50ms);

    const auto firstPayload = std::string(950, 'I');
    firstSession->send(firstPayload);
    {
        std::unique_lock lock(received.mutex);
        assert(received.cv.wait_for(lock, 2s, [&] {
            return received.onB.size() >= 1;
        }));
        assert(received.onB[0] == firstPayload);
    }
    assert(proxy.dataAboveInitialFrames() >= 1);

    transportA.notifyPathMtuFailure(peerAddress, 1200, 900);
    std::promise<void> signalAppliedPromise;
    auto signalAppliedFuture = signalAppliedPromise.get_future();
    loop->queueInLoop([&] { signalAppliedPromise.set_value(); });
    assert(signalAppliedFuture.wait_for(2s) == std::future_status::ready);

    const auto probeFramesAfterSignal = proxy.probeFrames();
    const auto dataAboveInitialAfterSignal = proxy.dataAboveInitialFrames();

    const auto secondPayload = std::string(950, 'J');
    firstSession->send(secondPayload);
    {
        std::unique_lock lock(received.mutex);
        assert(received.cv.wait_for(lock, 2s, [&] {
            return received.onB.size() >= 2;
        }));
        assert(received.onB[1] == secondPayload);
    }

    assert(proxy.dataAboveInitialFrames() == dataAboveInitialAfterSignal);
    assert(proxy.oversizedDataFrames() == 0);
    std::this_thread::sleep_for(150ms);
    assert(proxy.probeFrames() == probeFramesAfterSignal);

    std::promise<void> closePromise;
    auto closeFuture = closePromise.get_future();
    loop->queueInLoop([&] {
        transportA.closeSession(firstSession->sessionId());
        transportB.closeSession(firstSession->sessionId());
        closePromise.set_value();
    });
    assert(closeFuture.wait_for(2s) == std::future_status::ready);

    std::promise<std::shared_ptr<mini::net::kcp::KcpSession>> secondSessionPromise;
    auto secondSessionFuture = secondSessionPromise.get_future();
    loop->queueInLoop([&] {
        secondSessionPromise.set_value(transportA.openSession(peerAddress));
    });

    const auto secondSession = secondSessionFuture.get();
    assert(secondSession != nullptr);

    const auto thirdPayload = std::string(950, 'K');
    secondSession->send(thirdPayload);
    {
        std::unique_lock lock(received.mutex);
        assert(received.cv.wait_for(lock, 2s, [&] {
            return received.onB.size() >= 3;
        }));
        assert(received.onB[2] == thirdPayload);
    }

    assert(proxy.dataAboveInitialFrames() == dataAboveInitialAfterSignal);
    std::this_thread::sleep_for(150ms);
    assert(proxy.probeFrames() == probeFramesAfterSignal);

    std::promise<void> stoppedPromise;
    auto stoppedFuture = stoppedPromise.get_future();
    loop->queueInLoop([&] {
        transportA.closeSession(secondSession->sessionId());
        transportA.stop();
        transportB.stop();
        stoppedPromise.set_value();
    });
    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);
    proxy.stop();
}

void test_kcp_authenticated_raw_icmp_path_mtu_signal_requires_matching_session_quote() {
    const auto portA = allocatePort();
    const auto portB = allocatePort();
    MtuProbeUdpProxy proxy(portA, portB, 1300, 900);
    proxy.start();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::net::kcp::KcpTransportOptions options;
    options.initialRto = 100ms;
    options.maxRto = 200ms;
    options.maxRetransmissions = 4;
    options.enableMtuProbing = true;
    options.enablePathMtuSignalAuthentication = true;
    options.minDatagramPayloadSize = 900;
    options.maxDatagramPayloadSize = 1200;
    options.mtuProbeStepBytes = 300;
    options.mtuProbeMaxRetries = 1;
    options.mtuProbeInterval = 10ms;
    options.mtuProbeBlackholeCooldown = 1200ms;

    mini::net::kcp::KcpTransport transportA(
        loop,
        mini::net::InetAddress(portA, true),
        "contract-kcp-authenticated-raw-icmp-pmtu-a",
        true,
        options);
    mini::net::kcp::KcpTransport transportB(
        loop,
        mini::net::InetAddress(portB, true),
        "contract-kcp-authenticated-raw-icmp-pmtu-b");

    ReceivedState received;
    std::promise<std::shared_ptr<mini::net::kcp::KcpSession>> sessionPromise;
    auto sessionFuture = sessionPromise.get_future();
    const mini::net::InetAddress peerAddress("127.0.0.1", proxy.proxyPort());

    loop->queueInLoop([&] {
        transportB.setMessageCallback(
            [&](mini::net::transport::TransportSessionId,
                std::string_view packet,
                const mini::net::InetAddress&) {
                {
                    std::lock_guard lock(received.mutex);
                    received.onB.emplace_back(packet);
                }
                received.cv.notify_all();
            });

        transportA.start();
        transportB.start();
        sessionPromise.set_value(transportA.openSession(peerAddress));
    });

    const auto session = sessionFuture.get();
    assert(session != nullptr);
    assert(wait_for_predicate([&] { return proxy.probeAckFrames() >= 1; }, 2s));
    std::this_thread::sleep_for(50ms);

    const auto firstPayload = std::string(950, 'L');
    session->send(firstPayload);
    {
        std::unique_lock lock(received.mutex);
        assert(received.cv.wait_for(lock, 2s, [&] {
            return received.onB.size() >= 1;
        }));
        assert(received.onB[0] == firstPayload);
    }
    assert(proxy.dataAboveInitialFrames() >= 1);

    mini::net::udp::PathMtuFailure invalidFailure;
    invalidFailure.peerAddr = peerAddress;
    invalidFailure.failedDatagramPayloadSize = 1200;
    invalidFailure.suggestedDatagramPayloadSize = 900;
    invalidFailure.errorCode = EMSGSIZE;
    invalidFailure.source = mini::net::udp::PathMtuSignalSource::kRawIcmp;
    invalidFailure.quotedUdpPayloadPrefix =
        makeQuotedKcpFramePrefix(session->sessionId() + 1);
    transportA.notifyPathMtuFailure(invalidFailure);

    std::promise<void> invalidSignalDrainedPromise;
    auto invalidSignalDrainedFuture = invalidSignalDrainedPromise.get_future();
    loop->queueInLoop([&] { invalidSignalDrainedPromise.set_value(); });
    assert(invalidSignalDrainedFuture.wait_for(2s) == std::future_status::ready);

    const auto dataAboveAfterInvalidSignal = proxy.dataAboveInitialFrames();
    const auto secondPayload = std::string(950, 'M');
    session->send(secondPayload);
    {
        std::unique_lock lock(received.mutex);
        assert(received.cv.wait_for(lock, 2s, [&] {
            return received.onB.size() >= 2;
        }));
        assert(received.onB[1] == secondPayload);
    }
    assert(proxy.dataAboveInitialFrames() > dataAboveAfterInvalidSignal);

    mini::net::udp::PathMtuFailure validFailure;
    validFailure.peerAddr = peerAddress;
    validFailure.failedDatagramPayloadSize = 1200;
    validFailure.suggestedDatagramPayloadSize = 900;
    validFailure.errorCode = EMSGSIZE;
    validFailure.source = mini::net::udp::PathMtuSignalSource::kRawIcmp;
    validFailure.quotedUdpPayloadPrefix =
        makeQuotedKcpFramePrefix(session->sessionId());
    transportA.notifyPathMtuFailure(validFailure);

    std::promise<void> validSignalDrainedPromise;
    auto validSignalDrainedFuture = validSignalDrainedPromise.get_future();
    loop->queueInLoop([&] { validSignalDrainedPromise.set_value(); });
    assert(validSignalDrainedFuture.wait_for(2s) == std::future_status::ready);

    const auto dataAboveAfterValidSignal = proxy.dataAboveInitialFrames();
    const auto thirdPayload = std::string(950, 'N');
    session->send(thirdPayload);
    {
        std::unique_lock lock(received.mutex);
        assert(received.cv.wait_for(lock, 2s, [&] {
            return received.onB.size() >= 3;
        }));
        assert(received.onB[2] == thirdPayload);
    }

    assert(proxy.dataAboveInitialFrames() == dataAboveAfterValidSignal);
    assert(proxy.oversizedDataFrames() == 0);

    std::promise<void> stoppedPromise;
    auto stoppedFuture = stoppedPromise.get_future();
    loop->queueInLoop([&] {
        transportA.closeSession(session->sessionId());
        transportB.closeSession(session->sessionId());
        transportA.stop();
        transportB.stop();
        stoppedPromise.set_value();
    });
    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);
    proxy.stop();
}

void test_kcp_mtu_probe_failure_keeps_safe_fragment_size() {
    const auto portA = allocatePort();
    const auto portB = allocatePort();
    MtuProbeUdpProxy proxy(portA, portB, 950, 900);
    proxy.start();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::net::kcp::KcpTransportOptions options;
    options.initialRto = 10ms;
    options.maxRto = 30ms;
    options.maxRetransmissions = 4;
    options.enableMtuProbing = true;
    options.minDatagramPayloadSize = 900;
    options.maxDatagramPayloadSize = 1200;
    options.mtuProbeStepBytes = 300;
    options.mtuProbeMaxRetries = 1;
    options.mtuProbeInterval = 10ms;

    mini::net::kcp::KcpTransport transportA(
        loop,
        mini::net::InetAddress(portA, true),
        "contract-kcp-mtu-failure-a",
        true,
        options);
    mini::net::kcp::KcpTransport transportB(
        loop,
        mini::net::InetAddress(portB, true),
        "contract-kcp-mtu-failure-b");

    ReceivedState received;
    std::promise<std::shared_ptr<mini::net::kcp::KcpSession>> sessionPromise;
    auto sessionFuture = sessionPromise.get_future();

    loop->queueInLoop([&] {
        transportB.setMessageCallback(
            [&](mini::net::transport::TransportSessionId,
                std::string_view packet,
                const mini::net::InetAddress&) {
                {
                    std::lock_guard lock(received.mutex);
                    received.onB.emplace_back(packet);
                }
                received.cv.notify_all();
            });

        transportA.start();
        transportB.start();

        sessionPromise.set_value(
            transportA.openSession(mini::net::InetAddress("127.0.0.1", proxy.proxyPort())));
    });

    const auto session = sessionFuture.get();
    assert(session != nullptr);
    assert(wait_for_predicate([&] { return proxy.droppedProbeFrames() >= 2; }, 2s));
    std::this_thread::sleep_for(50ms);

    const auto payload = std::string(950, 'S');
    session->send(payload);

    {
        std::unique_lock lock(received.mutex);
        assert(received.cv.wait_for(lock, 2s, [&] {
            return received.onB.size() >= 1;
        }));
        assert(received.onB.size() == 1);
        assert(received.onB.front() == payload);
    }

    assert(proxy.probeFrames() >= 2);
    assert(proxy.droppedProbeFrames() >= 2);
    assert(proxy.probeAckFrames() == 0);
    assert(proxy.dataAboveInitialFrames() == 0);
    assert(proxy.oversizedDataFrames() == 0);

    std::promise<void> stoppedPromise;
    auto stoppedFuture = stoppedPromise.get_future();
    loop->queueInLoop([&] {
        transportA.closeSession(session->sessionId());
        transportA.stop();
        transportB.stop();
        stoppedPromise.set_value();
    });
    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);
    proxy.stop();
}

void test_kcp_mtu_path_cache_carries_blackhole_cooldown_to_reopened_session() {
    const auto portA = allocatePort();
    const auto portB = allocatePort();
    MtuProbeUdpProxy proxy(portA, portB, 950, 900);
    proxy.start();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::net::kcp::KcpTransportOptions options;
    options.initialRto = 10ms;
    options.maxRto = 30ms;
    options.maxRetransmissions = 4;
    options.enableMtuProbing = true;
    options.enableMtuPathCache = true;
    options.minDatagramPayloadSize = 900;
    options.maxDatagramPayloadSize = 1200;
    options.mtuProbeStepBytes = 300;
    options.mtuProbeMaxRetries = 1;
    options.mtuProbeInterval = 10ms;
    options.mtuProbeBlackholeCooldown = 1200ms;

    mini::net::kcp::KcpTransport transportA(
        loop,
        mini::net::InetAddress(portA, true),
        "contract-kcp-mtu-path-cache-blackhole-a",
        true,
        options);
    mini::net::kcp::KcpTransport transportB(
        loop,
        mini::net::InetAddress(portB, true),
        "contract-kcp-mtu-path-cache-blackhole-b");

    ReceivedState received;
    std::promise<std::shared_ptr<mini::net::kcp::KcpSession>> firstSessionPromise;
    auto firstSessionFuture = firstSessionPromise.get_future();

    loop->queueInLoop([&] {
        transportB.setMessageCallback(
            [&](mini::net::transport::TransportSessionId,
                std::string_view packet,
                const mini::net::InetAddress&) {
                {
                    std::lock_guard lock(received.mutex);
                    received.onB.emplace_back(packet);
                }
                received.cv.notify_all();
            });

        transportA.start();
        transportB.start();

        firstSessionPromise.set_value(
            transportA.openSession(mini::net::InetAddress("127.0.0.1", proxy.proxyPort())));
    });

    const auto firstSession = firstSessionFuture.get();
    assert(firstSession != nullptr);
    assert(wait_for_predicate([&] { return proxy.droppedProbeFrames() >= 2; }, 2s));
    std::this_thread::sleep_for(80ms);

    std::promise<void> closePromise;
    auto closeFuture = closePromise.get_future();
    loop->queueInLoop([&] {
        transportA.closeSession(firstSession->sessionId());
        closePromise.set_value();
    });
    assert(closeFuture.wait_for(2s) == std::future_status::ready);

    const auto droppedAfterFirstSession = proxy.droppedProbeFrames();

    std::promise<std::shared_ptr<mini::net::kcp::KcpSession>> secondSessionPromise;
    auto secondSessionFuture = secondSessionPromise.get_future();
    loop->queueInLoop([&] {
        secondSessionPromise.set_value(
            transportA.openSession(mini::net::InetAddress("127.0.0.1", proxy.proxyPort())));
    });

    const auto secondSession = secondSessionFuture.get();
    assert(secondSession != nullptr);
    std::this_thread::sleep_for(150ms);
    assert(proxy.droppedProbeFrames() == droppedAfterFirstSession);

    const auto payload = std::string(950, 'P');
    secondSession->send(payload);

    {
        std::unique_lock lock(received.mutex);
        assert(received.cv.wait_for(lock, 2s, [&] {
            return received.onB.size() >= 1;
        }));
        assert(received.onB.size() == 1);
        assert(received.onB.front() == payload);
    }

    assert(proxy.probeAckFrames() == 0);
    assert(proxy.dataAboveInitialFrames() == 0);
    assert(proxy.oversizedDataFrames() == 0);

    std::promise<void> stoppedPromise;
    auto stoppedFuture = stoppedPromise.get_future();
    loop->queueInLoop([&] {
        transportA.closeSession(secondSession->sessionId());
        transportA.stop();
        transportB.stop();
        stoppedPromise.set_value();
    });
    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);
    proxy.stop();
}

void test_kcp_mtu_blackhole_cooldown_suppresses_immediate_reprobe() {
    const auto portA = allocatePort();
    const auto portB = allocatePort();
    MtuProbeUdpProxy proxy(portA, portB, 950, 900);
    proxy.start();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::net::kcp::KcpTransportOptions options;
    options.initialRto = 10ms;
    options.maxRto = 30ms;
    options.maxRetransmissions = 4;
    options.enableMtuProbing = true;
    options.minDatagramPayloadSize = 900;
    options.maxDatagramPayloadSize = 1200;
    options.mtuProbeStepBytes = 300;
    options.mtuProbeMaxRetries = 1;
    options.mtuProbeInterval = 10ms;
    options.mtuProbeBlackholeCooldown = 1200ms;

    mini::net::kcp::KcpTransport transportA(
        loop,
        mini::net::InetAddress(portA, true),
        "contract-kcp-mtu-blackhole-cooldown-a",
        true,
        options);
    mini::net::kcp::KcpTransport transportB(
        loop,
        mini::net::InetAddress(portB, true),
        "contract-kcp-mtu-blackhole-cooldown-b");

    ReceivedState received;
    std::promise<std::shared_ptr<mini::net::kcp::KcpSession>> sessionPromise;
    auto sessionFuture = sessionPromise.get_future();

    loop->queueInLoop([&] {
        transportB.setMessageCallback(
            [&](mini::net::transport::TransportSessionId,
                std::string_view packet,
                const mini::net::InetAddress&) {
                {
                    std::lock_guard lock(received.mutex);
                    received.onB.emplace_back(packet);
                }
                received.cv.notify_all();
            });

        transportA.start();
        transportB.start();

        sessionPromise.set_value(
            transportA.openSession(mini::net::InetAddress("127.0.0.1", proxy.proxyPort())));
    });

    const auto session = sessionFuture.get();
    assert(session != nullptr);
    assert(wait_for_predicate([&] { return proxy.droppedProbeFrames() >= 2; }, 2s));
    std::this_thread::sleep_for(80ms);
    const auto afterBlackhole = proxy.droppedProbeFrames();
    std::this_thread::sleep_for(150ms);
    assert(proxy.droppedProbeFrames() == afterBlackhole);

    const auto payload = std::string(950, 'B');
    session->send(payload);

    {
        std::unique_lock lock(received.mutex);
        assert(received.cv.wait_for(lock, 2s, [&] {
            return received.onB.size() >= 1;
        }));
        assert(received.onB.size() == 1);
        assert(received.onB.front() == payload);
    }

    assert(proxy.probeAckFrames() == 0);
    assert(proxy.dataAboveInitialFrames() == 0);
    assert(proxy.oversizedDataFrames() == 0);
    assert(wait_for_predicate([&] { return proxy.droppedProbeFrames() > afterBlackhole; }, 4s));

    std::promise<void> stoppedPromise;
    auto stoppedFuture = stoppedPromise.get_future();
    loop->queueInLoop([&] {
        transportA.closeSession(session->sessionId());
        transportA.stop();
        transportB.stop();
        stoppedPromise.set_value();
    });
    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);
    proxy.stop();
}

void test_kcp_congestion_window_caps_initial_burst_and_drains_on_ack() {
    const auto portA = allocatePort();
    const auto portB = allocatePort();
    CongestionWindowUdpProxy proxy(portA, portB, 80ms);
    proxy.start();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::net::kcp::KcpTransportOptions options;
    options.initialRto = 200ms;
    options.maxRto = 400ms;
    options.maxRetransmissions = 4;
    options.enableCongestionWindow = true;
    options.minCongestionWindow = 3;
    options.initialCongestionWindow = 3;
    options.maxCongestionWindow = 3;

    mini::net::kcp::KcpTransport transportA(
        loop,
        mini::net::InetAddress(portA, true),
        "contract-kcp-cwnd-a",
        true,
        options);
    mini::net::kcp::KcpTransport transportB(
        loop,
        mini::net::InetAddress(portB, true),
        "contract-kcp-cwnd-b");

    std::vector<std::string> messages;
    for (int i = 0; i < 12; ++i) {
        messages.push_back("cwnd-" + std::to_string(i));
    }

    ReceivedState received;
    std::promise<mini::net::transport::TransportSessionId> sessionPromise;
    auto sessionFuture = sessionPromise.get_future();

    loop->queueInLoop([&] {
        transportB.setMessageCallback(
            [&](mini::net::transport::TransportSessionId,
                std::string_view packet,
                const mini::net::InetAddress&) {
                {
                    std::lock_guard lock(received.mutex);
                    received.onB.emplace_back(packet);
                }
                received.cv.notify_all();
            });

        transportA.start();
        transportB.start();

        auto session = transportA.openSession(
            mini::net::InetAddress("127.0.0.1", proxy.proxyPort()));
        if (!session) {
            sessionPromise.set_value(mini::net::transport::kInvalidTransportSessionId);
            return;
        }
        for (const auto& message : messages) {
            session->send(message);
        }
        sessionPromise.set_value(session->sessionId());
    });

    const auto sessionId = sessionFuture.get();
    assert(sessionId != mini::net::transport::kInvalidTransportSessionId);

    {
        std::unique_lock lock(received.mutex);
        assert(received.cv.wait_for(lock, 4s, [&] {
            return received.onB.size() >= messages.size();
        }));
        assert(received.onB.size() == messages.size());
        assert(received.onB == messages);
    }

    assert(proxy.dataFramesFromA() >= messages.size());
    assert(proxy.dataFramesBeforeFirstAck() == 3);
    assert(proxy.maxOutstandingFromA() <= 3);

    std::promise<void> stoppedPromise;
    auto stoppedFuture = stoppedPromise.get_future();
    loop->queueInLoop([&] {
        transportA.closeSession(sessionId);
        transportA.stop();
        transportB.stop();
        stoppedPromise.set_value();
    });
    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);
    proxy.stop();
}

void test_kcp_redundant_copies_cover_first_data_loss_without_rto() {
    const auto portA = allocatePort();
    const auto portB = allocatePort();
    RedundantCopyUdpProxy proxy(portA, portB, 80ms);
    proxy.start();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::net::kcp::KcpTransportOptions options;
    options.initialRto = 200ms;
    options.maxRto = 400ms;
    options.maxRetransmissions = 4;
    options.enableRedundantCopies = true;
    options.redundantCopyCount = 1;

    mini::net::kcp::KcpTransport transportA(
        loop,
        mini::net::InetAddress(portA, true),
        "contract-kcp-redundant-a",
        true,
        options);
    mini::net::kcp::KcpTransport transportB(
        loop,
        mini::net::InetAddress(portB, true),
        "contract-kcp-redundant-b");

    std::vector<std::string> messages;
    for (int i = 0; i < 8; ++i) {
        messages.push_back("redundant-" + std::to_string(i));
    }

    ReceivedState received;
    std::promise<mini::net::transport::TransportSessionId> sessionPromise;
    auto sessionFuture = sessionPromise.get_future();

    loop->queueInLoop([&] {
        transportB.setMessageCallback(
            [&](mini::net::transport::TransportSessionId,
                std::string_view packet,
                const mini::net::InetAddress&) {
                {
                    std::lock_guard lock(received.mutex);
                    received.onB.emplace_back(packet);
                }
                received.cv.notify_all();
            });

        transportA.start();
        transportB.start();

        auto session = transportA.openSession(
            mini::net::InetAddress("127.0.0.1", proxy.proxyPort()));
        if (!session) {
            sessionPromise.set_value(mini::net::transport::kInvalidTransportSessionId);
            return;
        }
        for (const auto& message : messages) {
            session->send(message);
        }
        sessionPromise.set_value(session->sessionId());
    });

    const auto sessionId = sessionFuture.get();
    assert(sessionId != mini::net::transport::kInvalidTransportSessionId);

    {
        std::unique_lock lock(received.mutex);
        assert(received.cv.wait_for(lock, 2s, [&] {
            return received.onB.size() >= messages.size();
        }));
        assert(received.onB.size() == messages.size());
        assert(received.onB == messages);
    }

    assert(proxy.droppedFirstDataFrames() == messages.size());
    assert(proxy.redundantDataFrames() >= messages.size());
    assert(proxy.dataFramesFromA() >= messages.size() * 2);
    assert(proxy.lateRetransmissionDataFrames() == 0);

    std::promise<void> stoppedPromise;
    auto stoppedFuture = stoppedPromise.get_future();
    loop->queueInLoop([&] {
        transportA.closeSession(sessionId);
        transportA.stop();
        transportB.stop();
        stoppedPromise.set_value();
    });
    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);
    proxy.stop();
}

void test_kcp_xor_parity_recovers_one_lost_packet_per_group_without_rto() {
    const auto portA = allocatePort();
    const auto portB = allocatePort();
    XorParityUdpProxy proxy(portA, portB, 4, 1, 120ms);
    proxy.start();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::net::kcp::KcpTransportOptions options;
    options.initialRto = 200ms;
    options.maxRto = 400ms;
    options.maxRetransmissions = 4;
    options.enableXorParityRecovery = true;
    options.xorParityGroupSize = 4;

    mini::net::kcp::KcpTransport transportA(
        loop,
        mini::net::InetAddress(portA, true),
        "contract-kcp-xor-parity-a",
        true,
        options);
    mini::net::kcp::KcpTransport transportB(
        loop,
        mini::net::InetAddress(portB, true),
        "contract-kcp-xor-parity-b",
        true,
        options);

    std::vector<std::string> messages;
    for (int i = 0; i < 8; ++i) {
        messages.push_back("xor-parity-" + std::to_string(i));
    }

    ReceivedState received;
    std::promise<mini::net::transport::TransportSessionId> sessionPromise;
    auto sessionFuture = sessionPromise.get_future();

    loop->queueInLoop([&] {
        transportB.setMessageCallback(
            [&](mini::net::transport::TransportSessionId,
                std::string_view packet,
                const mini::net::InetAddress&) {
                {
                    std::lock_guard lock(received.mutex);
                    received.onB.emplace_back(packet);
                }
                received.cv.notify_all();
            });

        transportA.start();
        transportB.start();

        auto session = transportA.openSession(
            mini::net::InetAddress("127.0.0.1", proxy.proxyPort()));
        if (!session) {
            sessionPromise.set_value(mini::net::transport::kInvalidTransportSessionId);
            return;
        }
        for (const auto& message : messages) {
            session->send(message);
        }
        sessionPromise.set_value(session->sessionId());
    });

    const auto sessionId = sessionFuture.get();
    assert(sessionId != mini::net::transport::kInvalidTransportSessionId);

    {
        std::unique_lock lock(received.mutex);
        assert(received.cv.wait_for(lock, 2s, [&] {
            return received.onB.size() >= messages.size();
        }));
        assert(received.onB.size() == messages.size());
        assert(received.onB == messages);
    }

    assert(proxy.droppedDataFrames() == 2);
    assert(proxy.parityFramesFromA() >= 2);
    assert(proxy.dataFramesFromA() >= messages.size());
    assert(proxy.lateRetransmissionDataFrames() == 0);

    std::promise<void> stoppedPromise;
    auto stoppedFuture = stoppedPromise.get_future();
    loop->queueInLoop([&] {
        transportA.closeSession(sessionId);
        transportA.stop();
        transportB.stop();
        stoppedPromise.set_value();
    });
    assert(stoppedFuture.wait_for(2s) == std::future_status::ready);
    proxy.stop();
}

void test_kcp_stop_and_session_close_are_concurrent_safe() {
    const auto port = allocatePort();
    const auto dropPort = allocatePort();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::net::kcp::KcpTransport transport(
        loop,
        mini::net::InetAddress(port, true),
        "contract-kcp-stop-close-race");
    transport.start();

    std::promise<std::shared_ptr<mini::net::kcp::KcpSession>> sessionPromise;
    auto sessionFuture = sessionPromise.get_future();
    loop->queueInLoop([&] {
        sessionPromise.set_value(
            transport.openSession(mini::net::InetAddress("127.0.0.1", dropPort)));
    });

    const auto session = sessionFuture.get();
    assert(session);
    assert(session->connected());

    std::promise<void> startPromise;
    auto startFuture = startPromise.get_future().share();

    auto sendFuture = std::async(std::launch::async, [session, startFuture] {
        startFuture.wait();
        for (int i = 0; i < 128; ++i) {
            session->send("race-send-" + std::to_string(i));
        }
    });

    auto closeFuture = std::async(std::launch::async, [session, startFuture] {
        startFuture.wait();
        session->forceClose();
    });

    auto stopFuture = std::async(std::launch::async, [&transport, startFuture] {
        startFuture.wait();
        transport.stop();
    });

    startPromise.set_value();
    assert(sendFuture.wait_for(2s) == std::future_status::ready);
    assert(closeFuture.wait_for(2s) == std::future_status::ready);
    assert(stopFuture.wait_for(2s) == std::future_status::ready);
    sendFuture.get();
    closeFuture.get();
    stopFuture.get();

    assert(!transport.started());
    assert(transport.sessionCount() == 0);
    assert(!session->connected());
    assert(!session->hasOwner());
    assert(session->getLoop() == nullptr);
}

}  // namespace

int main() {
    test_kcp_survives_loss_reorder_duplicate_and_jitter();
    test_kcp_long_run_survives_periodic_high_loss_with_tuned_retry_budget();
    test_kcp_selective_ack_suppresses_retransmit_for_out_of_order_packets();
    test_kcp_mtu_probe_success_allows_larger_single_frame();
    test_kcp_mtu_path_cache_reuses_confirmed_size_for_reopened_session();
    test_kcp_shared_mtu_path_cache_survives_transport_restart();
    test_kcp_shared_mtu_path_cache_carries_blackhole_cooldown_to_replacement_transport();
    test_kcp_path_mtu_failure_signal_downgrades_cached_size_for_reopened_session();
    test_kcp_authenticated_raw_icmp_path_mtu_signal_requires_matching_session_quote();
    test_kcp_mtu_probe_failure_keeps_safe_fragment_size();
    test_kcp_mtu_path_cache_carries_blackhole_cooldown_to_reopened_session();
    test_kcp_mtu_blackhole_cooldown_suppresses_immediate_reprobe();
    test_kcp_congestion_window_caps_initial_burst_and_drains_on_ack();
    test_kcp_redundant_copies_cover_first_data_loss_without_rto();
    test_kcp_xor_parity_recovers_one_lost_packet_per_group_without_rto();
    test_kcp_stop_and_session_close_are_concurrent_safe();
    return 0;
}
