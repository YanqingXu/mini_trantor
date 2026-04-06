#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

typedef struct ssl_st SSL;

namespace mini::net {
class Buffer;
class Channel;
class Socket;
class TlsContext;
}

namespace mini::net::detail {

class ConnectionTransport {
public:
    enum class Status { kOk, kWouldBlock, kPeerClosed, kError };

    struct ReadResult {
        Status status{Status::kWouldBlock};
        std::size_t bytes{0};
        int savedErrno{0};
    };

    struct WriteResult {
        Status status{Status::kWouldBlock};
        std::size_t bytes{0};
        int savedErrno{0};
    };

    struct HandshakeResult {
        bool completed{false};
        bool failed{false};
        int savedErrno{0};
    };

    ConnectionTransport() = default;
    ~ConnectionTransport();

    bool enableTls(int fd, std::shared_ptr<TlsContext> ctx, bool isServer, std::string_view hostname);

    bool handshakePending() const noexcept;
    bool isTlsEstablished() const noexcept;

    HandshakeResult advanceHandshake(Channel& channel);
    ReadResult readInto(Buffer& inputBuffer, Channel& channel);
    WriteResult writeRaw(std::string_view data, Channel& channel);
    WriteResult writeFromBuffer(Buffer& outputBuffer, Channel& channel);
    void shutdownWrite(Socket& socket);

private:
    enum TlsState { kTlsNone, kTlsHandshaking, kTlsEstablished, kTlsShuttingDown };

    void resetTls() noexcept;
    ReadResult sslReadIntoBuffer(Buffer& inputBuffer, Channel& channel);
    WriteResult sslWriteRaw(std::string_view data, Channel& channel);
    WriteResult sslWriteFromBuffer(Buffer& outputBuffer, Channel& channel);

    std::shared_ptr<TlsContext> tlsContext_;
    SSL* ssl_{nullptr};
    TlsState tlsState_{kTlsNone};
};

}  // namespace mini::net::detail
