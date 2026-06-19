#include "mini/http/HttpClient.h"

#include "mini/net/EventLoop.h"
#include "mini/net/ProtocolConnectionAdapter.h"
#include "mini/net/Buffer.h"
#include "mini/net/InetAddress.h"
#include "mini/net/NetError.h"
#include "mini/net/TcpConnection.h"
#include "mini/http/HttpResponseContext.h"

#include <chrono>
#include <expected>
#include <sstream>
#include <utility>

namespace mini::http {

namespace {

std::string normalizePath(std::string path) {
    if (path.empty()) {
        return "/";
    }
    return path;
}

bool isTokenSafe(std::string_view value) {
    return value.find('\r') == std::string_view::npos &&
           value.find('\n') == std::string_view::npos;
}

}  // namespace

HttpClient::HttpClient(mini::net::EventLoop* loop,
                       const mini::net::InetAddress& serverAddr,
                       std::string name,
                       HttpClientOptions options)
    : loop_(loop),
      options_(std::move(options)),
      hostHeader_(serverAddr.toIpPort()),
      client_(loop, serverAddr, std::move(name),
              mini::net::TcpClientOptions{options_.connector, options_.enableRetry}) {
    options_.validate();
    if (options_.enableRetry) {
        client_.enableRetry();
    }

    client_.setConnectionCallback(
        [this](const mini::net::TcpConnectionPtr& conn) { onConnection(conn); });
    client_.setMessageCallback(
        [this](const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf) {
            onMessage(conn, buf);
        });
    client_.setConnectorEventCallback(
        [this](const mini::net::InetAddress& addr, mini::net::ConnectorEvent event) {
            onConnectorEvent(addr, event);
        });
}

void HttpClient::connect() {
    runInLoopAuto([this] { client_.connect(); });
}

void HttpClient::disconnect() {
    runInLoopAuto([this] {
        reconnectAfterClose_ = false;
        client_.disconnect();
        state_ = State::kIdle;
    });
}

void HttpClient::stop() {
    runInLoopAuto([this] {
        stopped_ = true;
        reconnectAfterClose_ = false;
        failAllPending(mini::net::NetError::NotConnected);
        client_.stop();
        state_ = State::kIdle;
        pendingRequests_.clear();
    });
}

bool HttpClient::connected() const {
    auto conn = client_.connection();
    return conn && conn->connected();
}

void HttpClient::runInLoopAuto(std::function<void()> fn) {
    if (loop_->isInLoopThread()) {
        fn();
    } else {
        loop_->queueInLoop(std::move(fn));
    }
}

void HttpClient::call(std::string method,
                     std::string path,
                     std::string body,
                     HttpHeaders headers,
                     HttpResponseCallback cb,
                     int timeoutMs) {
    runInLoopAuto([this, method = std::move(method), path = std::move(path), body = std::move(body),
                   headers = std::move(headers), timeoutMs, cb = std::move(cb)]() mutable {
        if (stopped_) {
            if (cb) {
                cb(std::unexpected(mini::net::NetError::NotConnected));
            }
            return;
        }

        if (!isTokenSafe(method) || !isTokenSafe(path)) {
            if (cb) {
                cb(std::unexpected(mini::net::NetError::ResolveFailed));
            }
            return;
        }

        PendingRequest req;
        req.id = nextRequestId_++;
        req.method = method;
        req.path = normalizePath(std::move(path));
        req.body = std::move(body);
        req.headers = std::move(headers);
        req.timeoutMs = effectiveTimeoutMs(timeoutMs);
        req.callback = std::move(cb);

        pendingRequests_.push_back(std::move(req));

        if (state_ == State::kWaitingResponse) {
            return;
        }
        startOrQueueRequest();
    });
}

void HttpClient::asyncGet(std::string path,
                         HttpHeaders headers,
                         HttpResponseCallback cb,
                         int timeoutMs) {
    call("GET", std::move(path), "", std::move(headers), std::move(cb), timeoutMs);
}

void HttpClient::asyncPost(std::string path,
                          std::string body,
                          HttpHeaders headers,
                          HttpResponseCallback cb,
                          int timeoutMs) {
    call("POST", std::move(path), std::move(body), std::move(headers), std::move(cb), timeoutMs);
}

void HttpClient::startOrQueueRequest() {
    if (stopped_) {
        return;
    }

    if (state_ == State::kWaitingResponse || state_ == State::kConnecting) {
        return;
    }

    const auto conn = client_.connection();
    if (!conn || !conn->connected()) {
        if (state_ != State::kConnecting) {
            state_ = State::kConnecting;
            client_.connect();
        }
        return;
    }

    sendCurrent();
}

void HttpClient::sendCurrent() {
    if (pendingRequests_.empty() || state_ == State::kWaitingResponse) {
        return;
    }

    const auto conn = client_.connection();
    if (!conn || !conn->connected()) {
        state_ = State::kConnecting;
        client_.connect();
        return;
    }

    auto request = buildRequestString(pendingRequests_.front());
    state_ = State::kWaitingResponse;

    if (pendingRequests_.front().timeoutMs > 0) {
        auto id = pendingRequests_.front().id;
        pendingRequests_.front().timeoutId = loop_->runAfter(
            std::chrono::milliseconds(pendingRequests_.front().timeoutMs),
            [this, id] { onTimeout(id); });
    }

    conn->send(request);
}

void HttpClient::onTimeout(std::uint64_t requestId) {
    if (pendingRequests_.empty()) {
        return;
    }
    if (pendingRequests_.front().id != requestId) {
        return;
    }
    failCurrentLocked(mini::net::NetError::TimedOut);
}

void HttpClient::failCurrentLocked(mini::net::NetError err) {
    if (pendingRequests_.empty()) {
        state_ = State::kIdle;
        return;
    }

    if (state_ == State::kWaitingResponse && pendingRequests_.front().timeoutId.valid()) {
        loop_->cancel(pendingRequests_.front().timeoutId);
        pendingRequests_.front().timeoutId = {};
    }
    auto req = std::move(pendingRequests_.front());
    pendingRequests_.pop_front();
    state_ = State::kIdle;

    if (req.callback) {
        req.callback(std::unexpected(err));
    }

    if (!pendingRequests_.empty()) {
        if (auto conn = client_.connection(); !conn || !conn->connected()) {
            client_.connect();
            state_ = State::kConnecting;
        } else {
            sendCurrent();
        }
    }
}

void HttpClient::failAllPending(mini::net::NetError err) {
    while (!pendingRequests_.empty()) {
        failCurrentLocked(err);
    }
}

void HttpClient::completeCurrentResponse(HttpResponse response, bool shouldClose) {
    if (pendingRequests_.empty()) {
        state_ = State::kIdle;
        return;
    }

    if (pendingRequests_.front().timeoutId.valid()) {
        loop_->cancel(pendingRequests_.front().timeoutId);
        pendingRequests_.front().timeoutId = {};
    }

    const bool needReconnectForNext = shouldClose && !pendingRequests_.empty() && !stopped_;

    auto req = std::move(pendingRequests_.front());
    pendingRequests_.pop_front();
    state_ = needReconnectForNext ? State::kConnecting : State::kIdle;

    if (req.callback) {
        req.callback(std::move(response));
    }

    if (needReconnectForNext) {
        reconnectAfterClose_ = true;
        client_.disconnect();
        return;
    }

    if (!pendingRequests_.empty()) {
        sendCurrent();
    }
}

void HttpClient::maybeReconnectAfterClose() {
    if (stopped_) {
        return;
    }

    const bool needReconnect = reconnectAfterClose_;
    reconnectAfterClose_ = false;

    if (!pendingRequests_.empty() && needReconnect) {
        state_ = State::kConnecting;
        client_.connect();
    } else if (pendingRequests_.empty()) {
        state_ = State::kIdle;
    } else if (options_.enableRetry) {
        state_ = State::kConnecting;
        client_.connect();
    } else {
        failAllPending(mini::net::NetError::NotConnected);
    }
}

std::string HttpClient::buildRequestString(const PendingRequest& req) const {
    HttpHeaders headers = req.headers;
    headers["Host"] = hostHeader_;
    headers["Connection"] = options_.enableKeepAlive ? "keep-alive" : "close";
    headers["Content-Length"] = std::to_string(req.body.size());

    std::string requestLine = req.method + " " + req.path + " HTTP/1.1\r\n";

    std::ostringstream out;
    out << requestLine;
    for (const auto& [key, value] : headers) {
        out << key << ": " << value << "\r\n";
    }
    out << "\r\n";
    out << req.body;
    return out.str();
}

void HttpClient::onConnection(const std::shared_ptr<mini::net::TcpConnection>& conn) {
    if (!conn->connected()) {
        if (state_ == State::kWaitingResponse && !reconnectAfterClose_) {
            failCurrentLocked(mini::net::NetError::ConnectionReset);
        }
        maybeReconnectAfterClose();

        if (userConnectionCallback_) {
            userConnectionCallback_(conn);
        }
        return;
    }

    // Bind parser state on first connect and reuse one connection per request sequence.
    auto adapter = mini::net::ProtocolConnectionAdapter::sharedFrom(conn);
    if (!adapter) {
        adapter = mini::net::ProtocolConnectionAdapter::createAndBind(conn);
    }
    adapter->setProtocolContext(HttpResponseContext());

    state_ = State::kIdle;
    if (userConnectionCallback_) {
        userConnectionCallback_(conn);
    }

    if (!pendingRequests_.empty()) {
        sendCurrent();
    } else {
        state_ = State::kIdle;
    }
}

void HttpClient::onMessage(const std::shared_ptr<mini::net::TcpConnection>& conn,
                          mini::net::Buffer* buf) {
    if (!conn->connected() || stopped_) {
        return;
    }

    auto adapter = mini::net::ProtocolConnectionAdapter::getFrom(conn);
    if (!adapter) {
        conn->forceClose();
        return;
    }

    auto* context = std::any_cast<HttpResponseContext>(&adapter->getProtocolContext());
    if (!context) {
        conn->forceClose();
        return;
    }

    if (!context->parseResponse(buf)) {
        failCurrentLocked(mini::net::NetError::ConnectionReset);
        conn->forceClose();
        return;
    }

    if (context->gotAll()) {
        auto response = context->response();
        context->reset();
        completeCurrentResponse(response, response.closeConnection());
        return;
    }
}

void HttpClient::onConnectorEvent(const mini::net::InetAddress&, mini::net::ConnectorEvent event) {
    if (stopped_) {
        return;
    }

    if (event == mini::net::ConnectorEvent::ConnectFailed ||
        event == mini::net::ConnectorEvent::ConnectTimeout) {
        if (!options_.enableRetry) {
            failAllPending(mini::net::NetError::NotConnected);
            state_ = State::kIdle;
        }
    }
}

HttpClient::CallAwaitable::CallAwaitable(HttpClient* client,
                      std::string method,
                      std::string path,
                      std::string body,
                      HttpHeaders headers,
                      int timeoutMs)
    : state_(std::make_shared<State>()) {
    state_->client = client;
    state_->method = std::move(method);
    state_->path = std::move(path);
    state_->body = std::move(body);
    state_->headers = std::move(headers);
    state_->timeoutMs = timeoutMs;
}

void HttpClient::CallAwaitable::await_suspend(std::coroutine_handle<> handle) {
    auto state = state_;
    state->client->call(state->method,
                        state->path,
                        state->body,
                        state->headers,
                        [state, handle](mini::net::Expected<HttpResponse> resp) mutable {
        state->result.emplace(std::move(resp));
        handle.resume();
    }, state->timeoutMs);
}

mini::net::Expected<HttpResponse> HttpClient::CallAwaitable::await_resume() {
    if (!state_ || !state_->result.has_value()) {
        return std::unexpected(mini::net::NetError::NotConnected);
    }
    return std::move(*state_->result);
}

HttpClient::CallAwaitable HttpClient::asyncRequest(std::string method,
                                                  std::string path,
                                                  std::string body,
                                                  HttpHeaders headers,
                                                  int timeoutMs) {
    return CallAwaitable(this,
                         std::move(method),
                         std::move(path),
                         std::move(body),
                         std::move(headers),
                         timeoutMs);
}

}  // namespace mini::http
