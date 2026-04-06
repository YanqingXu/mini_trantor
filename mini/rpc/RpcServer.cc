#include "mini/rpc/RpcServer.h"

#include "mini/net/Buffer.h"
#include "mini/net/TcpConnection.h"

#include <any>
#include <utility>

namespace {

// Free function (not lambda) to avoid dangling-capture-in-coroutine issues.
// All parameters are safely stored in the coroutine frame by value.
mini::coroutine::Task<void> dispatchCoroHandler(
    mini::rpc::RpcCoroHandler handler,
    std::string payload,
    std::function<void(std::string_view)> respond,
    std::function<void(std::string_view)> respondError) {
    try {
        auto task = handler(std::move(payload));
        std::string result = co_await std::move(task);
        respond(result);
    } catch (const std::exception& e) {
        respondError(e.what());
    }
}

}  // namespace

namespace mini::rpc {

RpcServer::RpcServer(mini::net::EventLoop* loop,
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

void RpcServer::registerMethod(std::string method, RpcMethodHandler handler) {
    methods_.emplace(std::move(method), std::move(handler));
}

void RpcServer::registerCoroMethod(std::string method, RpcCoroHandler handler) {
    coroMethods_.emplace(std::move(method), std::move(handler));
}

void RpcServer::start() {
    server_.start();
}

void RpcServer::onConnection(const mini::net::TcpConnectionPtr& conn) {
    if (conn->connected()) {
        auto channel = RpcChannel(conn->getLoop());

        // Capture methods_ by reference — RpcServer outlives connections.
        channel.setRequestCallback(
            [this](std::string_view method,
                   std::string_view payload,
                   std::function<void(std::string_view)> respond,
                   std::function<void(std::string_view)> respondError) {
                // Check coroutine handlers first.
                auto coroIt = coroMethods_.find(std::string(method));
                if (coroIt != coroMethods_.end()) {
                    dispatchCoroHandler(coroIt->second,
                                        std::string(payload),
                                        std::move(respond),
                                        std::move(respondError)).detach();
                    return;
                }
                // Then check callback handlers.
                auto it = methods_.find(std::string(method));
                if (it != methods_.end()) {
                    it->second(payload, std::move(respond), std::move(respondError));
                } else {
                    respondError("method not found: " + std::string(method));
                }
            });

        conn->setContext(std::move(channel));
    } else {
        // Connection closing: fail all pending calls.
        auto* channel = std::any_cast<RpcChannel>(&conn->getContext());
        if (channel) {
            channel->failAllPending("connection closed");
        }
    }
}

void RpcServer::onMessage(const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf) {
    auto* channel = std::any_cast<RpcChannel>(&conn->getContext());
    if (!channel) {
        conn->forceClose();
        return;
    }

    if (!channel->onMessage(conn, buf)) {
        // Malformed frame: close the connection.
        conn->forceClose();
    }
}

}  // namespace mini::rpc
