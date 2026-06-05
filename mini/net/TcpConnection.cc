#include "mini/net/TcpConnection.h"

#include "mini/base/Logger.h"
#include "mini/net/Buffer.h"
#include "mini/net/Channel.h"
#include "mini/net/EventLoop.h"
#include "mini/net/Socket.h"
#include "mini/net/SocketsOps.h"
#include "mini/net/TlsContext.h"
#include "mini/net/detail/ConnectionAwaiterRegistry.h"
#include "mini/net/detail/ConnectionBackpressureController.h"
#include "mini/net/detail/ConnectionCallbackDispatcher.h"
#include "mini/net/detail/ConnectionTransport.h"

#include <stdexcept>
#include <utility>

namespace mini::net {

struct TcpConnection::Impl {
    enum class CloseReason {
        kNone,
        kPeerClosed,
        kError,
    };

    Impl(
        EventLoop* loop,
        std::string connName,
        int sockfd,
        const InetAddress& local,
        const InetAddress& peer)
        : loop(loop),
          name(std::move(connName)),
          state(TcpConnection::kConnecting),
          socket(std::make_unique<Socket>(sockfd)),
          channel(std::make_unique<Channel>(loop, sockfd)),
          localAddr(local),
          peerAddr(peer),
          callbacks(std::make_unique<detail::ConnectionCallbackDispatcher>()),
          backpressure(std::make_unique<detail::ConnectionBackpressureController>(loop)),
          awaiters(std::make_unique<detail::ConnectionAwaiterRegistry>(loop)),
          transport(std::make_unique<detail::ConnectionTransport>()),
          closeReason(CloseReason::kNone) {
    }

    EventLoop* loop;
    std::string name;
    TcpConnection::StateE state;
    std::unique_ptr<Socket> socket;
    std::unique_ptr<Channel> channel;
    InetAddress localAddr;
    InetAddress peerAddr;
    Buffer inputBuffer;
    Buffer outputBuffer;
    std::unique_ptr<detail::ConnectionCallbackDispatcher> callbacks;
    std::unique_ptr<detail::ConnectionBackpressureController> backpressure;
    std::unique_ptr<detail::ConnectionAwaiterRegistry> awaiters;
    std::unique_ptr<detail::ConnectionTransport> transport;
    CloseReason closeReason;
    std::any context;

