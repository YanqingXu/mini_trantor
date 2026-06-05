#pragma once

// HttpResponse 是 HTTP/1.1 响应构建器。
// 支持设置状态码、头部和正文，序列化为符合 HTTP/1.1 规范的字节流。
// 无线程亲和性，可自由复制和移动。

#include <map>
#include <string>

namespace mini::http {

class HttpResponse {
public:
    enum HttpStatusCode {
        k200Ok = 200,
        k204NoContent = 204,
        k301MovedPermanently = 301,
        k302Found = 302,
        k304NotModified = 304,
        k400BadRequest = 400,
        k403Forbidden = 403,
        k404NotFound = 404,
        k405MethodNotAllowed = 405,
        k500InternalServerError = 500,
    };

    explicit HttpResponse(bool close = false)
        : closeConnection_(close) {}

    void setStatusCode(HttpStatusCode code) noexcept { statusCode_ = code; }
    HttpStatusCode statusCode() const noexcept { return statusCode_; }

    void setStatusMessage(std::string message) { statusMessage_ = std::move(message); }
    const std::string& statusMessage() const noexcept { return statusMessage_; }

    void setCloseConnection(bool on) noexcept { closeConnection_ = on; }
    bool closeConnection() const noexcept { return closeConnection_; }

    void setContentType(std::string contentType) {
        addHeader("Content-Type", std::move(contentType));
    }

    void addHeader(std::string key, std::string value) {
        headers_[std::move(key)] = std::move(value);
    }

    void setBody(std::string body) { body_ = std::move(body); }
    const std::string& body() const noexcept { return body_; }

    /// Serialize the response to a string ready for sending.
    std::string serialize() const;

private:
    HttpStatusCode statusCode_{k200Ok};
    std::string statusMessage_{"OK"};
    bool closeConnection_{false};
    std::map<std::string, std::string> headers_;
    std::string body_;
};

}  // namespace mini::http
