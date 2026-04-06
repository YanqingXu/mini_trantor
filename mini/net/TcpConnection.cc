#include "mini/net/TcpConnection.h"

#include "mini/base/Logger.h"
#include "mini/net/EventLoop.h"
#include "mini/net/SocketsOps.h"
#include "mini/net/TlsContext.h"
#include "mini/net/detail/ConnectionAwaiterRegistry.h"
#include "mini/net/detail/ConnectionBackpressureController.h"
#include "mini/net/detail/ConnectionCallbackDispatcher.h"
#include "mini/net/detail/ConnectionTransport.h"

#include <stdexcept>
#include <utility>

namespace mini::net {

TcpConnection::TcpConnection(
    EventLoop* loop,
    std::string name,
    int sockfd,
    const InetAddress& localAddr,
    const InetAddress& peerAddr)
    : loop_(loop),
      name_(std::move(name)),
      state_(kConnecting),
      socket_(std::make_unique<Socket>(sockfd)),
      channel_(std::make_unique<Channel>(loop, sockfd)),
      localAddr_(localAddr),
      peerAddr_(peerAddr),
      callbacks_(std::make_unique<detail::ConnectionCallbackDispatcher>()),
      backpressure_(std::make_unique<detail::ConnectionBackpressureController>(loop)),
      awaiters_(std::make_unique<detail::ConnectionAwaiterRegistry>(loop)),
      transport_(std::make_unique<detail::ConnectionTransport>()) {
    channel_->setReadCallback([this](mini::base::Timestamp receiveTime) { handleRead(receiveTime); });
    channel_->setWriteCallback([this] { handleWrite(); });
    channel_->setCloseCallback([this] { handleClose(); });
    channel_->setErrorCallback([this] { handleError(); });
}

TcpConnection::~TcpConnection() = default;

EventLoop* TcpConnection::getLoop() const noexcept {
    return loop_;
}

const std::string& TcpConnection::name() const noexcept {
    return name_;
}

const InetAddress& TcpConnection::localAddress() const noexcept {
    return localAddr_;
}

const InetAddress& TcpConnection::peerAddress() const noexcept {
    return peerAddr_;
}

bool TcpConnection::connected() const noexcept {
    return state_ == kConnected;
}

bool TcpConnection::disconnected() const noexcept {
    return state_ == kDisconnected;
}

void TcpConnection::send(std::string_view message) {
    send(message.data(), message.size());
}

void TcpConnection::send(const void* data, std::size_t len) {
    if (state_ != kConnected) {
        return;
    }

    if (loop_->isInLoopThread()) {
        sendInLoop(static_cast<const char*>(data), len);
        return;
    }

    auto self = shared_from_this();
    std::string payload(static_cast<const char*>(data), len);
    loop_->runInLoop([self, payload = std::move(payload)]() mutable { self->sendInLoop(payload.data(), payload.size()); });
}

void TcpConnection::shutdown() {
    if (state_ == kConnected) {
        setState(kDisconnecting);
        auto self = shared_from_this();
        loop_->runInLoop([self] { self->shutdownInLoop(); });
    }
}

void TcpConnection::forceClose() {
    auto self = shared_from_this();
    if (loop_->isInLoopThread()) {
        forceCloseInLoop();
    } else {
        loop_->runInLoop([self] { self->forceCloseInLoop(); });
    }
}

void TcpConnection::setTcpNoDelay(bool on) {
    socket_->setTcpNoDelay(on);
}

void TcpConnection::setBackpressurePolicy(std::size_t highWaterMark, std::size_t lowWaterMark) {
    detail::ConnectionBackpressureController::validateThresholds(highWaterMark, lowWaterMark);

    if (loop_->isInLoopThread()) {
        setBackpressurePolicyInLoop(highWaterMark, lowWaterMark);
        return;
    }

    auto self = shared_from_this();
    loop_->runInLoop([self, highWaterMark, lowWaterMark] {
        self->setBackpressurePolicyInLoop(highWaterMark, lowWaterMark);
    });
}

void TcpConnection::startTls(std::shared_ptr<TlsContext> ctx, bool isServer, const std::string& hostname) {
    loop_->assertInLoopThread();
    transport_->enableTls(socket_->fd(), std::move(ctx), isServer, hostname);
}

bool TcpConnection::isTlsEstablished() const noexcept {
    return transport_->isTlsEstablished();
}

void TcpConnection::setConnectionCallback(ConnectionCallback cb) {
    callbacks_->setConnectionCallback(std::move(cb));
}

void TcpConnection::setMessageCallback(MessageCallback cb) {
    callbacks_->setMessageCallback(std::move(cb));
}

void TcpConnection::setHighWaterMarkCallback(HighWaterMarkCallback cb, std::size_t highWaterMark) {
    callbacks_->setHighWaterMarkCallback(std::move(cb), highWaterMark);
}

void TcpConnection::setWriteCompleteCallback(WriteCompleteCallback cb) {
    callbacks_->setWriteCompleteCallback(std::move(cb));
}

void TcpConnection::setCloseCallback(CloseCallback cb) {
    callbacks_->setCloseCallback(std::move(cb));
}

void TcpConnection::connectEstablished() {
    loop_->assertInLoopThread();
    setState(kConnected);
    channel_->tie(shared_from_this());
    channel_->enableReading();
    backpressure_->onConnectionEstablished(outputBuffer_.readableBytes(), *channel_);

    if (transport_->handshakePending()) {
        advanceTransportHandshake();
        return;
    }

    notifyConnected(shared_from_this());
}

void TcpConnection::connectDestroyed() {
    loop_->assertInLoopThread();
    if (state_ == kConnected) {
        runCloseSequence(shared_from_this(), false);
    }
    channel_->remove();
}

TcpConnection::ReadAwaitable::ReadAwaitable(TcpConnectionPtr connection, std::size_t minBytes)
    : connection_(std::move(connection)), minBytes_(minBytes) {
}

bool TcpConnection::ReadAwaitable::await_ready() const noexcept {
    return !connection_ ||
           (connection_->getLoop()->isInLoopThread() && connection_->canReadImmediately(minBytes_));
}

void TcpConnection::ReadAwaitable::await_suspend(std::coroutine_handle<> handle) {
    connection_->armReadWaiter(handle, minBytes_);
}

Expected<std::string> TcpConnection::ReadAwaitable::await_resume() {
    if (!connection_) {
        return std::unexpected(NetError::NotConnected);
    }
    if (connection_->disconnected() && connection_->inputBuffer_.readableBytes() == 0) {
        return std::unexpected(NetError::PeerClosed);
    }
    return connection_->consumeReadableBytes(minBytes_);
}

TcpConnection::WriteAwaitable::WriteAwaitable(TcpConnectionPtr connection, std::string data)
    : connection_(std::move(connection)), data_(std::move(data)) {
}

bool TcpConnection::WriteAwaitable::await_ready() const noexcept {
    return !connection_ ||
           (connection_->getLoop()->isInLoopThread() && (data_.empty() || connection_->disconnected()));
}

void TcpConnection::WriteAwaitable::await_suspend(std::coroutine_handle<> handle) {
    connection_->armWriteWaiter(handle, std::move(data_));
}

Expected<void> TcpConnection::WriteAwaitable::await_resume() const {
    if (!connection_) {
        return std::unexpected(NetError::NotConnected);
    }
    if (connection_->disconnected()) {
        return std::unexpected(NetError::PeerClosed);
    }
    return {};
}

TcpConnection::CloseAwaitable::CloseAwaitable(TcpConnectionPtr connection) : connection_(std::move(connection)) {
}

bool TcpConnection::CloseAwaitable::await_ready() const noexcept {
    return !connection_ ||
           (connection_->getLoop()->isInLoopThread() && connection_->disconnected());
}

void TcpConnection::CloseAwaitable::await_suspend(std::coroutine_handle<> handle) {
    connection_->armCloseWaiter(handle);
}

void TcpConnection::CloseAwaitable::await_resume() const noexcept {
}

TcpConnection::ReadAwaitable TcpConnection::asyncReadSome(std::size_t minBytes) {
    return ReadAwaitable(shared_from_this(), minBytes);
}

TcpConnection::WriteAwaitable TcpConnection::asyncWrite(std::string data) {
    return WriteAwaitable(shared_from_this(), std::move(data));
}

TcpConnection::CloseAwaitable TcpConnection::waitClosed() {
    return CloseAwaitable(shared_from_this());
}

void TcpConnection::handleRead(mini::base::Timestamp receiveTime) {
    (void)receiveTime;
    loop_->assertInLoopThread();

    if (transport_->handshakePending()) {
        advanceTransportHandshake();
        return;
    }

    const bool hasReadWaiter = awaiters_->hasReadWaiter();
    const auto result = transport_->readInto(inputBuffer_, *channel_);

    if (result.status == detail::ConnectionTransport::Status::kOk) {
        auto self = shared_from_this();
        awaiters_->resumeReadWaiterIfSatisfied(inputBuffer_.readableBytes());
        callbacks_->notifyMessage(self, &inputBuffer_, hasReadWaiter);
        return;
    }

    if (result.status == detail::ConnectionTransport::Status::kPeerClosed) {
        handleClose();
        return;
    }

    if (result.status == detail::ConnectionTransport::Status::kError) {
        handleError(result.savedErrno);
    }
}

void TcpConnection::handleWrite() {
    loop_->assertInLoopThread();

    if (transport_->handshakePending()) {
        advanceTransportHandshake();
        return;
    }

    if (!channel_->isWriting()) {
        return;
    }

    const auto result = transport_->writeFromBuffer(outputBuffer_, *channel_);
    if (result.status == detail::ConnectionTransport::Status::kError) {
        handleError(result.savedErrno);
        return;
    }

    if (result.status == detail::ConnectionTransport::Status::kOk) {
        applyBackpressurePolicy();
        if (outputBuffer_.readableBytes() == 0) {
            channel_->disableWriting();
            finishPendingWrite(shared_from_this());
        }
    }
}

void TcpConnection::handleClose() {
    loop_->assertInLoopThread();
    if (state_ == kDisconnected) {
        return;
    }
    runCloseSequence(shared_from_this(), true);
}

void TcpConnection::handleError(int savedErrno) {
    loop_->assertInLoopThread();
    const int err = savedErrno != 0 ? savedErrno : sockets::getSocketError(channel_->fd());
    LOG_ERROR << "TcpConnection error on " << name_ << ": " << err;
    if (state_ != kDisconnected) {
        handleClose();
    }
}

void TcpConnection::sendInLoop(const char* data, std::size_t len) {
    loop_->assertInLoopThread();
    if (state_ == kDisconnected) {
        return;
    }

    std::size_t remaining = len;
    std::size_t nwrote = 0;

    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
        const auto result = transport_->writeRaw(std::string_view(data, len), *channel_);
        if (result.status == detail::ConnectionTransport::Status::kError) {
            handleError(result.savedErrno);
            return;
        }

        nwrote = result.bytes;
        remaining = len - nwrote;
        if (remaining == 0) {
            finishPendingWrite(shared_from_this());
            return;
        }
    }

