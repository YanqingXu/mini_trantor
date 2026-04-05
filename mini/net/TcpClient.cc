#include "mini/net/TcpClient.h"

#include "mini/net/Connector.h"
#include "mini/net/EventLoop.h"
#include "mini/net/SocketsOps.h"
#include "mini/net/TcpConnection.h"

#include <cassert>
#include <cstdio>
#include <utility>

namespace mini::net {

TcpClient::TcpClient(EventLoop* loop, const InetAddress& serverAddr, std::string name)
    : loop_(loop),
      name_(std::move(name)),
      connector_(std::make_shared<Connector>(loop, serverAddr)),
      retry_(false),
      connect_(true),
      nextConnId_(1) {
    connector_->setNewConnectionCallback([this](int sockfd) { newConnection(sockfd); });
}

TcpClient::~TcpClient() {
    loop_->assertInLoopThread();

    TcpConnectionPtr conn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        conn = connection_;
    }

    if (conn) {
        // Detach connection from this client: clear close callback,
        // then destroy the connection on its owner loop.
        EventLoop* ioLoop = conn->getLoop();
        ioLoop->runInLoop([conn] {
            conn->setCloseCallback({});
            conn->connectDestroyed();
        });
    }

    connector_->stop();
}

void TcpClient::connect() {
    connect_ = true;
    connector_->start();
}

void TcpClient::disconnect() {
    connect_ = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connection_) {
            connection_->shutdown();
        }
    }
}

void TcpClient::stop() {
    connect_ = false;
    connector_->stop();
}

void TcpClient::enableRetry() noexcept {
    retry_ = true;
}

void TcpClient::disableRetry() noexcept {
    retry_ = false;
}

bool TcpClient::retry() const noexcept {
    return retry_;
}

const std::string& TcpClient::name() const noexcept {
    return name_;
}

EventLoop* TcpClient::getLoop() const noexcept {
    return loop_;
}

TcpConnectionPtr TcpClient::connection() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connection_;
}

void TcpClient::setConnectionCallback(ConnectionCallback cb) {
    connectionCallback_ = std::move(cb);
}

void TcpClient::setMessageCallback(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

void TcpClient::setWriteCompleteCallback(WriteCompleteCallback cb) {
    writeCompleteCallback_ = std::move(cb);
}

void TcpClient::newConnection(int sockfd) {
    loop_->assertInLoopThread();

    const InetAddress peerAddr(sockets::getPeerAddr(sockfd));
    const InetAddress localAddr(sockets::getLocalAddr(sockfd));
    const std::string connName = name_ + "#" + std::to_string(nextConnId_++);

    auto conn = std::make_shared<TcpConnection>(loop_, connName, sockfd, localAddr, peerAddr);
    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);
    conn->setCloseCallback([this](const TcpConnectionPtr& c) { removeConnection(c); });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_ = conn;
    }

    conn->connectEstablished();
}

void TcpClient::removeConnection(const TcpConnectionPtr& conn) {
    loop_->assertInLoopThread();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        assert(connection_ == conn);
        connection_.reset();
    }

    loop_->queueInLoop([conn] { conn->connectDestroyed(); });

    if (retry_ && connect_) {
        connector_->restart();
    }
}

}  // namespace mini::net
