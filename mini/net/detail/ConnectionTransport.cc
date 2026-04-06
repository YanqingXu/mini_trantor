#include "mini/net/detail/ConnectionTransport.h"

#include "mini/base/Logger.h"
#include "mini/net/Buffer.h"
#include "mini/net/Channel.h"
#include "mini/net/Socket.h"
#include "mini/net/TlsContext.h"

#include <cerrno>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace mini::net::detail {

ConnectionTransport::~ConnectionTransport() {
    resetTls();
}

bool ConnectionTransport::enableTls(
    int fd,
    std::shared_ptr<TlsContext> ctx,
    bool isServer,
    std::string_view hostname) {
    resetTls();
    tlsContext_ = std::move(ctx);
    ssl_ = SSL_new(tlsContext_->nativeHandle());
    if (!ssl_) {
        LOG_ERROR << "ConnectionTransport::enableTls: SSL_new failed";
        tlsContext_.reset();
        return false;
    }

    SSL_set_fd(ssl_, fd);
    if (isServer) {
        SSL_set_accept_state(ssl_);
    } else {
        SSL_set_connect_state(ssl_);
        if (!hostname.empty()) {
            SSL_set_tlsext_host_name(ssl_, hostname.data());
        }
    }
    tlsState_ = kTlsHandshaking;
    return true;
}

bool ConnectionTransport::handshakePending() const noexcept {
    return ssl_ != nullptr && tlsState_ == kTlsHandshaking;
}

bool ConnectionTransport::isTlsEstablished() const noexcept {
    return tlsState_ == kTlsEstablished;
}

ConnectionTransport::HandshakeResult ConnectionTransport::advanceHandshake(Channel& channel) {
    if (!ssl_ || tlsState_ != kTlsHandshaking) {
        return {.completed = true};
    }

    ERR_clear_error();
    const int ret = SSL_do_handshake(ssl_);
    if (ret == 1) {
        tlsState_ = kTlsEstablished;
        channel.disableWriting();
        return {.completed = true};
    }

    const int err = SSL_get_error(ssl_, ret);
    if (err == SSL_ERROR_WANT_READ) {
        if (!channel.isReading()) {
            channel.enableReading();
        }
        return {};
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        channel.enableWriting();
        return {};
    }

    LOG_ERROR << "ConnectionTransport TLS handshake failed: SSL error " << err;
    return {.failed = true, .savedErrno = errno};
}

ConnectionTransport::ReadResult ConnectionTransport::readInto(Buffer& inputBuffer, Channel& channel) {
    if (ssl_) {
        return sslReadIntoBuffer(inputBuffer, channel);
    }

    int savedErrno = 0;
    const ssize_t n = inputBuffer.readFd(channel.fd(), &savedErrno);
    if (n > 0) {
        return {.status = Status::kOk, .bytes = static_cast<std::size_t>(n)};
    }
    if (n == 0) {
        return {.status = Status::kPeerClosed};
    }
    if (savedErrno == EWOULDBLOCK || savedErrno == EAGAIN) {
        return {.status = Status::kWouldBlock};
    }
    return {.status = Status::kError, .savedErrno = savedErrno};
}

ConnectionTransport::WriteResult ConnectionTransport::writeRaw(std::string_view data, Channel& channel) {
    if (ssl_) {
        return sslWriteRaw(data, channel);
    }

    const ssize_t nwrote = ::write(channel.fd(), data.data(), data.size());
    if (nwrote >= 0) {
        return {.status = Status::kOk, .bytes = static_cast<std::size_t>(nwrote)};
    }
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
        return {.status = Status::kWouldBlock};
    }
    return {.status = Status::kError, .savedErrno = errno};
}