    if (remaining > 0) {
        const auto oldLen = outputBuffer_.readableBytes();
        outputBuffer_.append(data + nwrote, remaining);
        if (!channel_->isWriting()) {
            channel_->enableWriting();
        }
        const auto newLen = outputBuffer_.readableBytes();
        maybeQueueHighWaterMark(shared_from_this(), oldLen, newLen);
        applyBackpressurePolicy();
    }
}

void TcpConnection::shutdownInLoop() {
    loop_->assertInLoopThread();
    if (!channel_->isWriting()) {
        transport_->shutdownWrite(*socket_);
    }
}

void TcpConnection::forceCloseInLoop() {
    loop_->assertInLoopThread();
    if (state_ == kConnected || state_ == kDisconnecting) {
        handleClose();
    }
}

void TcpConnection::setBackpressurePolicyInLoop(std::size_t highWaterMark, std::size_t lowWaterMark) {
    loop_->assertInLoopThread();
    backpressure_->configure(
        highWaterMark,
        lowWaterMark,
        outputBuffer_.readableBytes(),
        *channel_);
}

void TcpConnection::applyBackpressurePolicy() {
    backpressure_->onBufferedBytesChanged(outputBuffer_.readableBytes(), *channel_);
}

void TcpConnection::setState(StateE state) noexcept {
    state_ = state;
}

