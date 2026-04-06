#pragma once

// NetError 定义网络层协程 awaitable 的统一错误类型。
// 配合 std::expected<T, NetError> 使调用者能区分正常数据、对端关闭和 I/O 错误。

#include <expected>
#include <string>
#include <string_view>

namespace mini::net {

enum class NetError {
    /// Peer closed the connection (read returned 0, or already disconnected).
    PeerClosed,
    /// Connection was forcibly reset or encountered an I/O error.
    ConnectionReset,
    /// Connection is not in a valid state for this operation.
    NotConnected,
};

constexpr std::string_view netErrorMessage(NetError e) noexcept {
    switch (e) {
        case NetError::PeerClosed: return "peer closed the connection";
        case NetError::ConnectionReset: return "connection reset";
        case NetError::NotConnected: return "not connected";
    }
    return "unknown error";
}

/// Convenience alias for awaitable return types.
template <typename T>
using Expected = std::expected<T, NetError>;

}  // namespace mini::net
