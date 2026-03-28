#include "mini/net/TcpServer.h"

#include "mini/net/EventLoop.h"
#include "mini/net/SocketsOps.h"
#include "mini/net/TcpConnection.h"

#include <utility>

namespace mini::net {

TcpServer::TcpServer(EventLoop* loop, const InetAddress& listenAddr, std::string name, bool reusePort)
    : loop_(loop),
      name_(std::move(name)),
      acceptor_(std::make_unique<Acceptor>(loop, listenAddr, reusePort)),
      threadPool_(std::make_shared<EventLoopThreadPool>(loop, name_)),
      started_(false),
      nextConnId_(1) {
    acceptor_->setNewConnectionCallback(
        [this](int sockfd, const InetAddress& peerAddr) { newConnection(sockfd, peerAddr); });
}

TcpServer::~TcpServer() {
    for (auto& [name, connection] : connections_) {
        auto conn = connection;
        conn->getLoop()->runInLoop([conn] { conn->connectDestroyed(); });
    }
}

void TcpServer::setThreadNum(int numThreads) {
    threadPool_->setThreadNum(numThreads);
}

void TcpServer::setThreadInitCallback(ThreadInitCallback cb) {
    threadInitCallback_ = std::move(cb);
}

void TcpServer::setConnectionCallback(ConnectionCallback cb) {
    connectionCallback_ = std::move(cb);
}

void TcpServer::setMessageCallback(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

void TcpServer::setWriteCompleteCallback(WriteCompleteCallback cb) {
    writeCompleteCallback_ = std::move(cb);
}

void TcpServer::start() {
    bool expected = false;
    if (started_.compare_exchange_strong(expected, true)) {
        threadPool_->start(threadInitCallback_);
        loop_->runInLoop([this] { acceptor_->listen(); });
    }
}

void TcpServer::newConnection(int sockfd, const InetAddress& peerAddr) {
    loop_->assertInLoopThread();
    EventLoop* ioLoop = threadPool_->getNextLoop();
    const std::string connName = name_ + "#" + std::to_string(nextConnId_++);

    const InetAddress localAddr(sockets::getLocalAddr(sockfd));
    auto connection = std::make_shared<TcpConnection>(ioLoop, connName, sockfd, localAddr, peerAddr);
    connections_[connName] = connection;

    connection->setConnectionCallback(connectionCallback_);
    connection->setMessageCallback(messageCallback_);
    connection->setWriteCompleteCallback(writeCompleteCallback_);
    connection->setCloseCallback([this](const TcpConnectionPtr& conn) { removeConnection(conn); });

    ioLoop->runInLoop([connection] { connection->connectEstablished(); });
}

void TcpServer::removeConnection(const TcpConnectionPtr& connection) {
    loop_->runInLoop([this, connection] { removeConnectionInLoop(connection); });
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& connection) {
    loop_->assertInLoopThread();
    connections_.erase(connection->name());
    EventLoop* ioLoop = connection->getLoop();
    ioLoop->queueInLoop([connection] { connection->connectDestroyed(); });
}

}  // namespace mini::net
