#pragma once

// HttpResponseContext is a per-connection incremental HTTP response parser.
// It reads from a Buffer and parses the status line, headers, and body,
// preserving state across multiple onMessage calls.
// Thread-safe only on the connection's owner loop.

#include "mini/http/HttpRequest.h"
#include "mini/http/HttpResponse.h"
#include "mini/net/Buffer.h"

#include <cstddef>

namespace mini::http {

class HttpResponseContext {
public:
    enum ParseState {
        kExpectStatusLine,
        kExpectHeaders,
        kExpectBody,
        kGotAll,
    };

    HttpResponseContext() = default;

    /// Parse data from buffer. Returns false if the response is malformed.
    bool parseResponse(mini::net::Buffer* buf);

    /// Whether a complete response has been parsed.
    bool gotAll() const noexcept { return state_ == kGotAll; }

    /// Get the parsed response.
    const HttpResponse& response() const noexcept { return response_; }
    HttpResponse& response() noexcept { return response_; }

    /// Reset for the next response on a keep-alive connection.
    void reset() {
        state_ = kExpectStatusLine;
        version_ = HttpVersion::kUnknown;
        response_.reset();
        response_.setCloseConnection(true);  // Default close=true; overridden by headers
        contentLength_ = 0;
    }

    ParseState state() const noexcept { return state_; }

private:
    bool parseStatusLine(const char* begin, const char* end);

    ParseState state_{kExpectStatusLine};
    HttpVersion version_{HttpVersion::kUnknown};
    HttpResponse response_{true};
    std::size_t contentLength_{0};

    static constexpr std::size_t kMaxHeaderSize = 8192;
};

}  // namespace mini::http
