// Intent: tcp_server.intent.md; callback cardinality and owner-loop stop.
#include "mini/net/Buffer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/Socket.h"
#include "mini/net/SocketsOps.h"
#include "mini/net/TcpClient.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"

#include <atomic>
#include <cassert>
#include <chrono>

using namespace mini::net;
using namespace std::chrono_literals;

static void verify(int workers, bool force) {
    EventLoop loop;
    // Reserve an ephemeral port without assuming a machine-specific free port.
    auto port = [] {
        Socket socket(sockets::createNonblockingOrDie(AF_INET));
        socket.bindAddress(InetAddress(0, true));
        return InetAddress(sockets::getLocalAddr(socket.fd())).port();
    }();
    TcpServer server(&loop, InetAddress(port, true), "events", false);
    server.setThreadNum(workers);
    TcpClient client(&loop, InetAddress(port, true), "client");
    std::atomic<int> connected{0}, disconnected{0}, forced{0};
    bool timedOut = false;
    server.setConnectionEventCallback([&](const TcpConnectionPtr& conn, ConnectionEvent event) {
        assert(conn->getLoop()->isInLoopThread());
        if (event == ConnectionEvent::Connected) {
            ++connected;
            loop.queueInLoop([&] {
                if (force) { server.stop(); loop.quit(); }
                else { client.disconnect(); }
            });
        } else if (event == ConnectionEvent::Disconnected) {
            ++disconnected;
            if (!force) {
                loop.queueInLoop([&] { server.stop(); loop.quit(); });
            }
        } else if (event == ConnectionEvent::ForceClosed) {
            ++forced;
        }
    });
    server.start();
    client.connect();
    loop.runAfter(3s, [&] { timedOut = true; server.stop(); loop.quit(); });
    loop.loop();
    client.stop();
    assert(!timedOut);
    assert(connected == 1);
    assert(disconnected == 1);
    assert(forced == (force ? 1 : 0));
}

int main() {
    for (int workers : {0, 1}) {
        verify(workers, false);
        verify(workers, true);
    }
}