    // Metrics hooks (v5-delta)
    BackpressureEventCallback backpressureEventCallback;
    TlsEventCallback tlsEventCallback;
};

TcpConnection::TcpConnection(
    EventLoop* loop,
    std::string name,
    int sockfd,
    const InetAddress& localAddr,
    const InetAddress& peerAddr)
    : impl_(std::make_unique<Impl>(loop, std::move(name), sockfd, localAddr, peerAddr)) {
    impl_->channel->setReadCallback([this](mini::base::Timestamp receiveTime) { handleRead(receiveTime); });
    impl_->channel->setWriteCallback([this] { handleWrite(); });
    impl_->channel->setCloseCallback([this] { handleClose(); });
    impl_->channel->setErrorCallback([this] { handleError(); });
}

TcpConnection::~TcpConnection() = default;

EventLoop* TcpConnection::getLoop() const noexcept {
    return impl_->loop;
}

const std::string& TcpConnection::name() const noexcept {
    return impl_->name;
}

const InetAddress& TcpConnection::localAddress() const noexcept {
    return impl_->localAddr;
}

const InetAddress& TcpConnection::peerAddress() const noexcept {
    return impl_->peerAddr;
}

bool TcpConnection::connected() const noexcept {
    return impl_->state == kConnected;
}

bool TcpConnection::disconnected() const noexcept {
    return impl_->state == kDisconnected;
}

void TcpConnection::send(std::string_view message) {
    send(message.data(), message.size());
}

void TcpConnection::send(const void* data, std::size_t len) {
    if (impl_->state != kConnected) {
        return;
    }

    if (impl_->loop->isInLoopThread()) {
        sendInLoop(static_cast<const char*>(data), len);
        return;
    }

    auto self = shared_from_this();
    std::string payload(static_cast<const char*>(data), len);
    impl_->loop->runInLoop([self, payload = std::move(payload)]() mutable { self->sendInLoop(payload.data(), payload.size()); });
}

void TcpConnection::shutdown() {
    if (impl_->state == kConnected) {
        setState(kDisconnecting);
        auto self = shared_from_this();
        impl_->loop->runInLoop([self] { self->shutdownInLoop(); });
    }
}

void TcpConnection::forceClose() {
    auto self = shared_from_this();
    if (impl_->loop->isInLoopThread()) {
        forceCloseInLoop();
    } else {
        impl_->loop->runInLoop([self] { self->forceCloseInLoop(); });
    }
}

void TcpConnection::setTcpNoDelay(bool on) {
    impl_->socket->setTcpNoDelay(on);
}

void TcpConnection::setBackpressurePolicy(std::size_t highWaterMark, std::size_t lowWaterMark) {
    detail::ConnectionBackpressureController::validateThresholds(highWaterMark, lowWaterMark);

    if (impl_->loop->isInLoopThread()) {
        setBackpressurePolicyInLoop(highWaterMark, lowWaterMark);
        return;
    }

    auto self = shared_from_this();
    impl_->loop->runInLoop([self, highWaterMark, lowWaterMark] {
        self->setBackpressurePolicyInLoop(highWaterMark, lowWaterMark);
    });
}

void TcpConnection::setContext(std::any context) {
    impl_->context = std::move(context);
}

const std::any& TcpConnection::getContext() const noexcept {
    return impl_->context;
}

std::any& TcpConnection::getContext() noexcept {
    return impl_->context;
}

void TcpConnection::startTls(std::shared_ptr<TlsContext> ctx, bool isServer, const std::string& hostname) {
    impl_->loop->assertInLoopThread();
    impl_->transport->enableTls(impl_->socket->fd(), std::move(ctx), isServer, hostname);
}

bool TcpConnection::isTlsEstablished() const noexcept {
    return impl_->transport->isTlsEstablished();
}

void TcpConnection::setConnectionCallback(ConnectionCallback cb) {
    impl_->callbacks->setConnectionCallback(std::move(cb));
}

void TcpConnection::setMessageCallback(MessageCallback cb) {
    impl_->callbacks->setMessageCallback(std::move(cb));
}

void TcpConnection::setHighWaterMarkCallback(HighWaterMarkCallback cb, std::size_t highWaterMark) {
    impl_->callbacks->setHighWaterMarkCallback(std::move(cb), highWaterMark);
}

void TcpConnection::setWriteCompleteCallback(WriteCompleteCallback cb) {
    impl_->callbacks->setWriteCompleteCallback(std::move(cb));
}

void TcpConnection::setCloseCallback(CloseCallback cb) {
    impl_->callbacks->setCloseCallback(std::move(cb));
}

void TcpConnection::setBackpressureEventCallback(BackpressureEventCallback cb) {
    impl_->backpressureEventCallback = cb;
    impl_->backpressure->setBackpressureEventCallback(std::move(cb));
    impl_->backpressure->setConnectionPtr(shared_from_this());
}

void TcpConnection::setTlsEventCallback(TlsEventCallback cb) {
    impl_->tlsEventCallback = std::move(cb);
}

void TcpConnection::connectEstablished() {
    impl_->loop->assertInLoopThread();
    setState(kConnected);
    impl_->closeReason = Impl::CloseReason::kNone;
    impl_->channel->tie(shared_from_this());
    impl_->channel->enableReading();
    impl_->backpressure->onConnectionEstablished(impl_->outputBuffer.readableBytes(), *impl_->channel);

    if (impl_->transport->handshakePending()) {
        advanceTransportHandshake();
        return;
    }

    notifyConnected(shared_from_this());
}

void TcpConnection::connectDestroyed() {
    impl_->loop->assertInLoopThread();
    if (impl_->state == kConnected) {
        if (impl_->closeReason == Impl::CloseReason::kNone) {
            impl_->closeReason = Impl::CloseReason::kPeerClosed;
        }
        runCloseSequence(shared_from_this(), false);
    }
    impl_->channel->remove();
}

TcpConnection::ReadAwaitable::ReadAwaitable(
    TcpConnectionPtr connection,
    std::size_t minBytes,
    mini::coroutine::CancellationToken token)
    : connection_(std::move(connection)),
      minBytes_(minBytes),
      cancellationState_(std::make_shared<AwaitCancellationState>()),
      token_(std::move(token)) {
}

bool TcpConnection::ReadAwaitable::await_ready() const noexcept {
    return !connection_ || connection_->isReadAwaitReady(minBytes_);
}

Expected<std::string> TcpConnection::ReadAwaitable::await_resume() {
    cancellationState_->registration.reset();
    if (cancellationState_->cancelled) {
        return std::unexpected(NetError::Cancelled);
    }
    return connection_ ? connection_->resumeReadAwait(minBytes_) : std::unexpected(NetError::NotConnected);
}

TcpConnection::WriteAwaitable::WriteAwaitable(
    TcpConnectionPtr connection,
    std::string data,
    mini::coroutine::CancellationToken token)
    : connection_(std::move(connection)),
      data_(std::move(data)),
      cancellationState_(std::make_shared<AwaitCancellationState>()),
      token_(std::move(token)) {
}

bool TcpConnection::WriteAwaitable::await_ready() const noexcept {
    return !connection_ || connection_->isWriteAwaitReady(data_);
}

Expected<void> TcpConnection::WriteAwaitable::await_resume() const {
    cancellationState_->registration.reset();
    if (cancellationState_->cancelled) {
        return std::unexpected(NetError::Cancelled);
    }
    return connection_ ? connection_->resumeWriteAwait() : std::unexpected(NetError::NotConnected);
}

TcpConnection::CloseAwaitable::CloseAwaitable(
    TcpConnectionPtr connection,
    mini::coroutine::CancellationToken token)
    : connection_(std::move(connection)),
      cancellationState_(std::make_shared<AwaitCancellationState>()),
      token_(std::move(token)) {
}

bool TcpConnection::CloseAwaitable::await_ready() const noexcept {
    return !connection_ || connection_->isCloseAwaitReady();
}

Expected<void> TcpConnection::CloseAwaitable::await_resume() const noexcept {
    cancellationState_->registration.reset();
    if (cancellationState_->cancelled) {
        return std::unexpected(NetError::Cancelled);
    }
    return connection_ ? connection_->resumeCloseAwait() : std::unexpected(NetError::NotConnected);
}

TcpConnection::ReadAwaitable TcpConnection::asyncReadSome(
    std::size_t minBytes,
    mini::coroutine::CancellationToken token) {
    return ReadAwaitable(shared_from_this(), minBytes, std::move(token));
}

TcpConnection::WriteAwaitable TcpConnection::asyncWrite(
    std::string data,
    mini::coroutine::CancellationToken token) {
    return WriteAwaitable(shared_from_this(), std::move(data), std::move(token));
}

TcpConnection::CloseAwaitable TcpConnection::waitClosed(mini::coroutine::CancellationToken token) {
    return CloseAwaitable(shared_from_this(), std::move(token));
}

void TcpConnection::handleRead(mini::base::Timestamp receiveTime) {
    (void)receiveTime;
    impl_->loop->assertInLoopThread();

    if (impl_->transport->handshakePending()) {
        advanceTransportHandshake();
        return;
    }

    const bool hasReadWaiter = impl_->awaiters->hasReadWaiter();
    const auto result = impl_->transport->readInto(impl_->inputBuffer, *impl_->channel);

    if (result.status == detail::ConnectionTransport::Status::kOk) {
        auto self = shared_from_this();
        impl_->awaiters->resumeReadWaiterIfSatisfied(impl_->inputBuffer.readableBytes());
        impl_->callbacks->notifyMessage(self, &impl_->inputBuffer, hasReadWaiter);
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
    impl_->loop->assertInLoopThread();

    if (impl_->transport->handshakePending()) {
        advanceTransportHandshake();
        return;
    }

    if (!impl_->channel->isWriting()) {
        return;
    }

    const auto result = impl_->transport->writeFromBuffer(impl_->outputBuffer, *impl_->channel);
    if (result.status == detail::ConnectionTransport::Status::kError) {
        handleError(result.savedErrno);
        return;
    }

    if (result.status == detail::ConnectionTransport::Status::kOk) {
        applyBackpressurePolicy();
        if (impl_->outputBuffer.readableBytes() == 0) {
            impl_->channel->disableWriting();
            finishPendingWrite(shared_from_this());
        }
    }
}

void TcpConnection::handleClose() {
    impl_->loop->assertInLoopThread();
    if (impl_->state == kDisconnected) {
        return;
    }
    if (impl_->closeReason == Impl::CloseReason::kNone) {
        impl_->closeReason = Impl::CloseReason::kPeerClosed;
    }
    runCloseSequence(shared_from_this(), true);
}

void TcpConnection::handleError(int savedErrno) {
    impl_->loop->assertInLoopThread();
    const int err = savedErrno != 0 ? savedErrno : sockets::getSocketError(impl_->channel->fd());
    LOG_ERROR << "TcpConnection error on " << impl_->name << ": " << err;
    impl_->closeReason = Impl::CloseReason::kError;
    if (impl_->state != kDisconnected) {
        handleClose();
    }
}

void TcpConnection::sendInLoop(const char* data, std::size_t len) {
    impl_->loop->assertInLoopThread();
    if (impl_->state == kDisconnected) {
        return;
    }

    std::size_t remaining = len;
    std::size_t nwrote = 0;

    if (!impl_->channel->isWriting() && impl_->outputBuffer.readableBytes() == 0) {
        const auto result = impl_->transport->writeRaw(std::string_view(data, len), *impl_->channel);
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
        const auto oldLen = impl_->outputBuffer.readableBytes();
        impl_->outputBuffer.append(data + nwrote, remaining);
        if (!impl_->channel->isWriting()) {
            impl_->channel->enableWriting();
        }
        const auto newLen = impl_->outputBuffer.readableBytes();
        maybeQueueHighWaterMark(shared_from_this(), oldLen, newLen);
        applyBackpressurePolicy();
    }
}

void TcpConnection::shutdownInLoop() {
    impl_->loop->assertInLoopThread();
    if (!impl_->channel->isWriting()) {
        impl_->transport->shutdownWrite(*impl_->socket);
    }
}

void TcpConnection::forceCloseInLoop() {
    impl_->loop->assertInLoopThread();
    if (impl_->state == kConnected || impl_->state == kDisconnecting) {
        handleClose();
    }
}

void TcpConnection::setBackpressurePolicyInLoop(std::size_t highWaterMark, std::size_t lowWaterMark) {
    impl_->loop->assertInLoopThread();
    impl_->backpressure->configure(
        highWaterMark,
        lowWaterMark,
        impl_->outputBuffer.readableBytes(),
        *impl_->channel);
}

void TcpConnection::applyBackpressurePolicy() {
    impl_->backpressure->onBufferedBytesChanged(impl_->outputBuffer.readableBytes(), *impl_->channel);
}

void TcpConnection::setState(StateE state) noexcept {
    impl_->state = state;
}

void TcpConnection::advanceTransportHandshake() {
    impl_->loop->assertInLoopThread();
    const auto result = impl_->transport->advanceHandshake(*impl_->channel);
    if (result.failed) {
        // Fire TLS handshake failed hook.
        if (impl_->tlsEventCallback) {
            impl_->tlsEventCallback(shared_from_this(), TlsEvent::HandshakeFailed);
        }
        handleError(result.savedErrno);
        return;
    }
    if (result.completed) {
        // Fire TLS handshake completed hook.
        if (impl_->tlsEventCallback) {
            impl_->tlsEventCallback(shared_from_this(), TlsEvent::HandshakeCompleted);
        }
        notifyConnected(shared_from_this());
    }
}

void TcpConnection::runCloseSequence(const TcpConnectionPtr& connection, bool notifyCloseCallback) {
    impl_->loop->assertInLoopThread();
    setState(kDisconnected);
    impl_->backpressure->onClosed();
    impl_->channel->disableAll();
    impl_->awaiters->resumeAllOnClose();
    notifyDisconnected(connection);
    if (notifyCloseCallback) {
        impl_->callbacks->notifyClose(connection);
    }
}

void TcpConnection::notifyConnected(const TcpConnectionPtr& connection) {
    impl_->callbacks->notifyConnected(connection);
}

void TcpConnection::notifyDisconnected(const TcpConnectionPtr& connection) {
    impl_->callbacks->notifyDisconnected(connection);
}

void TcpConnection::finishPendingWrite(const TcpConnectionPtr& connection) {
    impl_->callbacks->queueWriteComplete(*impl_->loop, connection);
    impl_->awaiters->resumeWriteWaiterIfNeeded();
    if (impl_->state == kDisconnecting) {
        shutdownInLoop();
    }
}

void TcpConnection::maybeQueueHighWaterMark(
    const TcpConnectionPtr& connection,
    std::size_t oldLen,
    std::size_t newLen) {
    impl_->callbacks->queueHighWaterMark(*impl_->loop, connection, oldLen, newLen);
}

bool TcpConnection::canReadImmediately(std::size_t minBytes) const noexcept {
    return impl_->inputBuffer.readableBytes() >= minBytes || impl_->state != kConnected;
}

bool TcpConnection::isReadAwaitReady(std::size_t minBytes) const noexcept {
    return impl_->loop->isInLoopThread() && canReadImmediately(minBytes);
}

bool TcpConnection::isWriteAwaitReady(std::string_view data) const noexcept {
    return impl_->loop->isInLoopThread() && (data.empty() || disconnected());
}

bool TcpConnection::isCloseAwaitReady() const noexcept {
    return impl_->loop->isInLoopThread() && disconnected();
}

std::string TcpConnection::consumeReadableBytes(std::size_t minBytes) {
    impl_->loop->assertInLoopThread();
    if (impl_->inputBuffer.readableBytes() == 0) {
        return {};
    }
    if (impl_->inputBuffer.readableBytes() < minBytes && impl_->state == kConnected) {
        return {};
    }
    return impl_->inputBuffer.retrieveAllAsString();
}

Expected<std::string> TcpConnection::resumeReadAwait(std::size_t minBytes) {
    if (disconnected() && impl_->inputBuffer.readableBytes() == 0) {
        const auto error = impl_->closeReason == Impl::CloseReason::kError
            ? NetError::ConnectionReset
            : NetError::PeerClosed;
        return std::unexpected(error);
    }
    return consumeReadableBytes(minBytes);
}

Expected<void> TcpConnection::resumeWriteAwait() const {
    if (disconnected()) {
        const auto error = impl_->closeReason == Impl::CloseReason::kError
            ? NetError::ConnectionReset
            : NetError::PeerClosed;
        return std::unexpected(error);
    }
    return {};
}

Expected<void> TcpConnection::resumeCloseAwait() const noexcept {
    return {};
}

void TcpConnection::armReadWaiter(std::coroutine_handle<> handle, std::size_t minBytes) {
    auto self = shared_from_this();
    auto action = [self, handle, minBytes] {
        self->impl_->loop->assertInLoopThread();
        self->impl_->awaiters->armReadWaiter(handle, minBytes, self->canReadImmediately(minBytes));
    };

    if (impl_->loop->isInLoopThread()) {
        action();
    } else {
        impl_->loop->queueInLoop(std::move(action));
    }
}

void TcpConnection::armWriteWaiter(std::coroutine_handle<> handle, std::string data) {
    auto self = shared_from_this();
    auto action = [self, handle, data = std::move(data)]() mutable {
        self->impl_->loop->assertInLoopThread();
        self->impl_->awaiters->armWriteWaiter(handle, data.empty() || self->impl_->state == kDisconnected);
        if (data.empty() || self->impl_->state == kDisconnected) {
            return;
        }

        self->sendInLoop(data.data(), data.size());
        if (!self->impl_->channel->isWriting() && self->impl_->outputBuffer.readableBytes() == 0) {
            self->impl_->awaiters->resumeWriteWaiterIfNeeded();
        }
    };

    if (impl_->loop->isInLoopThread()) {
        action();
    } else {
        impl_->loop->queueInLoop(std::move(action));
    }
}

void TcpConnection::armCloseWaiter(std::coroutine_handle<> handle) {
    auto self = shared_from_this();
    auto action = [self, handle] {
        self->impl_->loop->assertInLoopThread();
        self->impl_->awaiters->armCloseWaiter(handle, self->impl_->state == kDisconnected);
    };

    if (impl_->loop->isInLoopThread()) {
        action();
    } else {
        impl_->loop->queueInLoop(std::move(action));
    }
}

void TcpConnection::cancelReadWaiter(
    std::coroutine_handle<> handle,
    const std::shared_ptr<AwaitCancellationState>& cancellationState) {
    auto self = shared_from_this();
    auto action = [self, handle, cancellationState] {
        self->impl_->loop->assertInLoopThread();
        if (self->impl_->awaiters->cancelReadWaiter(handle)) {
            cancellationState->cancelled = true;
            cancellationState->registration.reset();
        }
    };

    if (impl_->loop->isInLoopThread()) {
        impl_->loop->queueInLoop(std::move(action));
    } else {
        impl_->loop->queueInLoop(std::move(action));
    }
}

void TcpConnection::cancelWriteWaiter(
    std::coroutine_handle<> handle,
    const std::shared_ptr<AwaitCancellationState>& cancellationState) {
    auto self = shared_from_this();
    auto action = [self, handle, cancellationState] {
        self->impl_->loop->assertInLoopThread();
        if (self->impl_->awaiters->cancelWriteWaiter(handle)) {
            cancellationState->cancelled = true;
            cancellationState->registration.reset();
        }
    };

    if (impl_->loop->isInLoopThread()) {
        impl_->loop->queueInLoop(std::move(action));
    } else {
        impl_->loop->queueInLoop(std::move(action));
    }
}

void TcpConnection::cancelCloseWaiter(
    std::coroutine_handle<> handle,
    const std::shared_ptr<AwaitCancellationState>& cancellationState) {
    auto self = shared_from_this();
    auto action = [self, handle, cancellationState] {
        self->impl_->loop->assertInLoopThread();
        if (self->impl_->awaiters->cancelCloseWaiter(handle)) {
            cancellationState->cancelled = true;
            cancellationState->registration.reset();
        }
    };

    if (impl_->loop->isInLoopThread()) {
        impl_->loop->queueInLoop(std::move(action));
    } else {
        impl_->loop->queueInLoop(std::move(action));
    }
}

}  // namespace mini::net
