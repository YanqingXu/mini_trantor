#include "mini/net/Buffer.h"
#include "mini/net/Channel.h"
#include "mini/net/EventLoop.h"
#include "mini/net/detail/ConnectionTransport.h"

#include <array>
#include <cassert>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

namespace {

std::array<int, 2> makeSocketPair() {
    std::array<int, 2> sockets{};
    const int rc = ::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data());
    assert(rc == 0);
    return sockets;
}

}  // namespace

int main() {
    auto sockets = makeSocketPair();
    mini::net::EventLoop loop;
    mini::net::Channel channel(&loop, sockets[0]);
    mini::net::detail::ConnectionTransport transport;

    const auto writeResult = transport.writeRaw("ping", channel);
    assert(writeResult.status == mini::net::detail::ConnectionTransport::Status::kOk);
    assert(writeResult.bytes == 4);

    char peerBuffer[8] = {};
    const ssize_t peerRead = ::read(sockets[1], peerBuffer, sizeof(peerBuffer));
    assert(peerRead == 4);
    assert(std::string_view(peerBuffer, 4) == "ping");

    constexpr std::string_view peerPayload = "pong";
    const ssize_t peerWrite = ::write(sockets[1], peerPayload.data(), peerPayload.size());
    assert(peerWrite == static_cast<ssize_t>(peerPayload.size()));

    mini::net::Buffer inputBuffer;
    const auto readResult = transport.readInto(inputBuffer, channel);
    assert(readResult.status == mini::net::detail::ConnectionTransport::Status::kOk);
    assert(readResult.bytes == peerPayload.size());
    assert(inputBuffer.retrieveAllAsString() == "pong");

    mini::net::Buffer outputBuffer;
    outputBuffer.append(std::string_view("tail"));
    const auto flushResult = transport.writeFromBuffer(outputBuffer, channel);
    assert(flushResult.status == mini::net::detail::ConnectionTransport::Status::kOk);
    assert(flushResult.bytes == 4);
    assert(outputBuffer.readableBytes() == 0);

    char flushedBuffer[8] = {};
    const ssize_t flushedRead = ::read(sockets[1], flushedBuffer, sizeof(flushedBuffer));
    assert(flushedRead == 4);
    assert(std::string_view(flushedBuffer, 4) == "tail");

    ::close(sockets[0]);
    ::close(sockets[1]);
    return 0;
}
