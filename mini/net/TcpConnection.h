#pragma once

// TcpConnection 表示一个绑定到单个 EventLoop 的 TCP 连接。
// 它统一管理连接状态、缓冲区、Channel 回调和 coroutine 恢复入口。
// 可选支持 TLS：通过 startTls() 激活后，read/write 透明走 SSL 路径。

#include "mini/base/noncopyable.h"
#include "mini/net/Buffer.h"
#include "mini/net/Callbacks.h"
#include "mini/net/Channel.h"
#include "mini/net/InetAddress.h"
#include "mini/net/NetError.h"
#include "mini/net/Socket.h"

#include <any>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace mini::net {

class EventLoop;
class TlsContext;

namespace detail {
class ConnectionCallbackDispatcher;
class ConnectionTransport;
class ConnectionAwaiterRegistry;
class ConnectionBackpressureController;
}

class TcpConnection : public std::enable_shared_from_this<TcpConnection>, private mini::base::noncopyable {
public:
    enum StateE { kConnecting, kConnected, kDisconnecting, kDisconnected };

    TcpConnection(
        EventLoop* loop,
        std::string name,
        int sockfd,
        const InetAddress& localAddr,
        const InetAddress& peerAddr);
    ~TcpConnection();

    EventLoop* getLoop() const noexcept;
    const std::string& name() const noexcept;
    const InetAddress& localAddress() const noexcept;
    const InetAddress& peerAddress() const noexcept;
    bool connected() const noexcept;
    bool disconnected() const noexcept;

    void send(std::string_view message);
    void send(const void* data, std::size_t len);
    void shutdown();
    void forceClose();
    void setTcpNoDelay(bool on);
    void setBackpressurePolicy(std::size_t highWaterMark, std::size_t lowWaterMark);

    void setContext(std::any context) { context_ = std::move(context); }
    const std::any& getContext() const noexcept { return context_; }
    std::any& getContext() noexcept { return context_; }

    void startTls(std::shared_ptr<TlsContext> ctx, bool isServer, const std::string& hostname = "");
    bool isTlsEstablished() const noexcept;

    void setConnectionCallback(ConnectionCallback cb);
    void setMessageCallback(MessageCallback cb);
    void setHighWaterMarkCallback(HighWaterMarkCallback cb, std::size_t highWaterMark);
    void setWriteCompleteCallback(WriteCompleteCallback cb);
    void setCloseCallback(CloseCallback cb);

    void connectEstablished();
    void connectDestroyed();

    class ReadAwaitable {
    public:
        ReadAwaitable(TcpConnectionPtr connection, std::size_t minBytes);

        bool await_ready() const noexcept;
        void await_suspend(std::coroutine_handle<> handle);
        Expected<std::string> await_resume();

    private:
        TcpConnectionPtr connection_;
        std::size_t minBytes_;
    };

    class WriteAwaitable {
    public:
        WriteAwaitable(TcpConnectionPtr connection, std::string data);

        bool await_ready() const noexcept;
        void await_suspend(std::coroutine_handle<> handle);
        Expected<void> await_resume() const;

    private:
        TcpConnectionPtr connection_;
        std::string data_;
    };

    class CloseAwaitable {
    public:
        explicit CloseAwaitable(TcpConnectionPtr connection);

        bool await_ready() const noexcept;
        void await_suspend(std::coroutine_handle<> handle);
        void await_resume() const noexcept;

    private:
        TcpConnectionPtr connection_;
    };

    ReadAwaitable asyncReadSome(std::size_t minBytes = 1);
    WriteAwaitable asyncWrite(std::string data);
    CloseAwaitable waitClosed();

private:
    void handleRead(mini::base::Timestamp receiveTime);
    void handleWrite();
    void handleClose();
    void handleError(int savedErrno = 0);

    void sendInLoop(const char* data, std::size_t len);
    void shutdownInLoop();
    void forceCloseInLoop();
    void setBackpressurePolicyInLoop(std::size_t highWaterMark, std::size_t lowWaterMark);
    void applyBackpressurePolicy();
    void setState(StateE state) noexcept;
    void advanceTransportHandshake();
    void runCloseSequence(const TcpConnectionPtr& connection, bool notifyCloseCallback);
    void notifyConnected(const TcpConnectionPtr& connection);
    void notifyDisconnected(const TcpConnectionPtr& connection);
    void finishPendingWrite(const TcpConnectionPtr& connection);
    void maybeQueueHighWaterMark(const TcpConnectionPtr& connection, std::size_t oldLen, std::size_t newLen);

    bool canReadImmediately(std::size_t minBytes) const noexcept;
    std::string consumeReadableBytes(std::size_t minBytes);
    void armReadWaiter(std::coroutine_handle<> handle, std::size_t minBytes);
    void armWriteWaiter(std::coroutine_handle<> handle, std::string data);
    void armCloseWaiter(std::coroutine_handle<> handle);

    EventLoop* loop_;
    std::string name_;
    StateE state_;
    std::unique_ptr<Socket> socket_;
    std::unique_ptr<Channel> channel_;
    InetAddress localAddr_;
    InetAddress peerAddr_;
    Buffer inputBuffer_;
    Buffer outputBuffer_;
    std::unique_ptr<detail::ConnectionCallbackDispatcher> callbacks_;
    std::unique_ptr<detail::ConnectionBackpressureController> backpressure_;
    std::unique_ptr<detail::ConnectionAwaiterRegistry> awaiters_;
    std::unique_ptr<detail::ConnectionTransport> transport_;
    std::any context_;
};

}  // namespace mini::net
