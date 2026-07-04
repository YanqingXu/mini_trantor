#include "mini/net/udp/IcmpPathMtuListener.h"

#include <arpa/inet.h>
#include <cassert>
#include <array>
#include <cerrno>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

void append16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

void append32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

using Ipv6Bytes = std::array<unsigned char, 16>;

void appendIpv6(std::string& out, const Ipv6Bytes& address) {
    for (const auto byte : address) {
        out.push_back(static_cast<char>(byte));
    }
}

std::string buildIpv4Header(std::uint16_t totalLength,
                            std::uint8_t protocol,
                            std::uint32_t sourceIp,
                            std::uint32_t destIp) {
    std::string out;
    out.reserve(20);
    out.push_back(static_cast<char>(0x45));
    out.push_back('\0');
    append16(out, totalLength);
    append16(out, 1);
    append16(out, 0);
    out.push_back(static_cast<char>(64));
    out.push_back(static_cast<char>(protocol));
    append16(out, 0);
    append32(out, sourceIp);
    append32(out, destIp);
    return out;
}

std::string buildIpv6Header(std::uint16_t payloadLength,
                            std::uint8_t nextHeader,
                            const Ipv6Bytes& sourceIp,
                            const Ipv6Bytes& destIp) {
    std::string out;
    out.reserve(40);
    out.push_back(static_cast<char>(0x60));
    out.push_back('\0');
    out.push_back('\0');
    out.push_back('\0');
    append16(out, payloadLength);
    out.push_back(static_cast<char>(nextHeader));
    out.push_back(static_cast<char>(64));
    appendIpv6(out, sourceIp);
    appendIpv6(out, destIp);
    return out;
}

std::string buildPacketTooBig(std::uint16_t localPort,
                              std::uint16_t peerPort,
                              std::uint16_t pathMtu,
                              std::uint16_t udpPayloadSize,
                              std::string_view udpPayloadPrefix = {}) {
    constexpr std::uint32_t kLocalIp = 0x0A000001;  // 10.0.0.1
    constexpr std::uint32_t kPeerIp = 0xCB007105;   // 203.0.113.5
    constexpr std::uint32_t kRouterIp = 0x0A0000FE; // 10.0.0.254

    const auto udpLength = static_cast<std::uint16_t>(8 + udpPayloadSize);
    auto quotedIp = buildIpv4Header(
        static_cast<std::uint16_t>(20 + udpLength),
        17,
        kLocalIp,
        kPeerIp);
    append16(quotedIp, localPort);
    append16(quotedIp, peerPort);
    append16(quotedIp, udpLength);
    append16(quotedIp, 0);
    assert(udpPayloadPrefix.size() <= udpPayloadSize);
    if (!udpPayloadPrefix.empty()) {
        quotedIp.append(udpPayloadPrefix.data(), udpPayloadPrefix.size());
    }

    std::string icmp;
    icmp.push_back(static_cast<char>(3));
    icmp.push_back(static_cast<char>(4));
    append16(icmp, 0);
    append16(icmp, 0);
    append16(icmp, pathMtu);
    icmp.append(quotedIp);

    auto outer = buildIpv4Header(
        static_cast<std::uint16_t>(20 + icmp.size()),
        1,
        kRouterIp,
        kLocalIp);
    outer.append(icmp);
    return outer;
}

std::string buildIcmpv6PacketTooBig(std::uint16_t localPort,
                                    std::uint16_t peerPort,
                                    std::uint32_t pathMtu,
                                    std::uint16_t udpPayloadSize,
                                    bool includeOuterIpv6Header,
                                    std::string_view udpPayloadPrefix = {}) {
    constexpr Ipv6Bytes kLocalIp{
        0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 1};
    constexpr Ipv6Bytes kPeerIp{
        0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 5};
    constexpr Ipv6Bytes kRouterIp{
        0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0xFF};

    const auto udpLength = static_cast<std::uint16_t>(8 + udpPayloadSize);
    auto quotedIp = buildIpv6Header(udpLength, 17, kLocalIp, kPeerIp);
    append16(quotedIp, localPort);
    append16(quotedIp, peerPort);
    append16(quotedIp, udpLength);
    append16(quotedIp, 0);
    assert(udpPayloadPrefix.size() <= udpPayloadSize);
    if (!udpPayloadPrefix.empty()) {
        quotedIp.append(udpPayloadPrefix.data(), udpPayloadPrefix.size());
    }

    std::string icmp;
    icmp.push_back(static_cast<char>(2));
    icmp.push_back('\0');
    append16(icmp, 0);
    append32(icmp, pathMtu);
    icmp.append(quotedIp);

    if (!includeOuterIpv6Header) {
        return icmp;
    }

    auto outer = buildIpv6Header(
        static_cast<std::uint16_t>(icmp.size()),
        58,
        kRouterIp,
        kLocalIp);
    outer.append(icmp);
    return outer;
}

