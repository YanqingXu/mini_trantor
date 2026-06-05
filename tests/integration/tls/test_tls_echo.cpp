#include "mini/coroutine/Task.h"
#include "mini/net/Buffer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpClient.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"
#include "mini/net/TlsContext.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace mini::net;
using namespace mini::coroutine;
using namespace std::chrono_literals;

const std::string kCertDir = SOURCE_DIR "/tests/certs/";
const std::string kServerCert = kCertDir + "server.crt";
const std::string kServerKey = kCertDir + "server.key";
const std::string kCaCert = kCertDir + "ca.crt";

namespace {

uint16_t allocateTestPort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    assert(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    const int bound = ::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    assert(bound == 0);
    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    const uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

// ── Integration: TLS echo server + client round-trip ──
void testTlsEchoRoundTrip() {
    const uint16_t port = allocateTestPort();
    EventLoop loop;
    InetAddress listenAddr(port, true);

    auto serverCtx = TlsContext::newServerContext(kServerCert, kServerKey);
    TcpServer server(&loop, listenAddr, "TlsEchoServer");
    server.enableSsl(serverCtx);

    // Echo server: send back what we receive
    server.setConnectionCallback([](const TcpConnectionPtr&) {});
    server.setMessageCallback([](const TcpConnectionPtr& conn, Buffer* buf) {
        std::string msg = buf->retrieveAllAsString();
        conn->send(msg);
    });
    server.start();

    auto clientCtx = TlsContext::newClientContext();
    clientCtx->setVerifyPeer(false);

    InetAddress serverAddr("127.0.0.1", port);
    TcpClient client(&loop, serverAddr, "TlsEchoClient");
    client.enableSsl(clientCtx, "localhost");

    std::string received;
    const std::string testMsg = "TLS integration test message 12345!";

    client.setConnectionCallback([&](const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            conn->send(testMsg);
        }
    });
    client.setMessageCallback([&](const TcpConnectionPtr&, Buffer* buf) {
        received += buf->retrieveAllAsString();
        if (received.size() >= testMsg.size()) {
            loop.quit();
        }
    });

    loop.runAfter(3s, [&] { loop.quit(); });
    loop.runAfter(50ms, [&] { client.connect(); });

    loop.loop();

    assert(received == testMsg);
    std::printf("  PASS: testTlsEchoRoundTrip\n");
}

// ── Integration: Coroutine TLS echo path ──
Task<void> tlsEchoSession(TcpConnectionPtr conn) {
    auto result = co_await conn->asyncReadSome();
    if (result && !result->empty()) {
        co_await conn->asyncWrite(std::move(*result));
    }
    co_await conn->waitClosed();
}

void testCoroutineTlsEcho() {
    const uint16_t port = allocateTestPort();
    EventLoop loop;
    InetAddress listenAddr(port, true);

    auto serverCtx = TlsContext::newServerContext(kServerCert, kServerKey);
    TcpServer server(&loop, listenAddr, "CoroTlsEchoServer");
    server.enableSsl(serverCtx);

    server.setConnectionCallback([](const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            tlsEchoSession(conn).detach();
        }
    });
    server.setMessageCallback([](const TcpConnectionPtr&, Buffer*) {});
    server.start();

    auto clientCtx = TlsContext::newClientContext();
    clientCtx->setVerifyPeer(false);

    InetAddress serverAddr("127.0.0.1", port);
    TcpClient client(&loop, serverAddr, "CoroTlsEchoClient");
    client.enableSsl(clientCtx, "localhost");

    std::string received;
    const std::string testMsg = "Coroutine TLS echo test!";

    client.setConnectionCallback([&](const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            conn->send(testMsg);
        }
    });
    client.setMessageCallback([&](const TcpConnectionPtr& conn, Buffer* buf) {
        received += buf->retrieveAllAsString();
        if (received.size() >= testMsg.size()) {
            conn->shutdown();
            loop.runAfter(100ms, [&] { loop.quit(); });
        }
    });

    loop.runAfter(3s, [&] { loop.quit(); });
    loop.runAfter(50ms, [&] { client.connect(); });

    loop.loop();

    assert(received == testMsg);
    std::printf("  PASS: testCoroutineTlsEcho\n");
}

// ── Integration: certificate verification failure ──
void testCertVerificationFailure() {
    const uint16_t port = allocateTestPort();
    EventLoop loop;
    InetAddress listenAddr(port, true);

    auto serverCtx = TlsContext::newServerContext(kServerCert, kServerKey);
    TcpServer server(&loop, listenAddr, "TlsCertFailServer");
    server.enableSsl(serverCtx);

    server.setConnectionCallback([](const TcpConnectionPtr&) {});
    server.setMessageCallback([](const TcpConnectionPtr&, Buffer*) {});
    server.start();

    // Client with strict verification — self-signed cert should fail
    auto clientCtx = TlsContext::newClientContext();
    clientCtx->setVerifyPeer(true);
    // Don't load CA cert, so server's self-signed cert won't be trusted

    InetAddress serverAddr("127.0.0.1", port);
    TcpClient client(&loop, serverAddr, "TlsCertFailClient");
    client.enableSsl(clientCtx, "localhost");

    bool clientGotDisconnect = false;
    bool clientGotConnect = false;

    client.setConnectionCallback([&](const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            clientGotConnect = true;
        } else {
            clientGotDisconnect = true;
        }
    });
    client.setMessageCallback([](const TcpConnectionPtr&, Buffer*) {});

    loop.runAfter(1s, [&] { loop.quit(); });
    loop.runAfter(50ms, [&] { client.connect(); });

    loop.loop();

    // With strict verification and no CA loaded, the handshake should fail.
    // The client should NOT have gotten a successful TLS connection.
    // It should have disconnected.
    assert(!clientGotConnect || clientGotDisconnect);
    std::printf("  PASS: testCertVerificationFailure\n");
}

}  // namespace

int main() {
    std::printf("=== TLS Integration Tests ===\n");
    testTlsEchoRoundTrip();
    testCoroutineTlsEcho();
    testCertVerificationFailure();
    std::printf("All TLS integration tests passed.\n");
    return 0;
}
