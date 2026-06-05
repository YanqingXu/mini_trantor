#include "mini/http/HttpServer.h"

#include "mini/http/HttpContext.h"
#include "mini/net/ProtocolConnectionAdapter.h"
#include "mini/net/TcpConnection.h"  // lifecycle: conn->connected()

#include <any>
#include <utility>

namespace mini::http {

HttpServer::HttpServer(mini::net::EventLoop* loop,
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

void HttpServer::start() {
    server_.start();
}

void HttpServer::stop() {
    server_.stop();
}

void HttpServer::onConnection(const mini::net::TcpConnectionPtr& conn) {
    if (conn->connected()) {
        auto adapter = mini::net::ProtocolConnectionAdapter::createAndBind(conn);
        adapter->setProtocolContext(HttpContext());
    }
}

void HttpServer::onMessage(const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf) {
    auto* adapter = mini::net::ProtocolConnectionAdapter::getFrom(conn);
    if (!adapter) {
        // Should not happen — adapter must have been set in onConnection.
        conn->forceClose();
        return;
    }

    auto* context = std::any_cast<HttpContext>(&adapter->getProtocolContext());
    if (!context) {
        adapter->forceClose();
        return;
    }

    if (!context->parseRequest(buf)) {
        // Malformed request: send 400 and close.
        HttpResponse resp(true);
        resp.setStatusCode(HttpResponse::k400BadRequest);
        resp.setStatusMessage("Bad Request");
        resp.setCloseConnection(true);
        adapter->send(resp.serialize());
        adapter->shutdown();
        return;
    }

    if (context->gotAll()) {
        onRequest(adapter, context->request());

        // Reset context for next request (keep-alive).
        context->reset();
    }
}

void HttpServer::onRequest(mini::net::IProtocolConnection* proto, const HttpRequest& req) {
    const std::string& connection = req.getHeader("Connection");
    bool close = (connection == "close") ||
                 (req.version() == HttpVersion::kHttp10 && connection != "Keep-Alive");

    HttpResponse response(close);

    if (httpCallback_) {
        httpCallback_(req, &response);
    }

    proto->send(response.serialize());

    if (response.closeConnection()) {
        proto->shutdown();
    }
}

}  // namespace mini::http
