#include "mini/net/TlsContext.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <stdexcept>
#include <string>

namespace mini::net {

namespace {

struct OpenSslInitializer {
    OpenSslInitializer() {
        OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);
    }
};

void ensureOpenSslInit() {
    static OpenSslInitializer init;
}

std::string getOpenSslError() {
    unsigned long err = ERR_get_error();
    if (err == 0) {
        return "unknown OpenSSL error";
    }
    char buf[256];
    ERR_error_string_n(err, buf, sizeof(buf));
    return buf;
}

}  // namespace

TlsContext::TlsContext(SSL_CTX* ctx) : ctx_(ctx) {
}

TlsContext::~TlsContext() {
    if (ctx_) {
        SSL_CTX_free(ctx_);
    }
}

std::shared_ptr<TlsContext> TlsContext::newServerContext(
    const std::string& certPath,
    const std::string& keyPath) {
    ensureOpenSslInit();

    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        throw std::runtime_error("SSL_CTX_new failed: " + getOpenSslError());
    }

    // Set minimum TLS 1.2
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    if (SSL_CTX_use_certificate_chain_file(ctx, certPath.c_str()) != 1) {
        SSL_CTX_free(ctx);
        throw std::runtime_error("Failed to load certificate: " + getOpenSslError());
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, keyPath.c_str(), SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx);
        throw std::runtime_error("Failed to load private key: " + getOpenSslError());
    }

    if (SSL_CTX_check_private_key(ctx) != 1) {
        SSL_CTX_free(ctx);
        throw std::runtime_error("Private key does not match certificate: " + getOpenSslError());
    }

    return std::shared_ptr<TlsContext>(new TlsContext(ctx));
}

std::shared_ptr<TlsContext> TlsContext::newClientContext() {
    ensureOpenSslInit();

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        throw std::runtime_error("SSL_CTX_new failed: " + getOpenSslError());
    }

    // Set minimum TLS 1.2
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    // Load default CA certificates
    SSL_CTX_set_default_verify_paths(ctx);

    return std::shared_ptr<TlsContext>(new TlsContext(ctx));
}

void TlsContext::setCaCertPath(const std::string& caFile, const std::string& caPath) {
    const char* file = caFile.empty() ? nullptr : caFile.c_str();
    const char* path = caPath.empty() ? nullptr : caPath.c_str();
    if (SSL_CTX_load_verify_locations(ctx_, file, path) != 1) {
        throw std::runtime_error("Failed to load CA certificates: " + getOpenSslError());
    }
}

void TlsContext::setVerifyPeer(bool verify) {
    SSL_CTX_set_verify(ctx_, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
}

SSL_CTX* TlsContext::nativeHandle() const noexcept {
    return ctx_;
}

}  // namespace mini::net
