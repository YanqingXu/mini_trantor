#include "mini/net/udp/IcmpPathMtuListener.h"

#include "mini/base/Logger.h"
#include "mini/net/Channel.h"
#include "mini/net/EventLoop.h"
#include "mini/net/Socket.h"
#include "mini/net/SocketsOps.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace mini::net::udp {

namespace {

constexpr std::size_t kRawPacketBufferSize = 2048;
constexpr std::size_t kMaxIcmpPacketsPerRead = 16;
constexpr std::uint8_t kIpv4Version = 4;
constexpr std::uint8_t kIpv6Version = 6;
constexpr std::uint8_t kIcmpDestinationUnreachable = 3;
constexpr std::uint8_t kIcmpFragmentationNeeded = 4;
constexpr std::uint8_t kIcmpv6PacketTooBig = 2;
constexpr std::uint8_t kIpProtocolUdp = 17;
constexpr std::uint8_t kIpProtocolIcmpv6 = 58;
constexpr std::size_t kIpv6HeaderSize = 40;
constexpr std::size_t kUdpHeaderSize = 8;

std::uint16_t read16(std::string_view data, std::size_t offset) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(static_cast<unsigned char>(data[offset])) << 8) |
        static_cast<std::uint16_t>(static_cast<unsigned char>(data[offset + 1])));
}

std::uint32_t read32(std::string_view data, std::size_t offset) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 3]));
}

std::size_t ipv4HeaderSize(std::string_view packet, std::size_t offset) {
    if (packet.size() <= offset) {
        return 0;
    }
    const auto versionAndIhl = static_cast<unsigned char>(packet[offset]);
    const auto version = static_cast<std::uint8_t>(versionAndIhl >> 4);
    const auto ihl = static_cast<std::size_t>(versionAndIhl & 0x0F) * 4;
    if (version != kIpv4Version || ihl < 20 || offset + ihl > packet.size()) {
        return 0;
    }
    return ihl;
}

std::size_t ipv6HeaderSize(std::string_view packet, std::size_t offset) {
    if (packet.size() < offset + kIpv6HeaderSize) {
        return 0;
    }
    const auto version = static_cast<std::uint8_t>(
        static_cast<unsigned char>(packet[offset]) >> 4);
    if (version != kIpv6Version) {
        return 0;
    }
    return kIpv6HeaderSize;
}

std::size_t udpPayloadSizeFromIpv4Mtu(std::uint16_t pathMtu) {
    if (pathMtu <= 20 + kUdpHeaderSize) {
        return 0;
    }
    return static_cast<std::size_t>(pathMtu) - 20 - kUdpHeaderSize;
}

std::size_t udpPayloadSizeFromIpv6Mtu(std::uint32_t pathMtu) {
    if (pathMtu <= kIpv6HeaderSize + kUdpHeaderSize) {
        return 0;
    }
    return static_cast<std::size_t>(pathMtu) - kIpv6HeaderSize - kUdpHeaderSize;
}

sockaddr_in makeIpv4Address(std::uint32_t networkAddress, std::uint16_t networkPort) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = networkAddress;
    addr.sin_port = networkPort;
    return addr;
}

sockaddr_in6 makeIpv6Address(const char* networkAddress, std::uint16_t networkPort) {
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    std::memcpy(&addr.sin6_addr, networkAddress, sizeof(addr.sin6_addr));
    addr.sin6_port = networkPort;
    return addr;
}

std::string quotedUdpPayloadPrefix(std::string_view packet, std::size_t offset) {
    if (offset >= packet.size()) {
        return {};
    }
    const auto length = std::min(
        packet.size() - offset,
        kMaxPathMtuQuotedUdpPayloadPrefix);
    return std::string(packet.substr(offset, length));
}

}  // namespace

IcmpPathMtuListener::IcmpPathMtuListener(EventLoop* loop,
                                         sa_family_t addressFamily,
                                         std::uint16_t localUdpPort,
                                         std::string name)
    : loop_(loop),
      addressFamily_(addressFamily),
      localUdpPort_(localUdpPort),
      name_(std::move(name)) {}

IcmpPathMtuListener::~IcmpPathMtuListener() {
    stop();
}