void TcpConnection::advanceTransportHandshake() {
    loop_->assertInLoopThread();
    const auto result = transport_->advanceHandshake(*channel_);
    if (result.failed) {
        handleError(result.savedErrno);
        return;
    }
    if (result.completed) {
        notifyConnected(shared_from_this());
    }
}

void TcpConnection::runCloseSequence(const TcpConnectionPtr& connection, bool notifyCloseCallback) {
    loop_->assertInLoopThread();
    setState(kDisconnected);
    backpressure_->onClosed();
    channel_->disableAll();
    awaiters_->resumeAllOnClose();
    notifyDisconnected(connection);
    if (notifyCloseCallback) {
        callbacks_->notifyClose(connection);
    }
}

void TcpConnection::notifyConnected(const TcpConnectionPtr& connection) {
    callbacks_->notifyConnected(connection);
}

void TcpConnection::notifyDisconnected(const TcpConnectionPtr& connection) {
    callbacks_->notifyDisconnected(connection);
}

void TcpConnection::finishPendingWrite(const TcpConnectionPtr& connection) {
    callbacks_->queueWriteComplete(*loop_, connection);
    awaiters_->resumeWriteWaiterIfNeeded();
    if (state_ == kDisconnecting) {
        shutdownInLoop();
    }
}

