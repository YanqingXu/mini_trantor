#include "mini/ws/WebSocketHandshake.h"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mini::ws {

namespace {

// RFC 6455 magic GUID
static constexpr const char* kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// Base64 encode
std::string base64Encode(const unsigned char* data, std::size_t len) {
    static constexpr const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(4 * ((len + 2) / 3));

    for (std::size_t i = 0; i < len; i += 3) {
        std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<std::uint32_t>(data[i + 2]);

        result.push_back(table[(n >> 18) & 0x3F]);
        result.push_back(table[(n >> 12) & 0x3F]);
        result.push_back((i + 1 < len) ? table[(n >> 6) & 0x3F] : '=');
        result.push_back((i + 2 < len) ? table[n & 0x3F] : '=');
    }
    return result;
}

// Case-insensitive string comparison
bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Check if a header value contains a token (case-insensitive, comma-separated)
bool headerContainsToken(const std::string& headerValue, const std::string& token) {
    // Simple check: look for the token case-insensitively
    std::string lower = headerValue;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    std::string lowerToken = token;
    std::transform(lowerToken.begin(), lowerToken.end(), lowerToken.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lower.find(lowerToken) != std::string::npos;
}

}  // namespace

bool isWebSocketUpgrade(const mini::http::HttpRequest& req) {
    // Must be GET
    if (req.method() != mini::http::HttpMethod::kGet) return false;

    // Must be HTTP/1.1
    if (req.version() != mini::http::HttpVersion::kHttp11) return false;

    // Must have Upgrade: websocket (case-insensitive)
    std::string upgrade = req.getHeader("Upgrade");
    if (!iequals(upgrade, "websocket")) return false;

    // Must have Connection: Upgrade (case-insensitive, may be comma-separated)
    std::string connection = req.getHeader("Connection");
    if (!headerContainsToken(connection, "upgrade")) return false;

    // Must have Sec-WebSocket-Key
    std::string key = req.getHeader("Sec-WebSocket-Key");
    if (key.empty()) return false;

    // Must have Sec-WebSocket-Version: 13
    std::string version = req.getHeader("Sec-WebSocket-Version");
    if (version != "13") return false;

    return true;
}

std::string computeAcceptKey(const std::string& clientKey) {
    if (clientKey.empty()) return {};

    std::string combined = clientKey + kWebSocketGuid;

    // SHA-1 using EVP API (OpenSSL 3.x compatible)
    unsigned char hash[20];
    unsigned int hashLen = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};

    if (EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, combined.data(), combined.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, hash, &hashLen) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }
    EVP_MD_CTX_free(ctx);

    return base64Encode(hash, hashLen);
}

std::string buildUpgradeResponse(const std::string& clientKey) {
    std::string accept = computeAcceptKey(clientKey);
    if (accept.empty()) return {};

    std::string response;
    response.reserve(256);
    response.append("HTTP/1.1 101 Switching Protocols\r\n");
    response.append("Upgrade: websocket\r\n");
    response.append("Connection: Upgrade\r\n");
    response.append("Sec-WebSocket-Accept: ");
    response.append(accept);
    response.append("\r\n\r\n");
    return response;
}

}  // namespace mini::ws
