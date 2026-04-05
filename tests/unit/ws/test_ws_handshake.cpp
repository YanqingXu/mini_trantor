// Unit tests for WebSocket handshake validation and Sec-WebSocket-Accept computation.

#include "mini/ws/WebSocketHandshake.h"
#include "mini/http/HttpRequest.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace mini::ws;
using namespace mini::http;

int main() {
    // 1. computeAcceptKey — RFC 6455 §4.2.2 example
    //    Key: "dGhlIHNhbXBsZSBub25jZQ=="
    //    Expected Accept: "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
    {
        std::string accept = computeAcceptKey("dGhlIHNhbXBsZSBub25jZQ==");
        assert(accept == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
        std::printf("  PASS: computeAcceptKey RFC 6455 example\n");
    }

    // 2. computeAcceptKey — empty key returns empty
    {
        std::string accept = computeAcceptKey("");
        assert(accept.empty());
        std::printf("  PASS: computeAcceptKey empty key\n");
    }

    // 3. isWebSocketUpgrade — valid upgrade request
    {
        HttpRequest req;
        req.setMethod(HttpMethod::kGet);
        req.setVersion(HttpVersion::kHttp11);
        req.setPath("/ws");
        req.addHeader("Upgrade", "websocket");
        req.addHeader("Connection", "Upgrade");
        req.addHeader("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
        req.addHeader("Sec-WebSocket-Version", "13");

        assert(isWebSocketUpgrade(req));
        std::printf("  PASS: valid upgrade request\n");
    }

    // 4. isWebSocketUpgrade — missing Upgrade header
    {
        HttpRequest req;
        req.setMethod(HttpMethod::kGet);
        req.setVersion(HttpVersion::kHttp11);
        req.addHeader("Connection", "Upgrade");
        req.addHeader("Sec-WebSocket-Key", "key");
        req.addHeader("Sec-WebSocket-Version", "13");

        assert(!isWebSocketUpgrade(req));
        std::printf("  PASS: reject missing Upgrade header\n");
    }

    // 5. isWebSocketUpgrade — wrong method (POST)
    {
        HttpRequest req;
        req.setMethod(HttpMethod::kPost);
        req.setVersion(HttpVersion::kHttp11);
        req.addHeader("Upgrade", "websocket");
        req.addHeader("Connection", "Upgrade");
        req.addHeader("Sec-WebSocket-Key", "key");
        req.addHeader("Sec-WebSocket-Version", "13");

        assert(!isWebSocketUpgrade(req));
        std::printf("  PASS: reject POST method\n");
    }

    // 6. isWebSocketUpgrade — wrong version (12)
    {
        HttpRequest req;
        req.setMethod(HttpMethod::kGet);
        req.setVersion(HttpVersion::kHttp11);
        req.addHeader("Upgrade", "websocket");
        req.addHeader("Connection", "Upgrade");
        req.addHeader("Sec-WebSocket-Key", "key");
        req.addHeader("Sec-WebSocket-Version", "12");

        assert(!isWebSocketUpgrade(req));
        std::printf("  PASS: reject wrong WebSocket version\n");
    }

    // 7. isWebSocketUpgrade — missing Sec-WebSocket-Key
    {
        HttpRequest req;
        req.setMethod(HttpMethod::kGet);
        req.setVersion(HttpVersion::kHttp11);
        req.addHeader("Upgrade", "websocket");
        req.addHeader("Connection", "Upgrade");
        req.addHeader("Sec-WebSocket-Version", "13");

        assert(!isWebSocketUpgrade(req));
        std::printf("  PASS: reject missing key\n");
    }

    // 8. isWebSocketUpgrade — case-insensitive Upgrade header
    {
        HttpRequest req;
        req.setMethod(HttpMethod::kGet);
        req.setVersion(HttpVersion::kHttp11);
        req.addHeader("Upgrade", "WebSocket");
        req.addHeader("Connection", "upgrade");
        req.addHeader("Sec-WebSocket-Key", "key");
        req.addHeader("Sec-WebSocket-Version", "13");

        assert(isWebSocketUpgrade(req));
        std::printf("  PASS: case-insensitive Upgrade\n");
    }

    // 9. buildUpgradeResponse — produces valid 101 response
    {
        std::string resp = buildUpgradeResponse("dGhlIHNhbXBsZSBub25jZQ==");
        assert(resp.find("HTTP/1.1 101 Switching Protocols\r\n") == 0);
        assert(resp.find("Upgrade: websocket\r\n") != std::string::npos);
        assert(resp.find("Connection: Upgrade\r\n") != std::string::npos);
        assert(resp.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos);
        assert(resp.find("\r\n\r\n") != std::string::npos);
        std::printf("  PASS: buildUpgradeResponse produces 101\n");
    }

    // 10. HTTP/1.0 rejected
    {
        HttpRequest req;
        req.setMethod(HttpMethod::kGet);
        req.setVersion(HttpVersion::kHttp10);
        req.addHeader("Upgrade", "websocket");
        req.addHeader("Connection", "Upgrade");
        req.addHeader("Sec-WebSocket-Key", "key");
        req.addHeader("Sec-WebSocket-Version", "13");

        assert(!isWebSocketUpgrade(req));
        std::printf("  PASS: reject HTTP/1.0\n");
    }

    std::printf("All WebSocket handshake unit tests passed.\n");
    return 0;
}
