#include "mini/http/HttpServer.h"

#include "mini/http/HttpContext.h"
#include "mini/net/TcpConnection.h"

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

void HttpServer::onConnection(const mini::net::TcpConnectionPtr& conn) {
    if (conn->connected()) {
        conn->setContext(HttpContext());
    }
}

void HttpServer::onMessage(const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf) {
    auto* context = std::any_cast<HttpContext>(&conn->getContext());
    if (!context) {
        // Should not happen — context must have been set onConnection.
        conn->forceClose();
        return;
    }

    if (!context->parseRequest(buf)) {
        // Malformed request: send 400 and close.
        HttpResponse resp(true);
        resp.setStatusCode(HttpResponse::k400BadRequest);
        resp.setStatusMessage("Bad Request");
        resp.setCloseConnection(true);
        conn->send(resp.serialize());
        conn->shutdown();
        return;
    }

    if (context->gotAll()) {
        onRequest(conn, context->request());

        // Reset context for next request (keep-alive).
        context->reset();
    }
}

void HttpServer::onRequest(const mini::net::TcpConnectionPtr& conn, const HttpRequest& req) {
    const std::string& connection = req.getHeader("Connection");
    bool close = (connection == "close") ||
                 (req.version() == HttpVersion::kHttp10 && connection != "Keep-Alive");

    HttpResponse response(close);

    if (httpCallback_) {
        httpCallback_(req, &response);
    }

    conn->send(response.serialize());

    if (response.closeConnection()) {
        conn->shutdown();
    }
}

}  // namespace mini::http
