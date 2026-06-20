#pragma once

// TransportTypes — 统一传输层的轻量类型与枚举。
//
// 为 TCP/TLS 优先链路预留了扩展位，后续可以自然加入 UDP/KCP 等传输类型。

#include <cstdint>

namespace mini::net {
class Buffer;
class EventLoop;
}

namespace mini::net::transport {

using TransportSessionId = std::uint64_t;
inline constexpr TransportSessionId kInvalidTransportSessionId = 0;
inline constexpr TransportSessionId kFirstTransportSessionId = 1;

// 未来可扩展传输类型：目前默认以 TCP 为主。
enum class TransportKind : std::uint8_t {
    kUnknown = 0,
    kTcp,
    kTcpTls,
    kUdp,
    kKcp
};

}  // namespace mini::net::transport

