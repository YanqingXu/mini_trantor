#include "mini/http/HttpResponse.h"

namespace mini::http {

std::string HttpResponse::serialize() const {
    std::string result;
    result.reserve(256 + body_.size());

    // Status line
    result.append("HTTP/1.1 ");
    result.append(std::to_string(static_cast<int>(statusCode_)));
    result.push_back(' ');
    result.append(statusMessage_);
    result.append("\r\n");

    // Headers
    if (closeConnection_) {
        result.append("Connection: close\r\n");
    } else {
        result.append("Connection: keep-alive\r\n");
    }

    // Content-Length (always, even for empty body)
    result.append("Content-Length: ");
    result.append(std::to_string(body_.size()));
    result.append("\r\n");

    for (const auto& [key, value] : headers_) {
        result.append(key);
        result.append(": ");
        result.append(value);
        result.append("\r\n");
    }

    // End of headers
    result.append("\r\n");

    // Body
    result.append(body_);
    return result;
}

}  // namespace mini::http
