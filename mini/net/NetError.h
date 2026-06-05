#pragma once

// NetError 定义当前异步桥接层使用的显式错误类型。
// 配合 std::expected<T, NetError> 让调用者区分成功、对端关闭、
// 主动取消、解析失败和 I/O/状态错误。

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
    /// The pending async operation was actively cancelled.
    Cancelled,
    /// The operation did not complete before its timeout budget expired.
    TimedOut,
    /// Asynchronous hostname resolution failed.
    ResolveFailed,
};

constexpr std::string_view netErrorMessage(NetError e) noexcept {
    switch (e) {
        case NetError::PeerClosed: return "peer closed the connection";
        case NetError::ConnectionReset: return "connection reset";
        case NetError::NotConnected: return "not connected";
        case NetError::Cancelled: return "operation cancelled";
        case NetError::TimedOut: return "operation timed out";
        case NetError::ResolveFailed: return "hostname resolution failed";
    }
    return "unknown error";
}

/// Convenience alias for awaitable return types.
template <typename T>
using Expected = std::expected<T, NetError>;

}  // namespace mini::net
