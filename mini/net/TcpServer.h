#pragma once

// TcpServer 协调 Acceptor、线程池和连接生命周期。
// 连接映射由 base loop 线程维护，跨 loop 移除必须显式回流。

#include "mini/base/noncopyable.h"
#include "mini/net/Acceptor.h"
#include "mini/net/Callbacks.h"
#include "mini/net/EventLoopThreadPool.h"

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>

namespace mini::net {

class EventLoop;

class TcpServer : private mini::base::noncopyable {
public:
    TcpServer(EventLoop* loop, const InetAddress& listenAddr, std::string name, bool reusePort = true);
    ~TcpServer();

    void setThreadNum(int numThreads);
    void setThreadInitCallback(ThreadInitCallback cb);
    void setConnectionCallback(ConnectionCallback cb);
    void setMessageCallback(MessageCallback cb);
    void setWriteCompleteCallback(WriteCompleteCallback cb);

    void start();

private:
    void newConnection(int sockfd, const InetAddress& peerAddr);
    void removeConnection(const TcpConnectionPtr& connection);
    void removeConnectionInLoop(const TcpConnectionPtr& connection);

    EventLoop* loop_;
    const std::string name_;
    std::unique_ptr<Acceptor> acceptor_;
    std::shared_ptr<EventLoopThreadPool> threadPool_;
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    ThreadInitCallback threadInitCallback_;
    std::atomic<bool> started_;
    int nextConnId_;
    std::unordered_map<std::string, TcpConnectionPtr> connections_;
    std::shared_ptr<void> lifetimeToken_;
};

}  // namespace mini::net