void testParseIpv4PacketTooBig() {
    const auto packet = buildPacketTooBig(41000, 42000, 1000, 1200);

    mini::net::udp::PathMtuFailure failure;
    assert(mini::net::udp::IcmpPathMtuListener::parseIpv4PacketTooBig(
        packet,
        41000,
        failure));

    assert(failure.errorCode == EMSGSIZE);
    assert(failure.peerAddr.toIp() == "203.0.113.5");
    assert(failure.peerAddr.port() == 42000);
    assert(failure.failedDatagramPayloadSize == 1200);
    assert(failure.suggestedDatagramPayloadSize == 972);
    assert(failure.source == mini::net::udp::PathMtuSignalSource::kRawIcmp);
    assert(failure.quotedUdpPayloadPrefix.empty());
}

void testParseIpv6PacketTooBig() {
    const auto packet = buildIcmpv6PacketTooBig(41000, 42000, 1280, 1400, false);

    mini::net::udp::PathMtuFailure failure;
    assert(mini::net::udp::IcmpPathMtuListener::parseIpv6PacketTooBig(
        packet,
        41000,
        failure));

    assert(failure.errorCode == EMSGSIZE);
    assert(failure.peerAddr.toIp() == "2001:db8::5");
    assert(failure.peerAddr.port() == 42000);
    assert(failure.failedDatagramPayloadSize == 1400);
    assert(failure.suggestedDatagramPayloadSize == 1232);
    assert(failure.source == mini::net::udp::PathMtuSignalSource::kRawIcmp);
    assert(failure.quotedUdpPayloadPrefix.empty());
}

void testParseIpv6PacketTooBigWithOuterIpHeader() {
    const auto packet = buildIcmpv6PacketTooBig(41000, 42000, 1400, 1500, true);

    mini::net::udp::PathMtuFailure failure;
    assert(mini::net::udp::IcmpPathMtuListener::parseIpv6PacketTooBig(
        packet,
        41000,
        failure));

    assert(failure.peerAddr.toIp() == "2001:db8::5");
    assert(failure.peerAddr.port() == 42000);
    assert(failure.failedDatagramPayloadSize == 1500);
    assert(failure.suggestedDatagramPayloadSize == 1352);
    assert(failure.source == mini::net::udp::PathMtuSignalSource::kRawIcmp);
}

void testParseIpv4PacketTooBigCarriesQuotedUdpPayloadPrefix() {
    const std::string quotedPrefix = "quoted-kcp-frame-prefix";
    const auto packet = buildPacketTooBig(41000, 42000, 1000, 1200, quotedPrefix);

    mini::net::udp::PathMtuFailure failure;
    assert(mini::net::udp::IcmpPathMtuListener::parseIpv4PacketTooBig(
        packet,
        41000,
        failure));

    assert(failure.source == mini::net::udp::PathMtuSignalSource::kRawIcmp);
    assert(failure.quotedUdpPayloadPrefix == quotedPrefix);
}

void testParseIpv6PacketTooBigCarriesQuotedUdpPayloadPrefix() {
    const std::string quotedPrefix = "quoted-kcp-v6-frame-prefix";
    const auto packet =
        buildIcmpv6PacketTooBig(41000, 42000, 1280, 1400, false, quotedPrefix);

    mini::net::udp::PathMtuFailure failure;
    assert(mini::net::udp::IcmpPathMtuListener::parseIpv6PacketTooBig(
        packet,
        41000,
        failure));

    assert(failure.source == mini::net::udp::PathMtuSignalSource::kRawIcmp);
    assert(failure.quotedUdpPayloadPrefix == quotedPrefix);
}

void testParseRejectsWrongLocalPort() {
    const auto packet = buildPacketTooBig(41000, 42000, 1000, 1200);

    mini::net::udp::PathMtuFailure failure;
    assert(!mini::net::udp::IcmpPathMtuListener::parseIpv4PacketTooBig(
        packet,
        41001,
        failure));
}

void testParseIpv6RejectsWrongLocalPort() {
    const auto packet = buildIcmpv6PacketTooBig(41000, 42000, 1280, 1400, false);

    mini::net::udp::PathMtuFailure failure;
    assert(!mini::net::udp::IcmpPathMtuListener::parseIpv6PacketTooBig(
        packet,
        41001,
        failure));
}

void testParseRejectsNonPacketTooBig() {
    auto packet = buildPacketTooBig(41000, 42000, 1000, 1200);
    packet[20 + 1] = static_cast<char>(3);

    mini::net::udp::PathMtuFailure failure;
    assert(!mini::net::udp::IcmpPathMtuListener::parseIpv4PacketTooBig(
        packet,
        41000,
        failure));
}

void testParseIpv6RejectsNonPacketTooBig() {
    auto packet = buildIcmpv6PacketTooBig(41000, 42000, 1280, 1400, false);
    packet[0] = static_cast<char>(1);

    mini::net::udp::PathMtuFailure failure;
    assert(!mini::net::udp::IcmpPathMtuListener::parseIpv6PacketTooBig(
        packet,
        41000,
        failure));
}

}  // namespace

int main() {
    testParseIpv4PacketTooBig();
    testParseIpv6PacketTooBig();
    testParseIpv6PacketTooBigWithOuterIpHeader();
    testParseIpv4PacketTooBigCarriesQuotedUdpPayloadPrefix();
    testParseIpv6PacketTooBigCarriesQuotedUdpPayloadPrefix();
    testParseRejectsWrongLocalPort();
    testParseIpv6RejectsWrongLocalPort();
    testParseRejectsNonPacketTooBig();
    testParseIpv6RejectsNonPacketTooBig();
    return 0;
}
