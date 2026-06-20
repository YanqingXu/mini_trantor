#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/broadcast/BroadcastRouter.h"

#include <array>
#include <cassert>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

std::pair<int, int> makeSocketPair() {
    std::array<int, 2> sockets{};
    const int rc = ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data());
    assert(rc == 0);
    return {sockets[0], sockets[1]};
}

mini::net::TcpConnectionPtr makeConnection(mini::net::EventLoop* loop,
                                          int fd,
                                          const std::string& name) {
    return std::make_shared<mini::net::TcpConnection>(
        loop, name, fd,
        mini::net::InetAddress(),
        mini::net::InetAddress());
}

void closeSocketPairPeer(std::pair<int, int> sockets) {
    ::close(sockets.second);
}

void testRouteBySessionIds() {
    mini::net::EventLoop baseLoop;
    mini::net::EventLoop loopA;
    mini::net::EventLoop loopB;

    auto ioSocketsA = makeSocketPair();
    auto ioSocketsB = makeSocketPair();

    mini::net::broadcast::BroadcastRouter router(&baseLoop);
    auto connA = makeConnection(&loopA, ioSocketsA.first, "session-a");
    auto connB = makeConnection(&loopB, ioSocketsB.first, "session-b");

    router.registerConnection(connA);
    router.registerConnection(connB);

    const auto batches = router.route({"session-a", "missing", "session-b", "session-a"});
    assert(!batches.empty());
    assert(router.sessionCount() == 2);
    assert(router.loopBucketCount() == 2);
    assert(router.hasSession("session-a"));
    assert(!router.hasSession("missing"));

    std::size_t sessionHitCount = 0;
    bool sawMissing = false;
    for (const auto& batch : batches) {
        assert(batch.loop != nullptr);
        assert(!batch.connections.empty());

        for (const auto& connection : batch.connections) {
            assert(connection != nullptr);
            if (connection->name() == "session-a") {
                ++sessionHitCount;
            } else if (connection->name() == "session-b") {
                ++sessionHitCount;
            } else {
                sawMissing = true;
            }
        }
    }

    assert(sessionHitCount == 2);
    assert(!sawMissing);

    closeSocketPairPeer(ioSocketsA);
    closeSocketPairPeer(ioSocketsB);
}

void testRouteAllAndDeregister() {
    mini::net::EventLoop baseLoop;
    mini::net::EventLoop loopA;
    mini::net::EventLoop loopB;

    auto ioSocketsA = makeSocketPair();
    auto ioSocketsB = makeSocketPair();
    mini::net::broadcast::BroadcastRouter router(&baseLoop);

    auto connA = makeConnection(&loopA, ioSocketsA.first, "session-a");
    auto connB = makeConnection(&loopB, ioSocketsB.first, "session-b");

    router.registerConnection(connA);
    router.registerConnection(connB);
    assert(router.sessionCount() == 2);

    auto allBatches = router.routeAll();
    assert(allBatches.size() == 2);
    std::size_t totalBefore = 0;
    for (const auto& batch : allBatches) {
        totalBefore += batch.connections.size();
        assert(!batch.connections.empty());
    }
    assert(totalBefore == 2);

    router.deregisterConnection(connA);
    assert(!router.hasSession("session-a"));
    assert(router.sessionCount() == 1);
    assert(router.loopBucketCount() == 1);

    auto after = router.routeAll();
    assert(after.size() == 1);
    assert(after.front().connections.size() == 1);
    assert(after.front().connections.front()->name() == "session-b");

    router.deregisterConnection(connB);
    assert(router.sessionCount() == 0);
    assert(router.loopBucketCount() == 0);

    closeSocketPairPeer(ioSocketsA);
    closeSocketPairPeer(ioSocketsB);
}

}  // namespace

int main() {
    testRouteBySessionIds();
    testRouteAllAndDeregister();
    return 0;
}