void IcmpPathMtuListener::setPathMtuFailureCallback(PathMtuFailureCallback cb) {
    pathMtuFailureCallback_ = std::move(cb);
}

bool IcmpPathMtuListener::start() {
    if (!loop_ || !loop_->isInLoopThread()) {
        return false;
    }
    if (started_) {
        return true;
    }

    if (addressFamily_ != AF_INET && addressFamily_ != AF_INET6) {
        return false;
    }

#ifdef __linux__
    int protocol = IPPROTO_ICMP;
    if (addressFamily_ == AF_INET6) {
        protocol = IPPROTO_ICMPV6;
    }
    const int fd = ::socket(addressFamily_, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
#else
    const int fd = -1;
    errno = EOPNOTSUPP;
#endif
    if (fd < 0) {
        available_ = false;
        if (errno != EPERM && errno != EACCES && errno != EPROTONOSUPPORT &&
            errno != EAFNOSUPPORT && errno != EOPNOTSUPP) {
            LOG_SYSERR << "IcmpPathMtuListener::start raw socket failed: "
                       << std::strerror(errno);
        }
        return false;
    }

    socket_ = std::make_unique<Socket>(fd);
    channel_ = std::make_unique<Channel>(loop_, fd);
    channel_->setReadCallback([this](mini::base::Timestamp receiveTime) {
        (void)receiveTime;
        handleRead();
    });
    channel_->enableReading();
    available_ = true;
    started_ = true;
    return true;
}

void IcmpPathMtuListener::stop() {
    if (!loop_ || !started_) {
        socket_.reset();
        channel_.reset();
        started_ = false;
        return;
    }
    if (!loop_->isInLoopThread()) {
        LOG_WARN << "IcmpPathMtuListener::stop called from non-owner loop thread";
        return;
    }

    if (channel_) {
        channel_->disableAll();
        channel_->remove();
    }
    channel_.reset();
    socket_.reset();
    started_ = false;
}

bool IcmpPathMtuListener::started() const noexcept {
    return started_;
}

bool IcmpPathMtuListener::available() const noexcept {
    return available_;
}

bool IcmpPathMtuListener::parseIpv4PacketTooBig(std::string_view packet,
                                                std::uint16_t localUdpPort,
                                                PathMtuFailure& out) {
    const auto outerIpHeaderSize = ipv4HeaderSize(packet, 0);
    if (outerIpHeaderSize == 0 || packet.size() < outerIpHeaderSize + 8) {
        return false;
    }

    const auto icmpOffset = outerIpHeaderSize;
    const auto type = static_cast<std::uint8_t>(static_cast<unsigned char>(packet[icmpOffset]));
    const auto code = static_cast<std::uint8_t>(static_cast<unsigned char>(packet[icmpOffset + 1]));
    if (type != kIcmpDestinationUnreachable || code != kIcmpFragmentationNeeded) {
        return false;
    }

    const auto nextHopMtu = read16(packet, icmpOffset + 6);
    const auto innerIpOffset = icmpOffset + 8;
    const auto innerIpHeaderSize = ipv4HeaderSize(packet, innerIpOffset);
    if (innerIpHeaderSize == 0 ||
        packet.size() < innerIpOffset + innerIpHeaderSize + kUdpHeaderSize) {
        return false;
    }

    const auto protocol =
        static_cast<std::uint8_t>(static_cast<unsigned char>(packet[innerIpOffset + 9]));
    if (protocol != kIpProtocolUdp) {
        return false;
    }

    const auto udpOffset = innerIpOffset + innerIpHeaderSize;
    const auto sourcePort = read16(packet, udpOffset);
    const auto destPort = read16(packet, udpOffset + 2);
    const auto udpLength = read16(packet, udpOffset + 4);
    if (sourcePort == 0 || destPort == 0 || udpLength < kUdpHeaderSize) {
        return false;
    }
    if (localUdpPort != 0 && sourcePort != localUdpPort) {
        return false;
    }

    std::uint32_t peerAddress = 0;
    std::memcpy(&peerAddress, packet.data() + innerIpOffset + 16, sizeof(peerAddress));
    out.peerAddr = InetAddress(makeIpv4Address(peerAddress, htons(destPort)));
    out.failedDatagramPayloadSize = static_cast<std::size_t>(udpLength - kUdpHeaderSize);
    out.suggestedDatagramPayloadSize = udpPayloadSizeFromIpv4Mtu(nextHopMtu);
    out.errorCode = EMSGSIZE;
    out.source = PathMtuSignalSource::kRawIcmp;
    out.quotedUdpPayloadPrefix = quotedUdpPayloadPrefix(packet, udpOffset + kUdpHeaderSize);
    return true;
}

bool IcmpPathMtuListener::parseIpv6PacketTooBig(std::string_view packet,
                                                std::uint16_t localUdpPort,
                                                PathMtuFailure& out) {
    std::size_t icmpOffset = 0;
    const auto outerIpHeaderSize = ipv6HeaderSize(packet, 0);
    if (outerIpHeaderSize != 0) {
        const auto nextHeader =
            static_cast<std::uint8_t>(static_cast<unsigned char>(packet[6]));
        if (nextHeader != kIpProtocolIcmpv6 ||
            packet.size() < outerIpHeaderSize + 8) {
            return false;
        }
        icmpOffset = outerIpHeaderSize;
    }

    if (packet.size() < icmpOffset + 8) {
        return false;
    }

    const auto type = static_cast<std::uint8_t>(static_cast<unsigned char>(packet[icmpOffset]));
    const auto code = static_cast<std::uint8_t>(static_cast<unsigned char>(packet[icmpOffset + 1]));
    if (type != kIcmpv6PacketTooBig || code != 0) {
        return false;
    }

    const auto nextHopMtu = read32(packet, icmpOffset + 4);
    const auto innerIpOffset = icmpOffset + 8;
    const auto innerIpHeaderSize = ipv6HeaderSize(packet, innerIpOffset);
    if (innerIpHeaderSize == 0 ||
        packet.size() < innerIpOffset + innerIpHeaderSize + kUdpHeaderSize) {
        return false;
    }

    const auto protocol =
        static_cast<std::uint8_t>(static_cast<unsigned char>(packet[innerIpOffset + 6]));
    if (protocol != kIpProtocolUdp) {
        return false;
    }

    const auto udpOffset = innerIpOffset + innerIpHeaderSize;
    const auto sourcePort = read16(packet, udpOffset);
    const auto destPort = read16(packet, udpOffset + 2);
    const auto udpLength = read16(packet, udpOffset + 4);
    if (sourcePort == 0 || destPort == 0 || udpLength < kUdpHeaderSize) {
        return false;
    }
    if (localUdpPort != 0 && sourcePort != localUdpPort) {
        return false;
    }

    out.peerAddr = InetAddress(
        makeIpv6Address(packet.data() + innerIpOffset + 24, htons(destPort)));
    out.failedDatagramPayloadSize = static_cast<std::size_t>(udpLength - kUdpHeaderSize);
    out.suggestedDatagramPayloadSize = udpPayloadSizeFromIpv6Mtu(nextHopMtu);
    out.errorCode = EMSGSIZE;
    out.source = PathMtuSignalSource::kRawIcmp;
    out.quotedUdpPayloadPrefix = quotedUdpPayloadPrefix(packet, udpOffset + kUdpHeaderSize);
    return true;
}

void IcmpPathMtuListener::handleRead() {
    if (!socket_) {
        return;
    }

    for (std::size_t count = 0; count < kMaxIcmpPacketsPerRead; ++count) {
        std::array<char, kRawPacketBufferSize> buffer{};
        const ssize_t n = ::recv(socket_->fd(), buffer.data(), buffer.size(), 0);
        if (n < 0) {
            if (errno == EINTR) {
                --count;
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_SYSERR << "IcmpPathMtuListener::handleRead recv failed: "
                           << std::strerror(errno);
            }
            return;
        }
        if (n == 0) {
            return;
        }

        if (!pathMtuFailureCallback_) {
            continue;
        }

        PathMtuFailure failure;
        const std::string_view packet(buffer.data(), static_cast<std::size_t>(n));
        const bool parsed = addressFamily_ == AF_INET6
                                ? parseIpv6PacketTooBig(packet, localUdpPort_, failure)
                                : parseIpv4PacketTooBig(packet, localUdpPort_, failure);
        if (parsed) {
            pathMtuFailureCallback_(failure);
        }
    }
}

}  // namespace mini::net::udp
