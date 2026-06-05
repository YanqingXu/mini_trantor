#include "mini/http/HttpResponseContext.h"

#include <algorithm>
#include <cstring>
#include <cctype>
#include <stdexcept>

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

}  // anonymous namespace

namespace mini::http {

bool HttpResponseContext::parseStatusLine(const char* begin, const char* end) {
    // Format: "HTTP/1.x STATUS_CODE STATUS_MESSAGE\r\n"
    // Example: "HTTP/1.1 200 OK\r\n"

    if (static_cast<std::size_t>(end - begin) < 12) {
        return false;
    }

    if (std::memcmp(begin, "HTTP/1.", 7) != 0) {
        return false;
    }

    // Track version internally; HttpResponse doesn't need version for serialization
    version_ = (begin[7] == '1') ? HttpVersion::kHttp11 :
               (begin[7] == '0') ? HttpVersion::kHttp10 :
               HttpVersion::kUnknown;
    if (version_ == HttpVersion::kUnknown) {
        return false;
    }

    const char* p = begin + 8;
    if (*p != ' ') {
        return false;
    }
    ++p;

    if (p + 3 > end || !std::isdigit(static_cast<unsigned char>(p[0])) ||
        !std::isdigit(static_cast<unsigned char>(p[1])) ||
        !std::isdigit(static_cast<unsigned char>(p[2]))) {
        return false;
    }

    // Reject 4-digit status codes
    if (p + 3 < end && std::isdigit(static_cast<unsigned char>(p[3]))) {
        return false;
    }

    int code = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
    response_.setStatusCode(static_cast<HttpResponse::HttpStatusCode>(code));
    p += 3;

    if (p < end && *p == ' ') {
        ++p;
    }

    response_.setStatusMessage(std::string(p, end));
    return true;
}

bool HttpResponseContext::parseResponse(mini::net::Buffer* buf) {
    bool hasMore = true;

    while (hasMore) {
        if (state_ == kExpectStatusLine) {
            const char* crlf = std::search(
                buf->peek(), buf->peek() + buf->readableBytes(),
                "\r\n", "\r\n" + 2);
            if (crlf == buf->peek() + buf->readableBytes()) {
                if (buf->readableBytes() > kMaxHeaderSize) return false;
                hasMore = false;
            } else {
                if (!parseStatusLine(buf->peek(), crlf)) return false;
                buf->retrieveUntil(crlf + 2);
                state_ = kExpectHeaders;
            }
        } else if (state_ == kExpectHeaders) {
            const char* crlf = std::search(
                buf->peek(), buf->peek() + buf->readableBytes(),
                "\r\n", "\r\n" + 2);
            if (crlf == buf->peek() + buf->readableBytes()) {
                if (buf->readableBytes() > kMaxHeaderSize) return false;
                hasMore = false;
            } else {
                if (crlf == buf->peek()) {
                    buf->retrieveUntil(crlf + 2);

                    const auto& hs = response_.headers();
                    auto it = hs.find("Content-Length");
                    if (it != hs.end()) {
                        try { contentLength_ = std::stoull(it->second); }
                        catch (...) { return false; }
                    }

                    auto connIt = hs.find("Connection");
                    if (connIt != hs.end() && toLower(connIt->second) == "close") {
                        response_.setCloseConnection(true);
                    }

                    state_ = (contentLength_ > 0) ? kExpectBody : kGotAll;
                    if (state_ == kGotAll) hasMore = false;
                } else {
                    const char* colon = std::find(buf->peek(), crlf, ':');
                    if (colon == crlf) return false;

                    std::string key(buf->peek(), colon);
                    const char* valStart = colon + 1;
                    while (valStart < crlf && *valStart == ' ') ++valStart;
                    const char* valEnd = crlf;
                    while (valEnd > valStart && *(valEnd - 1) == ' ') --valEnd;

                    response_.addHeader(std::move(key), std::string(valStart, valEnd));
                    buf->retrieveUntil(crlf + 2);
                }
            }
        } else if (state_ == kExpectBody) {
            if (buf->readableBytes() >= contentLength_) {
                response_.setBody(buf->retrieveAsString(contentLength_));
                state_ = kGotAll;
                hasMore = false;
            } else {
                hasMore = false;
            }
        } else {
            hasMore = false;
        }
    }

    return true;
}

}  // namespace mini::http
