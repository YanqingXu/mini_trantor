#pragma once

// HttpContext 是 per-connection 的 HTTP 增量解析器。
// 它从 Buffer 中逐步解析请求行、头部和正文，
// 跨多次 onMessage 调用保持解析状态。
// 只在连接的 owner loop 线程上访问。

#include "mini/http/HttpRequest.h"
#include "mini/net/Buffer.h"

#include <cstddef>

namespace mini::http {

class HttpContext {
public:
    enum ParseState {
        kExpectRequestLine,
        kExpectHeaders,
        kExpectBody,
        kGotAll,
    };

    HttpContext() = default;

    /// Parse data from buffer. Returns false if request is malformed.
    bool parseRequest(mini::net::Buffer* buf);

    /// Whether a complete request has been parsed.
    bool gotAll() const noexcept { return state_ == kGotAll; }

    /// Get the parsed request.
    const HttpRequest& request() const noexcept { return request_; }
    HttpRequest& request() noexcept { return request_; }

    /// Reset for the next request on a keep-alive connection.
    void reset() {
        state_ = kExpectRequestLine;
        request_.reset();
        contentLength_ = 0;
    }

    ParseState state() const noexcept { return state_; }

private:
    bool parseRequestLine(const char* begin, const char* end);

    ParseState state_{kExpectRequestLine};
    HttpRequest request_;
    std::size_t contentLength_{0};

    static constexpr std::size_t kMaxHeaderSize = 8192;
};

}  // namespace mini::http
