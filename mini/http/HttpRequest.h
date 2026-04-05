#pragma once

// HttpRequest 是解析后的 HTTP/1.1 请求值对象。
// 它保存方法、路径、查询字符串、版本、头部和正文。
// 无线程亲和性，可自由复制和移动。

#include <map>
#include <string>
#include <string_view>

namespace mini::http {

enum class HttpMethod {
    kGet,
    kPost,
    kPut,
    kDelete,
    kHead,
    kOptions,
    kPatch,
    kInvalid
};

enum class HttpVersion {
    kHttp10,
    kHttp11,
    kUnknown
};

class HttpRequest {
public:
    HttpRequest() = default;

    void setMethod(HttpMethod method) noexcept { method_ = method; }
    HttpMethod method() const noexcept { return method_; }

    static HttpMethod parseMethod(std::string_view methodStr);
    const char* methodString() const;

    void setPath(std::string path) { path_ = std::move(path); }
    const std::string& path() const noexcept { return path_; }

    void setQuery(std::string query) { query_ = std::move(query); }
    const std::string& query() const noexcept { return query_; }

    void setVersion(HttpVersion version) noexcept { version_ = version; }
    HttpVersion version() const noexcept { return version_; }

    void addHeader(std::string key, std::string value) {
        headers_[std::move(key)] = std::move(value);
    }
    std::string getHeader(const std::string& key) const {
        auto it = headers_.find(key);
        return it != headers_.end() ? it->second : std::string{};
    }
    const std::map<std::string, std::string>& headers() const noexcept { return headers_; }

    void setBody(std::string body) { body_ = std::move(body); }
    const std::string& body() const noexcept { return body_; }

    void reset() {
        method_ = HttpMethod::kInvalid;
        path_.clear();
        query_.clear();
        version_ = HttpVersion::kUnknown;
        headers_.clear();
        body_.clear();
    }

private:
    HttpMethod method_{HttpMethod::kInvalid};
    std::string path_;
    std::string query_;
    HttpVersion version_{HttpVersion::kUnknown};
    std::map<std::string, std::string> headers_;
    std::string body_;
};

}  // namespace mini::http
