#pragma once

// TlsContext 封装 OpenSSL 的 SSL_CTX，提供服务端/客户端 TLS 上下文配置。
// 构造后为只读对象，可安全跨线程共享给多个 TcpConnection 使用。

#include <memory>
#include <string>

// Forward declarations for OpenSSL types
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

namespace mini::net {

class TlsContext {
public:
    ~TlsContext();

    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    /// Create a server-side TLS context with certificate and private key files.
    /// Throws std::runtime_error on failure.
    static std::shared_ptr<TlsContext> newServerContext(
        const std::string& certPath,
        const std::string& keyPath);

    /// Create a client-side TLS context.
    /// Throws std::runtime_error on failure.
    static std::shared_ptr<TlsContext> newClientContext();

    /// Set the CA certificate file or directory for peer verification.
    void setCaCertPath(const std::string& caFile, const std::string& caPath = "");

    /// Enable or disable peer certificate verification.
    void setVerifyPeer(bool verify);

    /// Get the underlying SSL_CTX pointer (for internal use by TcpConnection).
    SSL_CTX* nativeHandle() const noexcept;

private:
    explicit TlsContext(SSL_CTX* ctx);

    SSL_CTX* ctx_;
};

}  // namespace mini::net
