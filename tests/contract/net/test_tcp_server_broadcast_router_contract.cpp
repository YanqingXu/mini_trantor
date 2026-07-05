#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/broadcast/BroadcastRouter.h"

#include <array>
#include <cassert>
#include <future>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace std::chrono_literals;

std::pair<int, int> makeSocketPair() {
    std::array<int, 2> sockets{};
    const int rc = ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data());
    assert(rc == 0);
    return {sockets[0], sockets[1]};
}

void closeSocketPairPeer(std::pair<int, int> sockets) {
    ::close(sockets.second);
}

std::string makeConnectionName(std::size_t index) {
    return "session-" + std::to_string(index);
}

mini::net::TcpConnectionPtr makeConnectionAsync(mini::net::EventLoop* loop,
                                              int fd,
                                              const std::string& name) {
    auto ready = std::make_shared<std::promise<mini::net::TcpConnectionPtr>>();
    auto readyFuture = ready->get_future();
    loop->queueInLoop([loop, fd, name, ready] {
        auto connection = std::make_shared<mini::net::TcpConnection>(
            loop,
            name,
            fd,
            mini::net::InetAddress(),
            mini::net::InetAddress());
        connection->connectEstablished();
        ready->set_value(connection);
    });
    return readyFuture.get();
}

std::vector<mini::net::broadcast::BroadcastRouter::LoopBatch> routeOnBaseLoop(
    mini::net::EventLoop* baseLoop,
    mini::net::broadcast::BroadcastRouter& router,
    std::vector<std::string> sessionIds) {
    auto ready = std::make_shared<std::promise<std::vector<mini::net::broadcast::BroadcastRouter::LoopBatch>>>();
    auto readyFuture = ready->get_future();
    baseLoop->queueInLoop([&router, sessionIds = std::move(sessionIds), ready] {
        ready->set_value(router.route(sessionIds));
    });
    return readyFuture.get();
}

std::vector<mini::net::broadcast::BroadcastRouter::LoopBatch> routeAllOnBaseLoop(
    mini::net::EventLoop* baseLoop,
    mini::net::broadcast::BroadcastRouter& router) {
    auto ready = std::make_shared<std::promise<std::vector<mini::net::broadcast::BroadcastRouter::LoopBatch>>>();
    auto readyFuture = ready->get_future();
    baseLoop->queueInLoop([&router, ready] {
        ready->set_value(router.routeAll());
    });
    return readyFuture.get();
}

std::vector<mini::net::broadcast::BroadcastRouter::LoopBatch> routeGroupOnBaseLoop(
    mini::net::EventLoop* baseLoop,
    mini::net::broadcast::BroadcastRouter& router,
    std::string_view groupId) {
    auto ready = std::make_shared<std::promise<std::vector<mini::net::broadcast::BroadcastRouter::LoopBatch>>>();
    auto readyFuture = ready->get_future();
    const auto target = std::string(groupId);
    baseLoop->queueInLoop([&router, target = std::move(target), ready] {
        ready->set_value(router.routeGroup(target));
    });
    return readyFuture.get();
}

std::vector<mini::net::broadcast::BroadcastRouter::LoopBatch> routeAoiOnBaseLoop(
    mini::net::EventLoop* baseLoop,
    mini::net::broadcast::BroadcastRouter& router,
    std::string_view aoiId) {
    auto ready = std::make_shared<std::promise<std::vector<mini::net::broadcast::BroadcastRouter::LoopBatch>>>();
    auto readyFuture = ready->get_future();
    const auto target = std::string(aoiId);
    baseLoop->queueInLoop([&router, target = std::move(target), ready] {
        ready->set_value(router.routeAoi(target));
    });
    return readyFuture.get();
}

std::size_t sessionCountOnBaseLoop(mini::net::EventLoop* baseLoop,
                                  mini::net::broadcast::BroadcastRouter& router) {
    auto ready = std::make_shared<std::promise<std::size_t>>();
    auto readyFuture = ready->get_future();
    baseLoop->queueInLoop([&router, ready] {
        ready->set_value(router.sessionCount());
    });
    return readyFuture.get();
}