ConnectionTransport::WriteResult ConnectionTransport::writeFromBuffer(Buffer& outputBuffer, Channel& channel) {
    if (ssl_) {
        return sslWriteFromBuffer(outputBuffer, channel);
    }

    int savedErrno = 0;
    const ssize_t n = outputBuffer.writeFd(channel.fd(), &savedErrno);
    if (n > 0) {
        outputBuffer.retrieve(static_cast<std::size_t>(n));
        return {.status = Status::kOk, .bytes = static_cast<std::size_t>(n)};
    }
    if (savedErrno == EWOULDBLOCK || savedErrno == EAGAIN) {
        return {.status = Status::kWouldBlock};
    }
    return {.status = Status::kError, .savedErrno = savedErrno};
}

void ConnectionTransport::shutdownWrite(Socket& socket) {
    if (ssl_ && tlsState_ == kTlsEstablished) {
        tlsState_ = kTlsShuttingDown;
        ERR_clear_error();
        SSL_shutdown(ssl_);
    }
    socket.shutdownWrite();
}

void ConnectionTransport::resetTls() noexcept {
    if (ssl_) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    tlsContext_.reset();
    tlsState_ = kTlsNone;
}

ConnectionTransport::ReadResult ConnectionTransport::sslReadIntoBuffer(Buffer& inputBuffer, Channel& channel) {
    ssize_t totalRead = 0;
    char buf[16384];

    while (true) {
        ERR_clear_error();
        const int n = SSL_read(ssl_, buf, sizeof(buf));
        if (n > 0) {
            inputBuffer.append(buf, static_cast<std::size_t>(n));
            totalRead += n;
            continue;
        }

        const int err = SSL_get_error(ssl_, n);
        if (err == SSL_ERROR_WANT_READ) {
            break;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            channel.enableWriting();
            break;
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            if (totalRead > 0) {
                break;
            }
            return {.status = Status::kPeerClosed};
        }

        if (totalRead > 0) {
            break;
        }
        return {.status = Status::kError, .savedErrno = errno};
    }

    if (totalRead > 0) {
        return {.status = Status::kOk, .bytes = static_cast<std::size_t>(totalRead)};
    }
    return {.status = Status::kWouldBlock};
}

ConnectionTransport::WriteResult ConnectionTransport::sslWriteRaw(std::string_view data, Channel& channel) {
    ERR_clear_error();
    const int n = SSL_write(ssl_, data.data(), static_cast<int>(data.size()));
    if (n > 0) {
        return {.status = Status::kOk, .bytes = static_cast<std::size_t>(n)};
    }

    const int err = SSL_get_error(ssl_, n);
    if (err == SSL_ERROR_WANT_WRITE) {
        return {.status = Status::kWouldBlock};
    }
    if (err == SSL_ERROR_WANT_READ) {
        channel.enableReading();
        return {.status = Status::kWouldBlock};
    }
    return {.status = Status::kError, .savedErrno = errno};
}

ConnectionTransport::WriteResult ConnectionTransport::sslWriteFromBuffer(Buffer& outputBuffer, Channel& channel) {
    std::size_t totalWritten = 0;

    while (outputBuffer.readableBytes() > 0) {
        ERR_clear_error();
        const int n = SSL_write(
            ssl_,
            outputBuffer.peek(),
            static_cast<int>(outputBuffer.readableBytes()));
        if (n > 0) {
            outputBuffer.retrieve(static_cast<std::size_t>(n));
            totalWritten += static_cast<std::size_t>(n);
            continue;
        }

        const int err = SSL_get_error(ssl_, n);
        if (err == SSL_ERROR_WANT_WRITE) {
            break;
        }
        if (err == SSL_ERROR_WANT_READ) {
            channel.enableReading();
            break;
        }

        if (totalWritten > 0) {
            break;
        }
        return {.status = Status::kError, .savedErrno = errno};
    }

    if (totalWritten > 0) {
        return {.status = Status::kOk, .bytes = totalWritten};
    }
    return {.status = Status::kWouldBlock};
}

}  // namespace mini::net::detail
