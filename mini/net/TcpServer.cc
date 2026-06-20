#include "mini/net/TcpServer.h"

#include "mini/base/Logger.h"
#include "mini/net/EventLoop.h"
#include "mini/net/Buffer.h"
#include "mini/net/SocketsOps.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TlsContext.h"

#include <cstdint>
#include <string_view>
#include <stdexcept>
#include <utility>

namespace mini::net {

namespace {

struct IdleTimeoutState {
    EventLoop* loop;
    std::weak_ptr<TcpConnection> connection;
    TcpServer::Duration timeout;
    TimerId timerId;
    std::uint64_t generation{0};
    ConnectionEventCallback connectionEventCallback;
};

void cancelIdleTimer(const std::shared_ptr<IdleTimeoutState>& idleState) {
    if (!idleState || idleState->timeout <= TcpServer::Duration::zero()) {
        return;
    }

    ++idleState->generation;
    if (idleState->timerId.valid()) {
        idleState->loop->cancel(idleState->timerId);
        idleState->timerId = {};
    }
}

void refreshIdleTimer(const std::shared_ptr<IdleTimeoutState>& idleState) {
    if (!idleState || idleState->timeout <= TcpServer::Duration::zero()) {
        return;
    }

    cancelIdleTimer(idleState);
    const auto generation = idleState->generation;
    idleState->timerId = idleState->loop->runAfter(idleState->timeout, [idleState, generation] {
        if (idleState->generation != generation) {
            return;
        }
        idleState->timerId = {};

        auto connection = idleState->connection.lock();
        if (!connection || !connection->connected()) {
            return;
        }

        // Notify connection event hook before force-close.
        if (idleState->connectionEventCallback) {
            idleState->connectionEventCallback(connection, ConnectionEvent::IdleTimeout);
        }
        connection->forceClose();
    });
}

}  // namespace

TcpServer::TcpServer(EventLoop* loop, const InetAddress& listenAddr, std::string name, bool reusePort)
    : TcpServer(loop, listenAddr, std::move(name),
                TcpServerOptions{.reusePort = reusePort}) {
}

TcpServer::TcpServer(EventLoop* loop, const InetAddress& listenAddr, std::string name, TcpServerOptions options)
    : loop_(loop),
      name_(std::move(name)),
      acceptor_(std::make_unique<Acceptor>(loop, listenAddr, options.reusePort)),
      threadPool_(std::make_shared<EventLoopThreadPool>(loop, name_)),
      broadcastRouter_(std::make_shared<broadcast::BroadcastRouter>(loop)),
      broadcastDispatcher_(std::make_shared<broadcast::BroadcastDispatcher>(loop)),
      payloadPool_(std::make_shared<buffer::PayloadPool>(loop)),
      started_(false),
      stopped_(false),
      draining_(false),
      broadcastMetricsEnabled_(options.metrics.enableBroadcastMetrics),
      eventLoopQueueMetricsEnabled_(options.metrics.enableEventLoopQueueMetrics),
      nextConnId_(1),
      highWaterMark_(0),
      backpressureHighWaterMark_(options.backpressureHighWaterMark),
      backpressureLowWaterMark_(options.backpressureLowWaterMark),
      idleTimeout_(options.idleTimeout),
      lifetimeToken_(std::make_shared<int>(0)) {
    // Apply options to thread pool.
    threadPool_->setThreadNum(options.numThreads);

    // Validate backpressure thresholds if configured.
    if (backpressureHighWaterMark_ > 0 || backpressureLowWaterMark_ > 0) {
        TcpServerOptions::validateBackpressure(backpressureHighWaterMark_, backpressureLowWaterMark_);
    }

    acceptor_->setNewConnectionCallback(
        [this](int sockfd, const InetAddress& peerAddr) { newConnection(sockfd, peerAddr); });
}

TcpServer::~TcpServer() {
    loop_->assertInLoopThread();
    lifetimeToken_.reset();
    acceptor_->setNewConnectionCallback({});

    for (auto& [name, connection] : connections_) {
        if (broadcastRouter_) {
            broadcastRouter_->deregisterConnection(connection);
        }
        auto conn = connection;
        conn->getLoop()->runInLoop([conn] {
            conn->setCloseCallback({});
            conn->connectDestroyed();
        });
    }
}

void TcpServer::setThreadNum(int numThreads) {
    threadPool_->setThreadNum(numThreads);
}

void TcpServer::setIdleTimeout(Duration timeout) {
    idleTimeout_ = timeout;
}

void TcpServer::setBackpressurePolicy(std::size_t highWaterMark, std::size_t lowWaterMark) {
    if (highWaterMark == 0) {
        if (lowWaterMark != 0) {
            throw std::invalid_argument("backpressure low water mark requires a non-zero high water mark");
        }
    } else if (lowWaterMark >= highWaterMark) {
        throw std::invalid_argument("backpressure low water mark must be smaller than high water mark");
    }

    backpressureHighWaterMark_ = highWaterMark;
    backpressureLowWaterMark_ = lowWaterMark;
}

void TcpServer::setThreadInitCallback(ThreadInitCallback cb) {
    threadInitCallback_ = std::move(cb);
}

void TcpServer::enableSsl(std::shared_ptr<TlsContext> tlsContext) {
    tlsContext_ = std::move(tlsContext);
}

void TcpServer::setConnectionCallback(ConnectionCallback cb) {
    connectionCallback_ = std::move(cb);
}

void TcpServer::setMessageCallback(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

void TcpServer::setLogicMessageCallback(LogicMessageCallback cb) {
    logicMessageCallback_ = std::move(cb);
}

void TcpServer::setHighWaterMarkCallback(HighWaterMarkCallback cb, std::size_t highWaterMark) {
    highWaterMarkCallback_ = std::move(cb);
    highWaterMark_ = highWaterMark;
}

void TcpServer::setWriteCompleteCallback(WriteCompleteCallback cb) {
    writeCompleteCallback_ = std::move(cb);
}

std::size_t TcpServer::connectionCount() const {
    loop_->assertInLoopThread();
    return connections_.size();
}

void TcpServer::bindBroadcastSession(const TcpConnectionPtr& connection, std::string sessionId) {
    if (!broadcastRouter_ || !connection || sessionId.empty()) {
        return;
    }
    if (!loop_->isInLoopThread()) {
        loop_->queueInLoop([this, connection, sessionId = std::move(sessionId)]() mutable {
            bindBroadcastSession(connection, std::move(sessionId));
        });
        return;
    }
    broadcastRouter_->registerSession(std::move(sessionId), connection);
}

void TcpServer::unbindBroadcastSession(std::string sessionId) {
    if (!broadcastRouter_ || sessionId.empty()) {
        return;
    }
    if (!loop_->isInLoopThread()) {
        loop_->queueInLoop([this, sessionId = std::move(sessionId)]() mutable {
            unbindBroadcastSession(std::move(sessionId));
        });
        return;
    }
    broadcastRouter_->deregisterSession(sessionId);
}

void TcpServer::joinBroadcastGroup(std::string sessionId, std::string groupId) {
    if (!broadcastRouter_ || sessionId.empty() || groupId.empty()) {
        return;
    }
    if (!loop_->isInLoopThread()) {
        loop_->queueInLoop([this, sessionId = std::move(sessionId), groupId = std::move(groupId)]() mutable {
            joinBroadcastGroup(std::move(sessionId), std::move(groupId));
        });
        return;
    }
    broadcastRouter_->joinGroup(std::move(sessionId), std::move(groupId));
}

void TcpServer::leaveBroadcastGroup(std::string sessionId, std::string groupId) {
    if (!broadcastRouter_ || sessionId.empty() || groupId.empty()) {
        return;
    }
    if (!loop_->isInLoopThread()) {
        loop_->queueInLoop([this, sessionId = std::move(sessionId), groupId = std::move(groupId)]() mutable {
            leaveBroadcastGroup(std::move(sessionId), std::move(groupId));
        });
        return;
    }
    broadcastRouter_->leaveGroup(sessionId, groupId);
}

void TcpServer::joinBroadcastAoi(std::string sessionId, std::string aoiId) {
    if (!broadcastRouter_ || sessionId.empty() || aoiId.empty()) {
        return;
    }
    if (!loop_->isInLoopThread()) {
        loop_->queueInLoop([this, sessionId = std::move(sessionId), aoiId = std::move(aoiId)]() mutable {
            joinBroadcastAoi(std::move(sessionId), std::move(aoiId));
        });
        return;
    }
    broadcastRouter_->joinAoi(std::move(sessionId), std::move(aoiId));
}

void TcpServer::leaveBroadcastAoi(std::string sessionId, std::string aoiId) {
    if (!broadcastRouter_ || sessionId.empty() || aoiId.empty()) {
        return;
    }
    if (!loop_->isInLoopThread()) {
        loop_->queueInLoop([this, sessionId = std::move(sessionId), aoiId = std::move(aoiId)]() mutable {
            leaveBroadcastAoi(std::move(sessionId), std::move(aoiId));
        });
        return;
    }
    broadcastRouter_->leaveAoi(sessionId, aoiId);
}

void TcpServer::broadcastTo(const std::vector<std::string>& sessionIds, const std::string& data) {
    if (!broadcastRouter_) {
        return;
    }
    if (!broadcastDispatcher_) {
        return;
    }
    if (!payloadPool_) {
        return;
    }

    const auto requestedAt = mini::base::now();
    if (!loop_->isInLoopThread()) {
        auto payload = payloadPool_->acquire(data);
        loop_->queueInLoop([this, sessionIds, payload = std::move(payload), requestedAt]() mutable {
            broadcastToInLoopWithMetrics(sessionIds, std::move(payload), requestedAt);
        });
        return;
    }

    auto payload = payloadPool_->acquire(data);
    broadcastToInLoopWithMetrics(sessionIds, std::move(payload), requestedAt);
}

void TcpServer::broadcastGroup(std::string groupId, const std::string& data) {
    if (!broadcastRouter_ || !broadcastDispatcher_ || !payloadPool_ || groupId.empty()) {
        return;
    }
    const auto requestedAt = mini::base::now();
    auto payload = payloadPool_->acquire(data);
    if (!loop_->isInLoopThread()) {
        loop_->queueInLoop([this, groupId = std::move(groupId), payload = std::move(payload), requestedAt]() mutable {
            auto batches = broadcastRouter_->routeGroup(groupId);
            broadcastBucketInLoopWithMetrics(std::move(batches), std::move(payload), requestedAt, true, 0);
        });
        return;
    }

    auto batches = broadcastRouter_->routeGroup(groupId);
    broadcastBucketInLoopWithMetrics(std::move(batches), std::move(payload), requestedAt, true, 0);
}

void TcpServer::broadcastAoi(std::string aoiId, const std::string& data) {
    if (!broadcastRouter_ || !broadcastDispatcher_ || !payloadPool_ || aoiId.empty()) {
        return;
    }
    const auto requestedAt = mini::base::now();
    auto payload = payloadPool_->acquire(data);
    if (!loop_->isInLoopThread()) {
        loop_->queueInLoop([this, aoiId = std::move(aoiId), payload = std::move(payload), requestedAt]() mutable {
            auto batches = broadcastRouter_->routeAoi(aoiId);
            broadcastBucketInLoopWithMetrics(std::move(batches), std::move(payload), requestedAt, true, 0);
        });
        return;
    }

    auto batches = broadcastRouter_->routeAoi(aoiId);
    broadcastBucketInLoopWithMetrics(std::move(batches), std::move(payload), requestedAt, true, 0);
}

void TcpServer::broadcast(const std::string& data) {
    if (!broadcastRouter_) {
        return;
    }
    if (!broadcastDispatcher_) {
        return;
    }
    if (!payloadPool_) {
        return;
    }
    const auto requestedAt = mini::base::now();
    if (!loop_->isInLoopThread()) {
        auto payload = payloadPool_->acquire(data);
        loop_->queueInLoop([this, payload = std::move(payload), requestedAt]() mutable {
            broadcastInLoopWithMetrics(std::move(payload), requestedAt);
        });
        return;
    }

    auto payload = payloadPool_->acquire(data);
    broadcastInLoopWithMetrics(std::move(payload), requestedAt);
}

void TcpServer::broadcastToInLoop(std::vector<std::string> sessionIds, buffer::PayloadPtr payload) {
    broadcastToInLoopWithMetrics(std::move(sessionIds), std::move(payload), mini::base::now());
}

void TcpServer::broadcastInLoop(buffer::PayloadPtr payload) {
    broadcastInLoopWithMetrics(std::move(payload), mini::base::now());
}

void TcpServer::broadcastToInLoopWithMetrics(
    std::vector<std::string> sessionIds,
    buffer::PayloadPtr payload,
    mini::base::Timestamp requestedAt) {
    if (!broadcastRouter_ || !broadcastDispatcher_ || !payload) {
        return;
    }
    if (sessionIds.empty()) {
        return;
    }
    const auto routeStartedAt = mini::base::now();
    auto batches = broadcastRouter_->route(sessionIds);
    const auto routedAt = mini::base::now();

    broadcast::BroadcastDispatcher::DispatchMetricContext metrics;
    metrics.requestedAt = requestedAt;
    metrics.routedAt = routedAt;
    metrics.targeted = true;
    metrics.requestedSessions = sessionIds.size();
    metrics.loopBatches = batches.size();
    metrics.payloadBytes = payload->size();
    metrics.routeLatency = routedAt - routeStartedAt;
    for (const auto& batch : batches) {
        metrics.fanoutConnections += batch.connections.size() + batch.endpoints.size();
    }

    broadcastDispatcher_->dispatch(std::move(batches), std::move(payload), metrics);
}

void TcpServer::broadcastBucketInLoopWithMetrics(
    std::vector<broadcast::BroadcastRouter::LoopBatch> batches,
    buffer::PayloadPtr payload,
    mini::base::Timestamp requestedAt,
    bool targeted,
    std::size_t requestedSessions) {
    if (!broadcastDispatcher_ || !payload || batches.empty()) {
        return;
    }
    broadcast::BroadcastDispatcher::DispatchMetricContext metrics;
    metrics.requestedAt = requestedAt;
    metrics.routedAt = mini::base::now();
    metrics.targeted = targeted;
    metrics.requestedSessions = requestedSessions;
    metrics.loopBatches = batches.size();
    metrics.payloadBytes = payload->size();
    metrics.routeLatency = metrics.routedAt - requestedAt;
    for (const auto& batch : batches) {
        metrics.fanoutConnections += batch.connections.size() + batch.endpoints.size();
    }
    if (metrics.requestedSessions == 0) {
        metrics.requestedSessions = metrics.fanoutConnections;
    }
    broadcastDispatcher_->dispatch(std::move(batches), std::move(payload), metrics);
}

void TcpServer::broadcastInLoopWithMetrics(buffer::PayloadPtr payload, mini::base::Timestamp requestedAt) {
    if (!broadcastRouter_ || !broadcastDispatcher_ || !payload) {
        return;
    }
    const auto routeStartedAt = mini::base::now();
    auto batches = broadcastRouter_->routeAll();
    const auto routedAt = mini::base::now();

    broadcast::BroadcastDispatcher::DispatchMetricContext metrics;
    metrics.requestedAt = requestedAt;
    metrics.routedAt = routedAt;
    metrics.targeted = false;
    metrics.loopBatches = batches.size();
    metrics.payloadBytes = payload->size();
    metrics.routeLatency = routedAt - routeStartedAt;
    for (const auto& batch : batches) {
        metrics.fanoutConnections += batch.connections.size() + batch.endpoints.size();
    }
    metrics.requestedSessions = metrics.fanoutConnections;

    broadcastDispatcher_->dispatch(std::move(batches), std::move(payload), metrics);
}

// ── Metrics hooks ──

void TcpServer::setConnectionEventCallback(ConnectionEventCallback cb) {
    connectionEventCallback_ = std::move(cb);
}

void TcpServer::setBackpressureEventCallback(BackpressureEventCallback cb) {
    backpressureEventCallback_ = std::move(cb);
}

void TcpServer::setTlsEventCallback(TlsEventCallback cb) {
    tlsEventCallback_ = std::move(cb);
}

void TcpServer::setBroadcastMetricCallback(BroadcastMetricCallback cb) {
    broadcastMetricCallback_ = std::move(cb);
    if (broadcastMetricCallback_) {
        broadcastMetricsEnabled_ = true;
    }
    configureBroadcastMetrics();
}

void TcpServer::setEventLoopMetricCallback(EventLoopMetricCallback cb) {
    eventLoopMetricCallback_ = std::move(cb);
    if (eventLoopMetricCallback_) {
        eventLoopQueueMetricsEnabled_ = true;
    }
}

void TcpServer::configureBroadcastMetrics() {
    if (!broadcastDispatcher_) {
        return;
    }
    if (broadcastMetricsEnabled_ && broadcastMetricCallback_) {
        broadcastDispatcher_->setBroadcastMetricCallback(broadcastMetricCallback_);
        return;
    }
    broadcastDispatcher_->setBroadcastMetricCallback({});
}

// ── Lifecycle ──

void TcpServer::start() {
    bool expected = false;
    if (started_.compare_exchange_strong(expected, true)) {
        stopped_ = false;
        configureBroadcastMetrics();
        auto loopMetricCallback = eventLoopQueueMetricsEnabled_
            ? eventLoopMetricCallback_
            : EventLoopMetricCallback{};
        if (loopMetricCallback) {
            loop_->setEventLoopMetricCallback(loopMetricCallback);
        }
        auto threadInitCallback = threadInitCallback_;
        threadPool_->start([loopMetricCallback, threadInitCallback](EventLoop* loop) {
            if (loopMetricCallback) {
                loop->setEventLoopMetricCallback(loopMetricCallback);
            }
            if (threadInitCallback) {
                threadInitCallback(loop);
            }
        });
        loop_->runInLoop([this] { acceptor_->listen(); });
    }
}

void TcpServer::stop() {
    loop_->assertInLoopThread();

    // Idempotent: if already stopped, return immediately.
    if (stopped_) {
        return;
    }
    stopped_ = true;
    draining_ = false;

    // 1. Stop accepting new connections.
    if (acceptor_->listening()) {
        acceptor_->setNewConnectionCallback({});
        acceptor_->stop();
    }

    // 2. Force-close all existing connections.
    forceCloseAllConnections();

    // 3. Stop worker loops (quit + join).
    threadPool_->stop();
}

void TcpServer::stop(Duration drainTimeout) {
    loop_->assertInLoopThread();

    if (stopped_) {
        return;
    }
    stopped_ = true;
    draining_ = true;

    // 1. Stop accepting new connections.
    if (acceptor_->listening()) {
        acceptor_->setNewConnectionCallback({});
        acceptor_->stop();
    }

    // 2. If no active connections, finish immediately.
    if (connections_.empty()) {
        draining_ = false;
        threadPool_->stop();
        return;
    }

    // 3. Set drain timeout timer.
    drainTimerId_ = loop_->runAfter(drainTimeout, [this] { onDrainTimeout(); });
}

void TcpServer::onDrainTimeout() {
    drainTimerId_ = {};
    if (!draining_) {
        return;
    }
    draining_ = false;
    LOG_WARN << "TcpServer::stop drain timeout, force-closing "
             << connections_.size() << " remaining connections";
    forceCloseAllConnections();
    threadPool_->stop();
}

void TcpServer::forceCloseAllConnections() {
    // Notify force-close via hook for each connection.
    if (connectionEventCallback_) {
        for (auto& [name, connection] : connections_) {
            if (connection->connected()) {
                connectionEventCallback_(connection, ConnectionEvent::ForceClosed);
            }
        }
    }

    auto conns = connections_;
    connections_.clear();
    for (auto& [name, connection] : conns) {
        if (broadcastRouter_) {
            broadcastRouter_->deregisterConnection(connection);
        }
        connection->setCloseCallback({});
        EventLoop* connLoop = connection->getLoop();
        if (connLoop == loop_) {
            connection->forceClose();
            connection->connectDestroyed();
        } else {
            connLoop->runInLoop([connection] {
                connection->forceClose();
                connection->connectDestroyed();
            });
        }
    }
}

void TcpServer::newConnection(int sockfd, const InetAddress& peerAddr) {
    loop_->assertInLoopThread();

    // If server is stopped/draining, reject the new connection.
    if (stopped_) {
        sockets::close(sockfd);
        return;
    }

    EventLoop* ioLoop = threadPool_->getNextLoop();
    const std::string connName = name_ + "#" + std::to_string(nextConnId_++);
    std::weak_ptr<void> lifetime = lifetimeToken_;

    const InetAddress localAddr(sockets::getLocalAddr(sockfd));
    auto connection = std::make_shared<TcpConnection>(ioLoop, connName, sockfd, localAddr, peerAddr);
    connections_[connName] = connection;
    if (broadcastRouter_) {
        broadcastRouter_->registerConnection(connection);
    }

    std::shared_ptr<IdleTimeoutState> idleState;
    if (idleTimeout_ > Duration::zero()) {
        idleState = std::make_shared<IdleTimeoutState>(IdleTimeoutState{
            .loop = ioLoop,
            .connection = connection,
            .timeout = idleTimeout_,
            .connectionEventCallback = connectionEventCallback_,
        });
    }

    // Capture hooks for this connection.
    auto connEventCb = connectionEventCallback_;
    auto bpEventCb = backpressureEventCallback_;
    auto tlsEventCb = tlsEventCallback_;

    connection->setConnectionCallback([cb = connectionCallback_, idleState, connEventCb](const TcpConnectionPtr& conn) {
        if (idleState != nullptr) {
            if (conn->connected()) {
                refreshIdleTimer(idleState);
            } else {
                cancelIdleTimer(idleState);
            }
        }
        if (connEventCb) {
            connEventCb(conn, conn->connected() ? ConnectionEvent::Connected : ConnectionEvent::Disconnected);
        }
        if (cb) {
            cb(conn);
        }
    });
    connection->setMessageCallback([cb = messageCallback_, logicCb = logicMessageCallback_, idleState](const TcpConnectionPtr& conn, Buffer* buffer) {
        if (idleState != nullptr) {
            refreshIdleTimer(idleState);
        }
        if (logicCb) {
            logicCb(conn, std::string_view(buffer->peek(), buffer->readableBytes()));
        }
        if (cb) {
            cb(conn, buffer);
        }
    });
    if (backpressureHighWaterMark_ > 0) {
        connection->setBackpressurePolicy(backpressureHighWaterMark_, backpressureLowWaterMark_);
    }
    if (highWaterMarkCallback_ && highWaterMark_ > 0) {
        connection->setHighWaterMarkCallback(highWaterMarkCallback_, highWaterMark_);
    }
    connection->setWriteCompleteCallback([cb = writeCompleteCallback_, idleState](const TcpConnectionPtr& conn) {
        if (idleState != nullptr) {
            refreshIdleTimer(idleState);
        }
        if (cb) {
            cb(conn);
        }
    });

    // Set backpressure event hook if configured.
    if (bpEventCb) {
        connection->setBackpressureEventCallback(bpEventCb);
    }

    // Set TLS event hook if configured.
    if (tlsEventCb) {
        connection->setTlsEventCallback(tlsEventCb);
    }

    // Guard delayed close callbacks so worker-loop teardown never dereferences a dead TcpServer.
    connection->setCloseCallback([this, lifetime, idleState, connEventCb](const TcpConnectionPtr& conn) {
        if (!lifetime.lock()) {
            return;
        }
        if (idleState != nullptr) {
            cancelIdleTimer(idleState);
        }
        if (connEventCb) {
            connEventCb(conn, ConnectionEvent::Disconnected);
        }
        removeConnection(conn);
    });

    if (tlsContext_) {
        auto ctx = tlsContext_;
        if (tlsEventCb) {
            tlsEventCb(connection, TlsEvent::HandshakeStarted);
        }
        ioLoop->runInLoop([connection, ctx] {
            connection->startTls(ctx, /*isServer=*/true);
            connection->connectEstablished();
        });
    } else {
        ioLoop->runInLoop([connection] { connection->connectEstablished(); });
    }

    // In drain mode: if all connections closed, finish shutdown.
    // The check happens in removeConnectionInLoop when the last connection is removed.
}

void TcpServer::removeConnection(const TcpConnectionPtr& connection) {
    std::weak_ptr<void> lifetime = lifetimeToken_;
    loop_->runInLoop([this, lifetime, connection] {
        if (!lifetime.lock()) {
            return;
        }
        removeConnectionInLoop(connection);
    });
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& connection) {
    loop_->assertInLoopThread();
    const auto erased = connections_.erase(connection->name());
    if (broadcastRouter_) {
        broadcastRouter_->deregisterConnection(connection);
    }

    if (erased == 0) {
        return;
    }

    auto* connectionLoop = connection->getLoop();
    if (connectionLoop) {
        connectionLoop->runInLoop([connection] { connection->connectDestroyed(); });
    }

    // In drain mode: if all connections closed, finish shutdown.
    if (draining_ && connections_.empty()) {
        draining_ = false;
        if (drainTimerId_.valid()) {
            loop_->cancel(drainTimerId_);
            drainTimerId_ = {};
        }
        threadPool_->stop();
    }
}

}  // namespace mini::net
