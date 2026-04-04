#pragma once

// TcpConnection 表示一个绑定到单个 EventLoop 的 TCP 连接。
// 它统一管理连接状态、缓冲区、Channel 回调和 coroutine 恢复入口。

#include "mini/base/noncopyable.h"
#include "mini/net/Buffer.h"
#include "mini/net/Callbacks.h"
#include "mini/net/Channel.h"
#include "mini/net/InetAddress.h"
#include "mini/net/Socket.h"

#include <coroutine>
#include <cstddef>
#include <string>
#include <string_view>

namespace mini::net {

class EventLoop;

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
        std::string await_resume();

    private:
        TcpConnectionPtr connection_;
        std::size_t minBytes_;
    };

    class WriteAwaitable {
    public:
        WriteAwaitable(TcpConnectionPtr connection, std::string data);

        bool await_ready() const noexcept;
        void await_suspend(std::coroutine_handle<> handle);
        void await_resume() const;

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
    struct ReadAwaiterState {
        std::coroutine_handle<> handle{};
        std::size_t minBytes{1};
        bool active{false};
    };

    struct WriteAwaiterState {
        std::coroutine_handle<> handle{};
        bool active{false};
    };

    struct CloseAwaiterState {
        std::coroutine_handle<> handle{};
        bool active{false};
    };

    void handleRead(mini::base::Timestamp receiveTime);
    void handleWrite();
    void handleClose();
    void handleError(int savedErrno = 0);

    void sendInLoop(const char* data, std::size_t len);
    void shutdownInLoop();
    void forceCloseInLoop();
    void setState(StateE state) noexcept;

    bool canReadImmediately(std::size_t minBytes) const noexcept;
    std::string consumeReadableBytes(std::size_t minBytes);
    void armReadWaiter(std::coroutine_handle<> handle, std::size_t minBytes);
    void armWriteWaiter(std::coroutine_handle<> handle, std::string data);
    void armCloseWaiter(std::coroutine_handle<> handle);
    void queueResume(std::coroutine_handle<> handle);
    void resumeReadWaiterIfNeeded();
    void resumeWriteWaiterIfNeeded();
    void resumeAllWaitersOnClose();

    EventLoop* loop_;
    std::string name_;
    StateE state_;
    std::unique_ptr<Socket> socket_;
    std::unique_ptr<Channel> channel_;
    InetAddress localAddr_;
    InetAddress peerAddr_;
    Buffer inputBuffer_;
    Buffer outputBuffer_;
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    HighWaterMarkCallback highWaterMarkCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    CloseCallback closeCallback_;
    std::size_t highWaterMark_;
    bool reading_;
    ReadAwaiterState readWaiter_;
    WriteAwaiterState writeWaiter_;
    CloseAwaiterState closeWaiter_;
};

}  // namespace mini::net
