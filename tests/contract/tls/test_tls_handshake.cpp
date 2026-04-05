#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpClient.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"
#include "mini/net/TlsContext.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace mini::net;
using namespace std::chrono_literals;

const std::string kCertDir = SOURCE_DIR "/tests/certs/";
const std::string kServerCert = kCertDir + "server.crt";
const std::string kServerKey = kCertDir + "server.key";
const std::string kCaCert = kCertDir + "ca.crt";

namespace {

// ── Contract: TLS handshake completes on owner loop thread ──
void testTlsHandshakeCompletes() {
    EventLoop loop;
    const uint16_t port = 19876;
    InetAddress listenAddr(port);

    auto serverCtx = TlsContext::newServerContext(kServerCert, kServerKey);
    TcpServer server(&loop, listenAddr, "TlsTestServer");
    server.enableSsl(serverCtx);

    bool serverConnected = false;
    bool clientConnected = false;

    server.setConnectionCallback([&](const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            serverConnected = true;
            assert(conn->isTlsEstablished());
        }
    });
    server.setMessageCallback([](const TcpConnectionPtr&, Buffer*) {});
    server.start();

    auto clientCtx = TlsContext::newClientContext();
    clientCtx->setCaCertPath(kCaCert);
    clientCtx->setVerifyPeer(false);  // Self-signed cert, skip verify for test

    InetAddress serverAddr("127.0.0.1", port);
    TcpClient client(&loop, serverAddr, "TlsTestClient");
    client.enableSsl(clientCtx, "localhost");
    client.setConnectionCallback([&](const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            clientConnected = true;
            assert(conn->isTlsEstablished());
        }
    });
    client.setMessageCallback([](const TcpConnectionPtr&, Buffer*) {});

    // Quit after handshake or timeout
    loop.runAfter(500ms, [&] {
        loop.quit();
    });

    loop.runAfter(50ms, [&] {
        client.connect();
    });

    loop.loop();

    assert(serverConnected);
    assert(clientConnected);
    std::printf("  PASS: testTlsHandshakeCompletes\n");
}

// ── Contract: TLS read/write through SSL layer ──
void testTlsReadWrite() {
    EventLoop loop;
    const uint16_t port = 19877;
    InetAddress listenAddr(port);

    auto serverCtx = TlsContext::newServerContext(kServerCert, kServerKey);
    TcpServer server(&loop, listenAddr, "TlsEchoServer");
    server.enableSsl(serverCtx);

    server.setConnectionCallback([](const TcpConnectionPtr&) {});
    server.setMessageCallback([](const TcpConnectionPtr& conn, Buffer* buf) {
        // Echo back
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
    const std::string testMsg = "Hello TLS World!";

    client.setConnectionCallback([&](const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            conn->send(testMsg);
        }
    });
    client.setMessageCallback([&](const TcpConnectionPtr&, Buffer* buf) {
        received = buf->retrieveAllAsString();
        loop.quit();
    });

    loop.runAfter(2s, [&] {
        loop.quit();
    });

    loop.runAfter(50ms, [&] {
        client.connect();
    });

    loop.loop();

    assert(received == testMsg);
    std::printf("  PASS: testTlsReadWrite\n");
}

// ── Contract: TLS shutdown integrates with close path ──
void testTlsShutdown() {
    EventLoop loop;
    const uint16_t port = 19878;
    InetAddress listenAddr(port);

    auto serverCtx = TlsContext::newServerContext(kServerCert, kServerKey);
    TcpServer server(&loop, listenAddr, "TlsShutdownServer");
    server.enableSsl(serverCtx);

    bool serverSawDisconnect = false;
    bool clientSawDisconnect = false;

    server.setConnectionCallback([&](const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            // Server initiates shutdown after TLS established
            conn->shutdown();
        } else {
            serverSawDisconnect = true;
        }
    });
    server.setMessageCallback([](const TcpConnectionPtr&, Buffer*) {});
    server.start();

    auto clientCtx = TlsContext::newClientContext();
    clientCtx->setVerifyPeer(false);

    InetAddress serverAddr("127.0.0.1", port);
    TcpClient client(&loop, serverAddr, "TlsShutdownClient");
    client.enableSsl(clientCtx);

    client.setConnectionCallback([&](const TcpConnectionPtr& conn) {
        if (!conn->connected()) {
            clientSawDisconnect = true;
        }
    });
    client.setMessageCallback([](const TcpConnectionPtr&, Buffer*) {});

    loop.runAfter(1s, [&] {
        loop.quit();
    });

    loop.runAfter(50ms, [&] {
        client.connect();
    });

    loop.loop();

    assert(serverSawDisconnect);
    assert(clientSawDisconnect);
    std::printf("  PASS: testTlsShutdown\n");
}

// ── Contract: Non-TLS connections are unaffected ──
void testNonTlsUnaffected() {
    EventLoop loop;
    const uint16_t port = 19879;
    InetAddress listenAddr(port);

    // Server without TLS
    TcpServer server(&loop, listenAddr, "PlainServer");

    std::string received;
    const std::string testMsg = "No TLS here";

    server.setConnectionCallback([](const TcpConnectionPtr&) {});
    server.setMessageCallback([](const TcpConnectionPtr& conn, Buffer* buf) {
        std::string msg = buf->retrieveAllAsString();
        conn->send(msg);
    });
    server.start();

    // Client without TLS
    InetAddress serverAddr("127.0.0.1", port);
    TcpClient client(&loop, serverAddr, "PlainClient");

    client.setConnectionCallback([&](const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            assert(!conn->isTlsEstablished());
            conn->send(testMsg);
        }
    });
    client.setMessageCallback([&](const TcpConnectionPtr&, Buffer* buf) {
        received = buf->retrieveAllAsString();
        loop.quit();
    });

    loop.runAfter(2s, [&] {
        loop.quit();
    });

    loop.runAfter(50ms, [&] {
        client.connect();
    });

    loop.loop();

    assert(received == testMsg);
    std::printf("  PASS: testNonTlsUnaffected\n");
}

}  // namespace

int main() {
    std::printf("=== TLS Contract Tests ===\n");
    testTlsHandshakeCompletes();
    testTlsReadWrite();
    testTlsShutdown();
    testNonTlsUnaffected();
    std::printf("All TLS contract tests passed.\n");
    return 0;
}
