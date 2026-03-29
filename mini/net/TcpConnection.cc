#include "mini/net/TcpConnection.h"

#include "mini/net/EventLoop.h"
#include "mini/net/SocketsOps.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <unistd.h>

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
      reading_(true) {
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

void TcpConnection::setTcpNoDelay(bool on) {
    socket_->setTcpNoDelay(on);
}

void TcpConnection::setConnectionCallback(ConnectionCallback cb) {
    connectionCallback_ = std::move(cb);
}

void TcpConnection::setMessageCallback(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

void TcpConnection::setWriteCompleteCallback(WriteCompleteCallback cb) {
    writeCompleteCallback_ = std::move(cb);
}

void TcpConnection::setCloseCallback(CloseCallback cb) {
    closeCallback_ = std::move(cb);
}

void TcpConnection::connectEstablished() {
    loop_->assertInLoopThread();
    setState(kConnected);
    channel_->tie(shared_from_this());
    channel_->enableReading();
    if (connectionCallback_) {
        connectionCallback_(shared_from_this());
    }
}

void TcpConnection::connectDestroyed() {
    loop_->assertInLoopThread();
    if (state_ == kConnected) {
        setState(kDisconnected);
        channel_->disableAll();
        if (connectionCallback_) {
            connectionCallback_(shared_from_this());
        }
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

std::string TcpConnection::ReadAwaitable::await_resume() {
    if (!connection_) {
        return {};
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

void TcpConnection::WriteAwaitable::await_resume() const {
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
    const bool hasReadWaiter = readWaiter_.active;

    int savedErrno = 0;
    const ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
    if (n > 0) {
        resumeReadWaiterIfNeeded();
        if (messageCallback_ && !hasReadWaiter) {
            messageCallback_(shared_from_this(), &inputBuffer_);
        }
    } else if (n == 0) {
        handleClose();
    } else {
        handleError(savedErrno);
    }
}

void TcpConnection::handleWrite() {
    loop_->assertInLoopThread();
    if (!channel_->isWriting()) {
        return;
    }

    int savedErrno = 0;
    const ssize_t n = outputBuffer_.writeFd(channel_->fd(), &savedErrno);
    if (n > 0) {
        outputBuffer_.retrieve(static_cast<std::size_t>(n));
        if (outputBuffer_.readableBytes() == 0) {
            channel_->disableWriting();
            if (writeCompleteCallback_) {
                auto self = shared_from_this();
                loop_->queueInLoop([self, cb = writeCompleteCallback_] { cb(self); });
            }
            resumeWriteWaiterIfNeeded();
            if (state_ == kDisconnecting) {
                shutdownInLoop();
            }
        }
        return;
    }

    if (savedErrno != EWOULDBLOCK && savedErrno != EAGAIN) {
        handleError(savedErrno);
    }
}

void TcpConnection::handleClose() {
    loop_->assertInLoopThread();
    setState(kDisconnected);
    channel_->disableAll();
    auto guardThis = shared_from_this();
    resumeAllWaitersOnClose();
    if (connectionCallback_) {
        connectionCallback_(guardThis);
    }
    if (closeCallback_) {
        closeCallback_(guardThis);
    }
}

void TcpConnection::handleError(int savedErrno) {
    loop_->assertInLoopThread();
    const int err = savedErrno != 0 ? savedErrno : sockets::getSocketError(channel_->fd());
    std::fprintf(stderr, "TcpConnection error on %s: %d (%s)\n", name_.c_str(), err, std::strerror(err));
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
    ssize_t nwrote = 0;
    bool faultError = false;

    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
        nwrote = ::write(channel_->fd(), data, len);
        if (nwrote >= 0) {
            remaining = len - static_cast<std::size_t>(nwrote);
            if (remaining == 0) {
                if (writeCompleteCallback_) {
                    auto self = shared_from_this();
                    loop_->queueInLoop([self, cb = writeCompleteCallback_] { cb(self); });
                }
                resumeWriteWaiterIfNeeded();
                return;
            }
        } else {
            nwrote = 0;
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                faultError = true;
            }
        }
    }

    if (faultError) {
        handleError(errno);
        return;
    }

    if (!faultError && remaining > 0) {
        outputBuffer_.append(data + nwrote, remaining);
        if (!channel_->isWriting()) {
            channel_->enableWriting();
        }
    }
}

void TcpConnection::shutdownInLoop() {
    loop_->assertInLoopThread();
    if (!channel_->isWriting()) {
        socket_->shutdownWrite();
    }
}

void TcpConnection::setState(StateE state) noexcept {
    state_ = state;
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
        if (self->readWaiter_.active) {
            throw std::logic_error("only one read waiter is allowed per TcpConnection");
        }
        if (self->canReadImmediately(minBytes)) {
            self->queueResume(handle);
            return;
        }
        self->readWaiter_ = {.handle = handle, .minBytes = minBytes, .active = true};
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
        if (self->writeWaiter_.active) {
            throw std::logic_error("only one write waiter is allowed per TcpConnection");
        }
        if (data.empty() || self->state_ == kDisconnected) {
            self->queueResume(handle);
            return;
        }
        self->writeWaiter_ = {.handle = handle, .active = true};
        self->sendInLoop(data.data(), data.size());
        if (!self->channel_->isWriting() && self->outputBuffer_.readableBytes() == 0) {
            self->resumeWriteWaiterIfNeeded();
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
        if (self->closeWaiter_.active) {
            throw std::logic_error("only one close waiter is allowed per TcpConnection");
        }
        if (self->state_ == kDisconnected) {
            self->queueResume(handle);
            return;
        }
        self->closeWaiter_ = {.handle = handle, .active = true};
    };

    if (loop_->isInLoopThread()) {
        action();
    } else {
        loop_->queueInLoop(std::move(action));
    }
}

void TcpConnection::queueResume(std::coroutine_handle<> handle) {
    if (!handle) {
        return;
    }
    loop_->queueInLoop([handle] { handle.resume(); });
}

void TcpConnection::resumeReadWaiterIfNeeded() {
    if (!readWaiter_.active) {
        return;
    }
    if (inputBuffer_.readableBytes() < readWaiter_.minBytes && state_ == kConnected) {
        return;
    }
    auto handle = readWaiter_.handle;
    readWaiter_ = {};
    queueResume(handle);
}

void TcpConnection::resumeWriteWaiterIfNeeded() {
    if (!writeWaiter_.active) {
        return;
    }
    auto handle = writeWaiter_.handle;
    writeWaiter_ = {};
    queueResume(handle);
}

void TcpConnection::resumeAllWaitersOnClose() {
    if (readWaiter_.active) {
        auto handle = readWaiter_.handle;
        readWaiter_ = {};
        queueResume(handle);
    }
    if (writeWaiter_.active) {
        auto handle = writeWaiter_.handle;
        writeWaiter_ = {};
        queueResume(handle);
    }
    if (closeWaiter_.active) {
        auto handle = closeWaiter_.handle;
        closeWaiter_ = {};
        queueResume(handle);
    }
}

}  // namespace mini::net
