#include "mini/net/TcpServer.h"

#include "mini/base/Logger.h"
#include "mini/net/EventLoop.h"
#include "mini/net/SocketsOps.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TlsContext.h"

#include <cstdint>
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
      started_(false),
      stopped_(false),
      draining_(false),
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

// ── Lifecycle ──

void TcpServer::start() {
    bool expected = false;
    if (started_.compare_exchange_strong(expected, true)) {
        stopped_ = false;
        threadPool_->start(threadInitCallback_);
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
    connection->setMessageCallback([cb = messageCallback_, idleState](const TcpConnectionPtr& conn, Buffer* buffer) {
        if (idleState != nullptr) {
            refreshIdleTimer(idleState);
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
    connections_.erase(connection->name());
    EventLoop* ioLoop = connection->getLoop();
    ioLoop->queueInLoop([connection] { connection->connectDestroyed(); });

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
