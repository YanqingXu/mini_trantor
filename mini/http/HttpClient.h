#pragma once

// HttpClient 是 HTTP/1.1 客户端协议适配器。
// 它封装 TcpClient + HttpResponseContext，实现串行请求、连接重建、超时和协程接口。
//
// v6-alpha: 与现有线程模型一致，只通过 owner EventLoop 在单线程内更新客户端状态，
// 回调与 coroutine resume 均回到 owner loop。

#include "mini/base/noncopyable.h"
#include "mini/net/Callbacks.h"
#include "mini/http/HttpResponse.h"
#include "mini/net/ConnectorOptions.h"
#include "mini/net/NetError.h"
#include "mini/net/TcpClient.h"
#include "mini/net/TimerId.h"

#include <cstdint>
#include <coroutine>
#include <expected>
#include <functional>
#include <deque>
#include <memory>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mini::net {
class EventLoop;
class InetAddress;
class TcpConnection;
using ConnectionCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
}  // namespace mini::net

namespace mini::http {

struct HttpClientOptions {
    bool enableKeepAlive = true;            ///< Keep-Alive by default.
    int defaultTimeoutMs = 3000;            ///< 0 = no timeout.
    bool enableRetry = false;               ///< whether to enable TcpClient retry policy.
    mini::net::ConnectorOptions connector;  ///< embedded TcpClient connector options.

    void validate() const {
        if (defaultTimeoutMs < 0) {
            throw std::invalid_argument("defaultTimeoutMs must be non-negative");
        }
        connector.validate();
    }
};

/// HTTP response callback.
using HttpResponseCallback = std::function<void(mini::net::Expected<HttpResponse>)>;
/// User custom headers.
using HttpHeaders = std::map<std::string, std::string>;

class HttpClient : private mini::base::noncopyable {
public:
    /// Callback on connection state changes.
    using ConnectionCallback = mini::net::ConnectionCallback;

    HttpClient(mini::net::EventLoop* loop,
               const mini::net::InetAddress& serverAddr,
               std::string name,
               HttpClientOptions options = {});
    ~HttpClient() = default;

    /// Connect immediately and start retries according to options.
    void connect();

    /// Close current connection without auto-reconnect.
    void disconnect();

    /// Stop client and fail all pending requests.
    void stop();

    bool connected() const;

    void setConnectionCallback(ConnectionCallback cb) { userConnectionCallback_ = std::move(cb); }

    /// Core callback request API.
    /// method/path/body can be any UTF-8 strings.
    void call(std::string method,
              std::string path,
              std::string body,
              HttpHeaders headers,
              HttpResponseCallback cb,
              int timeoutMs = 0);

    /// Common HTTP helpers.
    void asyncGet(std::string path,
                  HttpHeaders headers,
                  HttpResponseCallback cb,
                  int timeoutMs = 0);

    void asyncPost(std::string path,
                   std::string body,
                   HttpHeaders headers,
                   HttpResponseCallback cb,
                   int timeoutMs = 0);

    /// Awaitable API.
    /// await_resume() returns `Expected<HttpResponse, NetError>`.
    class CallAwaitable {
    public:
        CallAwaitable(HttpClient* client,
                      std::string method,
                      std::string path,
                      std::string body,
                      HttpHeaders headers,
                      int timeoutMs);

        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> handle);
        mini::net::Expected<HttpResponse> await_resume();

    private:
        struct State {
            HttpClient* client{nullptr};
            std::string method;
            std::string path;
            std::string body;
            HttpHeaders headers;
            int timeoutMs{0};
            std::optional<mini::net::Expected<HttpResponse>> result;
        };

        std::shared_ptr<State> state_;
    };

    CallAwaitable asyncRequest(std::string method,
                               std::string path,
                               std::string body,
                               HttpHeaders headers,
                               int timeoutMs = 0);

private:
    struct PendingRequest {
        std::uint64_t id{0};
        std::string method;
        std::string path;
        std::string body;
        HttpHeaders headers;
        int timeoutMs{0};
        HttpResponseCallback callback;
        mini::net::TimerId timeoutId;
    };

    enum class State { kIdle, kConnecting, kWaitingResponse };

    void onConnection(const std::shared_ptr<mini::net::TcpConnection>& conn);
    void onMessage(const std::shared_ptr<mini::net::TcpConnection>& conn,
                   mini::net::Buffer* buf);
    void onConnectorEvent(const mini::net::InetAddress&, mini::net::ConnectorEvent);
    void runInLoopAuto(std::function<void()> fn);

    void startOrQueueRequest();
    void sendCurrent();
    void onTimeout(std::uint64_t requestId);
    void failCurrentLocked(mini::net::NetError err);
    void failAllPending(mini::net::NetError err);
    std::string buildRequestString(const PendingRequest& req) const;
    void completeCurrentResponse(HttpResponse response, bool shouldClose);
    void maybeReconnectAfterClose();
    int effectiveTimeoutMs(int requestedMs) const {
        return requestedMs > 0 ? requestedMs : options_.defaultTimeoutMs;
    }

    bool stopped_{false};
    bool reconnectAfterClose_{false};
    State state_{State::kIdle};

    mini::net::EventLoop* loop_;
    HttpClientOptions options_;
    std::string hostHeader_;
    mini::net::TcpClient client_;
    ConnectionCallback userConnectionCallback_;

    std::uint64_t nextRequestId_{1};
    std::deque<PendingRequest> pendingRequests_;
};

}  // namespace mini::http