bool hasSessionOnBaseLoop(mini::net::EventLoop* baseLoop,
                         mini::net::broadcast::BroadcastRouter& router,
                         std::string_view sessionId) {
    auto ready = std::make_shared<std::promise<bool>>();
    auto readyFuture = ready->get_future();
    const auto target = std::string(sessionId);
    baseLoop->queueInLoop([&router, target = std::move(target), ready] {
        ready->set_value(router.hasSession(target));
    });
    return readyFuture.get();
}

bool hasGroupMemberOnBaseLoop(mini::net::EventLoop* baseLoop,
                             mini::net::broadcast::BroadcastRouter& router,
                             std::string_view groupId,
                             std::string_view sessionId) {
    auto ready = std::make_shared<std::promise<bool>>();
    auto readyFuture = ready->get_future();
    const auto group = std::string(groupId);
    const auto session = std::string(sessionId);
    baseLoop->queueInLoop([&router, group, session, ready] {
        ready->set_value(router.hasGroupMember(group, session));
    });
    return readyFuture.get();
}

bool hasAoiMemberOnBaseLoop(mini::net::EventLoop* baseLoop,
                           mini::net::broadcast::BroadcastRouter& router,
                           std::string_view aoiId,
                           std::string_view sessionId) {
    auto ready = std::make_shared<std::promise<bool>>();
    auto readyFuture = ready->get_future();
    const auto aoi = std::string(aoiId);
    const auto session = std::string(sessionId);
    baseLoop->queueInLoop([&router, aoi, session, ready] {
        ready->set_value(router.hasAoiMember(aoi, session));
    });
    return readyFuture.get();
}

std::size_t loopBucketCountOnBaseLoop(mini::net::EventLoop* baseLoop,
                                     mini::net::broadcast::BroadcastRouter& router) {
    auto ready = std::make_shared<std::promise<std::size_t>>();
    auto readyFuture = ready->get_future();
    baseLoop->queueInLoop([&router, ready] {
        ready->set_value(router.loopBucketCount());
    });
    return readyFuture.get();
}

void recycleConnection(mini::net::EventLoop* loop, mini::net::TcpConnectionPtr connection) {
    auto ready = std::make_shared<std::promise<void>>();
    auto readyFuture = ready->get_future();
    if (!connection) {
        ready->set_value();
        readyFuture.get();
        return;
    }
    loop->queueInLoop([connection = std::move(connection), ready] mutable {
        connection->connectDestroyed();
        connection.reset();
        ready->set_value();
    });
    readyFuture.get();
}

std::size_t totalRoutedConnections(
    const std::vector<mini::net::broadcast::BroadcastRouter::LoopBatch>& batches) {
    std::size_t total = 0;
    for (const auto& batch : batches) {
        total += batch.connections.size();
    }
    return total;
}

bool batchContainsConnection(
    const std::vector<mini::net::broadcast::BroadcastRouter::LoopBatch>& batches,
    const mini::net::TcpConnectionPtr& expected) {
    for (const auto& batch : batches) {
        for (const auto& connection : batch.connections) {
            if (connection == expected) {
                return true;
            }
        }
    }
    return false;
}

