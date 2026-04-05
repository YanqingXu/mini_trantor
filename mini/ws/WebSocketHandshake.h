#pragma once

// WebSocket 握手工具。
// 实现 RFC 6455 §4.2.2 的 Sec-WebSocket-Accept 计算：
//   Base64(SHA-1(Sec-WebSocket-Key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
// 使用 OpenSSL 进行 SHA-1 计算。

#include "mini/http/HttpRequest.h"

#include <string>

namespace mini::ws {

/// Check whether an HTTP request is a valid WebSocket upgrade request.
/// Validates: GET method, HTTP/1.1, Upgrade: websocket, Connection: Upgrade,
/// Sec-WebSocket-Key present, Sec-WebSocket-Version: 13.
bool isWebSocketUpgrade(const mini::http::HttpRequest& req);

/// Compute the Sec-WebSocket-Accept value from the client's Sec-WebSocket-Key.
/// Returns empty string on invalid input.
std::string computeAcceptKey(const std::string& clientKey);

/// Build the complete HTTP 101 Switching Protocols response string.
std::string buildUpgradeResponse(const std::string& clientKey);

}  // namespace mini::ws