void TcpConnection::maybeQueueHighWaterMark(
    const TcpConnectionPtr& connection,
    std::size_t oldLen,
    std::size_t newLen) {
    callbacks_->queueHighWaterMark(*loop_, connection, oldLen, newLen);
}

bool TcpConnection::canReadImmediately(std::size_t minBytes) const noexcept {
    return inputBuffer_.readableBytes() >= minBytes || state_ != kConnected;
}

std::string TcpConnection::consumeReadableBytes(std::size_t minBytes) {
    loop_->assertInLoopThread();
    if (inputBuffer_.readableBytes() == 0) {
        return {};
    }
    if (inputBuffer_.readableBytes() < minBytes && state_ == kConnected) {
        return {};
    }
    return inputBuffer_.retrieveAllAsString();
}

void TcpConnection::armReadWaiter(std::coroutine_handle<> handle, std::size_t minBytes) {
    auto self = shared_from_this();
    auto action = [self, handle, minBytes] {
        self->loop_->assertInLoopThread();
        self->awaiters_->armReadWaiter(handle, minBytes, self->canReadImmediately(minBytes));
    };

    if (loop_->isInLoopThread()) {
        action();
    } else {
        loop_->queueInLoop(std::move(action));
    }
}

void TcpConnection::armWriteWaiter(std::coroutine_handle<> handle, std::string data) {
    auto self = shared_from_this();
    auto action = [self, handle, data = std::move(data)]() mutable {
        self->loop_->assertInLoopThread();
        self->awaiters_->armWriteWaiter(handle, data.empty() || self->state_ == kDisconnected);
        if (data.empty() || self->state_ == kDisconnected) {
            return;
        }

        self->sendInLoop(data.data(), data.size());
        if (!self->channel_->isWriting() && self->outputBuffer_.readableBytes() == 0) {
            self->awaiters_->resumeWriteWaiterIfNeeded();
        }
    };

    if (loop_->isInLoopThread()) {
        action();
    } else {
        loop_->queueInLoop(std::move(action));
    }
}

void TcpConnection::armCloseWaiter(std::coroutine_handle<> handle) {
    auto self = shared_from_this();
    auto action = [self, handle] {
        self->loop_->assertInLoopThread();
        self->awaiters_->armCloseWaiter(handle, self->state_ == kDisconnected);
    };

    if (loop_->isInLoopThread()) {
        action();
    } else {
        loop_->queueInLoop(std::move(action));
    }
}

}  // namespace mini::net