void testRouteBySessionIds() {
    mini::net::EventLoopThread baseLoopThread;
    auto* baseLoop = baseLoopThread.startLoop();
    mini::net::EventLoopThread loopAThread;
    auto* loopA = loopAThread.startLoop();
    mini::net::EventLoopThread loopBThread;
    auto* loopB = loopBThread.startLoop();

    auto ioSocketsA = makeSocketPair();
    auto ioSocketsB = makeSocketPair();

    mini::net::broadcast::BroadcastRouter router(baseLoop);
    auto connA = makeConnectionAsync(loopA, ioSocketsA.first, makeConnectionName(1));
    auto connB = makeConnectionAsync(loopB, ioSocketsB.first, makeConnectionName(2));

    router.registerConnection(connA);
    router.registerConnection(connB);

    const auto batches = routeOnBaseLoop(
        baseLoop,
        router,
        {makeConnectionName(1), "missing", makeConnectionName(2), makeConnectionName(1)});
    assert(!batches.empty());
    assert(sessionCountOnBaseLoop(baseLoop, router) == 2);
    assert(loopBucketCountOnBaseLoop(baseLoop, router) == 2);
    assert(hasSessionOnBaseLoop(baseLoop, router, makeConnectionName(1)));
    assert(!hasSessionOnBaseLoop(baseLoop, router, "missing"));

    std::size_t sessionHitCount = 0;
    bool sawMissing = false;
    for (const auto& batch : batches) {
        assert(batch.loop != nullptr);
        assert(!batch.connections.empty());

        for (const auto& connection : batch.connections) {
            assert(connection != nullptr);
            if (connection->name() == makeConnectionName(1) || connection->name() == makeConnectionName(2)) {
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

    recycleConnection(loopA, std::move(connA));
    recycleConnection(loopB, std::move(connB));
}

void testRouteAllAndDeregister() {
    mini::net::EventLoopThread baseLoopThread;
    auto* baseLoop = baseLoopThread.startLoop();
    mini::net::EventLoopThread loopAThread;
    auto* loopA = loopAThread.startLoop();
    mini::net::EventLoopThread loopBThread;
    auto* loopB = loopBThread.startLoop();

    auto ioSocketsA = makeSocketPair();
    auto ioSocketsB = makeSocketPair();
    mini::net::broadcast::BroadcastRouter router(baseLoop);

    auto connA = makeConnectionAsync(loopA, ioSocketsA.first, makeConnectionName(1));
    auto connB = makeConnectionAsync(loopB, ioSocketsB.first, makeConnectionName(2));

    router.registerConnection(connA);
    router.registerConnection(connB);
    assert(sessionCountOnBaseLoop(baseLoop, router) == 2);

    const auto allBatches = routeAllOnBaseLoop(baseLoop, router);
    assert(allBatches.size() == 2);
    std::size_t totalBefore = 0;
    for (const auto& batch : allBatches) {
        totalBefore += batch.connections.size();
        assert(!batch.connections.empty());
    }
    assert(totalBefore == 2);

    router.deregisterConnection(connA);
    assert(!hasSessionOnBaseLoop(baseLoop, router, makeConnectionName(1)));
    assert(sessionCountOnBaseLoop(baseLoop, router) == 1);
    assert(loopBucketCountOnBaseLoop(baseLoop, router) == 1);

    const auto after = routeAllOnBaseLoop(baseLoop, router);
    assert(after.size() == 1);
    assert(after.front().connections.size() == 1);
    assert(after.front().connections.front()->name() == makeConnectionName(2));

    router.deregisterConnection(connB);
    assert(sessionCountOnBaseLoop(baseLoop, router) == 0);
    assert(loopBucketCountOnBaseLoop(baseLoop, router) == 0);

    closeSocketPairPeer(ioSocketsA);
    closeSocketPairPeer(ioSocketsB);

    recycleConnection(loopA, std::move(connA));
    recycleConnection(loopB, std::move(connB));
}

void testGroupAndAoiMembershipLeaveAndDeregister() {
    mini::net::EventLoopThread baseLoopThread;
    auto* baseLoop = baseLoopThread.startLoop();
    mini::net::EventLoopThread loopAThread;
    auto* loopA = loopAThread.startLoop();
    mini::net::EventLoopThread loopBThread;
    auto* loopB = loopBThread.startLoop();

    auto ioSocketsA = makeSocketPair();
    auto ioSocketsB = makeSocketPair();
    mini::net::broadcast::BroadcastRouter router(baseLoop);

    auto connA = makeConnectionAsync(loopA, ioSocketsA.first, "player-a-conn");
    auto connB = makeConnectionAsync(loopB, ioSocketsB.first, "player-b-conn");
    const std::string sessionA = "player-a";
    const std::string sessionB = "player-b";
    const std::string group = "room-alpha";
    const std::string aoi = "aoi-alpha";

    router.registerSession(sessionA, connA);
    router.registerSession(sessionB, connB);
    router.joinGroup(sessionA, group);
    router.joinGroup(sessionB, group);
    router.joinAoi(sessionA, aoi);
    router.joinAoi(sessionB, aoi);

    assert(hasGroupMemberOnBaseLoop(baseLoop, router, group, sessionA));
    assert(hasGroupMemberOnBaseLoop(baseLoop, router, group, sessionB));
    assert(hasAoiMemberOnBaseLoop(baseLoop, router, aoi, sessionA));
    assert(hasAoiMemberOnBaseLoop(baseLoop, router, aoi, sessionB));
    assert(totalRoutedConnections(routeGroupOnBaseLoop(baseLoop, router, group)) == 2);
    assert(totalRoutedConnections(routeAoiOnBaseLoop(baseLoop, router, aoi)) == 2);

    router.leaveGroup(sessionA, group);
    const auto groupAfterLeave = routeGroupOnBaseLoop(baseLoop, router, group);
    assert(!hasGroupMemberOnBaseLoop(baseLoop, router, group, sessionA));
    assert(hasGroupMemberOnBaseLoop(baseLoop, router, group, sessionB));
    assert(totalRoutedConnections(groupAfterLeave) == 1);
    assert(batchContainsConnection(groupAfterLeave, connB));

    router.leaveAoi(sessionB, aoi);
    const auto aoiAfterLeave = routeAoiOnBaseLoop(baseLoop, router, aoi);
    assert(hasAoiMemberOnBaseLoop(baseLoop, router, aoi, sessionA));
    assert(!hasAoiMemberOnBaseLoop(baseLoop, router, aoi, sessionB));
    assert(totalRoutedConnections(aoiAfterLeave) == 1);
    assert(batchContainsConnection(aoiAfterLeave, connA));

    router.deregisterConnection(connA);
    assert(!hasSessionOnBaseLoop(baseLoop, router, sessionA));
    assert(!hasAoiMemberOnBaseLoop(baseLoop, router, aoi, sessionA));
    assert(routeAoiOnBaseLoop(baseLoop, router, aoi).empty());
    assert(sessionCountOnBaseLoop(baseLoop, router) == 1);

    router.deregisterConnection(connB);
    assert(!hasSessionOnBaseLoop(baseLoop, router, sessionB));
    assert(!hasGroupMemberOnBaseLoop(baseLoop, router, group, sessionB));
    assert(routeGroupOnBaseLoop(baseLoop, router, group).empty());
    assert(sessionCountOnBaseLoop(baseLoop, router) == 0);

    closeSocketPairPeer(ioSocketsA);
    closeSocketPairPeer(ioSocketsB);

    recycleConnection(loopA, std::move(connA));
    recycleConnection(loopB, std::move(connB));
}

void testGuardedDeregisterKeepsReboundSession() {
    mini::net::EventLoopThread baseLoopThread;
    auto* baseLoop = baseLoopThread.startLoop();
    mini::net::EventLoopThread loopAThread;
    auto* loopA = loopAThread.startLoop();
    mini::net::EventLoopThread loopBThread;
    auto* loopB = loopBThread.startLoop();

    auto ioSocketsA = makeSocketPair();
    auto ioSocketsB = makeSocketPair();
    mini::net::broadcast::BroadcastRouter router(baseLoop);

    auto connA = makeConnectionAsync(loopA, ioSocketsA.first, "old-transport");
    auto connB = makeConnectionAsync(loopB, ioSocketsB.first, "new-transport");
    const std::string sessionId = "sticky-player";
    const std::string groupId = "room-sticky";

    router.registerSession(sessionId, connA);
    router.joinGroup(sessionId, groupId);

    auto initial = routeGroupOnBaseLoop(baseLoop, router, groupId);
    assert(initial.size() == 1);
    assert(initial.front().connections.size() == 1);
    assert(initial.front().connections.front() == connA);

    router.registerSession(sessionId, connB);
    router.deregisterSession(sessionId, connA);

    auto afterStaleClose = routeGroupOnBaseLoop(baseLoop, router, groupId);
    assert(afterStaleClose.size() == 1);
    assert(afterStaleClose.front().connections.size() == 1);
    assert(afterStaleClose.front().connections.front() == connB);
    assert(hasSessionOnBaseLoop(baseLoop, router, sessionId));
    assert(sessionCountOnBaseLoop(baseLoop, router) == 1);

    router.deregisterSession(sessionId, connB);
    assert(!hasSessionOnBaseLoop(baseLoop, router, sessionId));
    assert(routeGroupOnBaseLoop(baseLoop, router, groupId).empty());

    closeSocketPairPeer(ioSocketsA);
    closeSocketPairPeer(ioSocketsB);

    recycleConnection(loopA, std::move(connA));
    recycleConnection(loopB, std::move(connB));
}

}  // namespace

int main() {
    testRouteBySessionIds();
    testRouteAllAndDeregister();
    testGroupAndAoiMembershipLeaveAndDeregister();
    testGuardedDeregisterKeepsReboundSession();
    return 0;
}
