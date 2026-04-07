#pragma once

// TcpConnection 表示一个绑定到单个 EventLoop 的 TCP 连接。
// 它统一管理连接状态、缓冲区、Channel 回调和 coroutine 恢复入口。
// 可选支持 TLS：通过 startTls() 激活后，read/write 透明走 SSL 路径。

#include "mini/base/noncopyable.h"
#include "mini/coroutine/CancellationToken.h"
#include "mini/base/Timestamp.h"
#include "mini/net/Callbacks.h"
#include "mini/net/InetAddress.h"
#include "mini/net/NetError.h"

#include <any>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace mini::net {

class EventLoop;
class TlsContext;

class TcpConnection : public std::enable_shared_from_this<TcpConnection>, private mini::base::noncopyable {
public:
    enum StateE { kConnecting, kConnected, kDisconnecting, kDisconnected };
    struct AwaitCancellationState {
        bool cancelled{false};
        std::optional<mini::coroutine::CancellationRegistration> registration;
    };

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

    void setContext(std::any context);
    const std::any& getContext() const noexcept;
    std::any& getContext() noexcept;

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
        ReadAwaitable(
            TcpConnectionPtr connection,
            std::size_t minBytes,
            mini::coroutine::CancellationToken token = {});

        bool await_ready() const noexcept;
        template <typename Promise>
        void await_suspend(std::coroutine_handle<Promise> handle);
        Expected<std::string> await_resume();

    private:
        TcpConnectionPtr connection_;
        std::size_t minBytes_;
        std::shared_ptr<AwaitCancellationState> cancellationState_;
        mini::coroutine::CancellationToken token_;
    };

    class WriteAwaitable {
    public:
        WriteAwaitable(
            TcpConnectionPtr connection,
            std::string data,
            mini::coroutine::CancellationToken token = {});

        bool await_ready() const noexcept;
        template <typename Promise>
        void await_suspend(std::coroutine_handle<Promise> handle);
        Expected<void> await_resume() const;

    private:
        TcpConnectionPtr connection_;
        std::string data_;
        std::shared_ptr<AwaitCancellationState> cancellationState_;
        mini::coroutine::CancellationToken token_;
    };

    class CloseAwaitable {
    public:
        explicit CloseAwaitable(
            TcpConnectionPtr connection,
            mini::coroutine::CancellationToken token = {});

        bool await_ready() const noexcept;
        template <typename Promise>
        void await_suspend(std::coroutine_handle<Promise> handle);
        Expected<void> await_resume() const noexcept;

    private:
        TcpConnectionPtr connection_;
        std::shared_ptr<AwaitCancellationState> cancellationState_;
        mini::coroutine::CancellationToken token_;
    };

    ReadAwaitable asyncReadSome(
        std::size_t minBytes = 1,
        mini::coroutine::CancellationToken token = {});
    WriteAwaitable asyncWrite(
        std::string data,
        mini::coroutine::CancellationToken token = {});
    CloseAwaitable waitClosed(mini::coroutine::CancellationToken token = {});

private:
    struct Impl;

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
    bool isReadAwaitReady(std::size_t minBytes) const noexcept;
    bool isWriteAwaitReady(std::string_view data) const noexcept;
    bool isCloseAwaitReady() const noexcept;
    Expected<std::string> resumeReadAwait(std::size_t minBytes);
    Expected<void> resumeWriteAwait() const;
    Expected<void> resumeCloseAwait() const noexcept;
    void cancelReadWaiter(
        std::coroutine_handle<> handle,
        const std::shared_ptr<AwaitCancellationState>& cancellationState);
    void cancelWriteWaiter(
        std::coroutine_handle<> handle,
        const std::shared_ptr<AwaitCancellationState>& cancellationState);
    void cancelCloseWaiter(
        std::coroutine_handle<> handle,
        const std::shared_ptr<AwaitCancellationState>& cancellationState);

    std::unique_ptr<Impl> impl_;
};

template <typename Promise>
void TcpConnection::ReadAwaitable::await_suspend(std::coroutine_handle<Promise> handle) {
    connection_->armReadWaiter(handle, minBytes_);

    auto token = token_;
    if (!token) {
        if constexpr (requires(const Promise& promise) { promise.cancellationToken(); }) {
            token = handle.promise().cancellationToken();
        }
    }
    if (token && connection_) {
        auto state = cancellationState_;
        auto connection = connection_;
        state->registration.emplace(token.registerCallback([connection, state, handle] {
            connection->cancelReadWaiter(handle, state);
        }));
    }
}

template <typename Promise>
void TcpConnection::WriteAwaitable::await_suspend(std::coroutine_handle<Promise> handle) {
    connection_->armWriteWaiter(handle, std::move(data_));

    auto token = token_;
    if (!token) {
        if constexpr (requires(const Promise& promise) { promise.cancellationToken(); }) {
            token = handle.promise().cancellationToken();
        }
    }
    if (token && connection_) {
        auto state = cancellationState_;
        auto connection = connection_;
        state->registration.emplace(token.registerCallback([connection, state, handle] {
            connection->cancelWriteWaiter(handle, state);
        }));
    }
}

template <typename Promise>
void TcpConnection::CloseAwaitable::await_suspend(std::coroutine_handle<Promise> handle) {
    connection_->armCloseWaiter(handle);

    auto token = token_;
    if (!token) {
        if constexpr (requires(const Promise& promise) { promise.cancellationToken(); }) {
            token = handle.promise().cancellationToken();
        }
    }
    if (token && connection_) {
        auto state = cancellationState_;
        auto connection = connection_;
        state->registration.emplace(token.registerCallback([connection, state, handle] {
            connection->cancelCloseWaiter(handle, state);
        }));
    }
}

}  // namespace mini::net
