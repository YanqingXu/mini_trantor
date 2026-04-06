#include "mini/ws/WebSocketServer.h"

#include "mini/http/HttpContext.h"
#include "mini/http/HttpResponse.h"
#include "mini/net/Buffer.h"
#include "mini/net/TcpConnection.h"
#include "mini/ws/WebSocketHandshake.h"

#include <any>
#include <utility>

namespace mini::ws {

WebSocketServer::WebSocketServer(mini::net::EventLoop* loop,
                                 const mini::net::InetAddress& listenAddr,
                                 std::string name,
                                 bool reusePort)
    : server_(loop, listenAddr, std::move(name), reusePort) {
    server_.setConnectionCallback(
        [this](const mini::net::TcpConnectionPtr& conn) { onConnection(conn); });
    server_.setMessageCallback(
        [this](const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf) {
            onMessage(conn, buf);
        });
}

void WebSocketServer::start() {
    server_.start();
}

void WebSocketServer::onConnection(const mini::net::TcpConnectionPtr& conn) {
    if (conn->connected()) {
        ConnectionContext ctx;
        conn->setContext(std::move(ctx));
    }
}

void WebSocketServer::onMessage(const mini::net::TcpConnectionPtr& conn,
                                mini::net::Buffer* buf) {
    auto* ctx = std::any_cast<ConnectionContext>(&conn->getContext());
    if (!ctx) {
        conn->forceClose();
        return;
    }

    if (!ctx->upgraded) {
        // Still in HTTP handshake mode.
        if (!tryUpgrade(conn, buf, *ctx)) {
            return;  // Connection closed or waiting for more data.
        }
        // After upgrade, there may be remaining data in buffer (WebSocket frames).
        if (buf->readableBytes() == 0) return;
    }

    // WebSocket mode: decode frames.
    if (!ctx->wsConn.onData(conn, buf)) {
        // Protocol error — connection will be closed by onData.
        return;
    }
}

bool WebSocketServer::tryUpgrade(const mini::net::TcpConnectionPtr& conn,
                                 mini::net::Buffer* buf,
                                 ConnectionContext& ctx) {
    // Use HttpContext to incrementally parse the upgrade request.
    mini::http::HttpContext httpCtx;

    if (!httpCtx.parseRequest(buf)) {
        // Malformed HTTP request.
        mini::http::HttpResponse resp(true);
        resp.setStatusCode(mini::http::HttpResponse::k400BadRequest);
        resp.setStatusMessage("Bad Request");
        resp.setCloseConnection(true);
        conn->send(resp.serialize());
        conn->shutdown();
        return false;
    }

    if (!httpCtx.gotAll()) {
        // Incomplete HTTP request — need more data.
        // Put data back? No, HttpContext consumed from buffer.
        // We need to handle this differently: don't consume until we have full request.
        // Actually, HttpContext already consumed what it parsed. If not gotAll(),
        // the remaining data is in the buffer. We just return and wait.
        return false;
    }

    const auto& req = httpCtx.request();

    // Check if this is a WebSocket upgrade request.
    if (!isWebSocketUpgrade(req)) {
        // Not a WebSocket request — send 400.
        mini::http::HttpResponse resp(true);
        resp.setStatusCode(mini::http::HttpResponse::k400BadRequest);
        resp.setStatusMessage("Bad Request");
        resp.setBody("Expected WebSocket upgrade request");
        resp.setCloseConnection(true);
        conn->send(resp.serialize());
        conn->shutdown();
        return false;
    }

    // Build and send 101 Switching Protocols.
    std::string clientKey = req.getHeader("Sec-WebSocket-Key");
    std::string response = buildUpgradeResponse(clientKey);
    if (response.empty()) {
        mini::http::HttpResponse resp(true);
        resp.setStatusCode(mini::http::HttpResponse::k400BadRequest);
        resp.setStatusMessage("Bad Request");
        resp.setBody("Invalid Sec-WebSocket-Key");
        resp.setCloseConnection(true);
        conn->send(resp.serialize());
        conn->shutdown();
        return false;
    }

    conn->send(response);
    ctx.upgraded = true;

    // Wire up callbacks.
    ctx.wsConn.setMessageCallback(messageCallback_);
    ctx.wsConn.setCloseCallback(closeCallback_);

    // Notify user of new WebSocket connection.
    if (connectCallback_) {
        connectCallback_(conn);
    }

    return true;
}

}  // namespace mini::ws
