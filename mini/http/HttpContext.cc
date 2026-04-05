#include "mini/http/HttpContext.h"

#include <algorithm>
#include <cstring>

namespace mini::http {

bool HttpContext::parseRequestLine(const char* begin, const char* end) {
    // Format: "METHOD /path?query HTTP/1.x\r\n"
    const char* space = std::find(begin, end, ' ');
    if (space == end) return false;

    auto method = HttpRequest::parseMethod(std::string_view(begin, static_cast<std::size_t>(space - begin)));
    if (method == HttpMethod::kInvalid) return false;
    request_.setMethod(method);

    const char* pathStart = space + 1;
    space = std::find(pathStart, end, ' ');
    if (space == end) return false;

    // Split path and query at '?'
    const char* question = std::find(pathStart, space, '?');
    if (question != space) {
        request_.setPath(std::string(pathStart, question));
        request_.setQuery(std::string(question + 1, space));
    } else {
        request_.setPath(std::string(pathStart, space));
    }

    // Parse version
    const char* versionStart = space + 1;
    if (end - versionStart == 8) {
        if (std::memcmp(versionStart, "HTTP/1.1", 8) == 0) {
            request_.setVersion(HttpVersion::kHttp11);
        } else if (std::memcmp(versionStart, "HTTP/1.0", 8) == 0) {
            request_.setVersion(HttpVersion::kHttp10);
        } else {
            return false;
        }
    } else {
        return false;
    }

    return true;
}

bool HttpContext::parseRequest(mini::net::Buffer* buf) {
    bool hasMore = true;

    while (hasMore) {
        if (state_ == kExpectRequestLine) {
            // Look for \r\n in buffer.
            const char* crlf = std::search(
                buf->peek(), buf->peek() + buf->readableBytes(),
                "\r\n", "\r\n" + 2);
            if (crlf == buf->peek() + buf->readableBytes()) {
                // Not enough data yet.
                if (buf->readableBytes() > kMaxHeaderSize) {
                    return false;  // Request line too long.
                }
                hasMore = false;
            } else {
                if (!parseRequestLine(buf->peek(), crlf)) {
                    return false;
                }
                buf->retrieveUntil(crlf + 2);
                state_ = kExpectHeaders;
            }
        } else if (state_ == kExpectHeaders) {
            const char* crlf = std::search(
                buf->peek(), buf->peek() + buf->readableBytes(),
                "\r\n", "\r\n" + 2);
            if (crlf == buf->peek() + buf->readableBytes()) {
                if (buf->readableBytes() > kMaxHeaderSize) {
                    return false;  // Headers too large.
                }
                hasMore = false;
            } else {
                if (crlf == buf->peek()) {
                    // Empty line: end of headers.
                    buf->retrieveUntil(crlf + 2);

                    // Check for Content-Length.
                    std::string clStr = request_.getHeader("Content-Length");
                    if (!clStr.empty()) {
                        try {
                            contentLength_ = std::stoull(clStr);
                        } catch (...) {
                            return false;
                        }
                    }

                    if (contentLength_ > 0) {
                        state_ = kExpectBody;
                    } else {
                        state_ = kGotAll;
                        hasMore = false;
                    }
                } else {
                    // Parse header line: "Key: Value\r\n"
                    const char* colon = std::find(buf->peek(), crlf, ':');
                    if (colon == crlf) {
                        return false;  // Malformed header.
                    }
                    std::string key(buf->peek(), colon);

                    // Skip ': ' or ':' with optional spaces.
                    const char* valueStart = colon + 1;
                    while (valueStart < crlf && *valueStart == ' ') {
                        ++valueStart;
                    }
                    // Trim trailing spaces.
                    const char* valueEnd = crlf;
                    while (valueEnd > valueStart && *(valueEnd - 1) == ' ') {
                        --valueEnd;
                    }
                    std::string value(valueStart, valueEnd);

                    request_.addHeader(std::move(key), std::move(value));
                    buf->retrieveUntil(crlf + 2);
                }
            }
        } else if (state_ == kExpectBody) {
            if (buf->readableBytes() >= contentLength_) {
                request_.setBody(buf->retrieveAsString(contentLength_));
                state_ = kGotAll;
                hasMore = false;
            } else {
                hasMore = false;
            }
        } else {
            // kGotAll — shouldn't be called in this state.
            hasMore = false;
        }
    }

    return true;
}

}  // namespace mini::http
