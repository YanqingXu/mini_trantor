#include "mini/net/Buffer.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/uio.h>
#include <unistd.h>

namespace mini::net {

Buffer::Buffer()
    : buffer_(kCheapPrepend + kInitialSize), readerIndex_(kCheapPrepend), writerIndex_(kCheapPrepend) {
}

std::size_t Buffer::readableBytes() const noexcept {
    return writerIndex_ - readerIndex_;
}

std::size_t Buffer::writableBytes() const noexcept {
    return buffer_.size() - writerIndex_;
}

std::size_t Buffer::prependableBytes() const noexcept {
    return readerIndex_;
}

const char* Buffer::peek() const noexcept {
    return begin() + readerIndex_;
}

char* Buffer::beginWrite() noexcept {
    return begin() + writerIndex_;
}

const char* Buffer::beginWrite() const noexcept {
    return begin() + writerIndex_;
}

void Buffer::retrieve(std::size_t len) {
    if (len < readableBytes()) {
        readerIndex_ += len;
    } else {
        retrieveAll();
    }
}

void Buffer::retrieveUntil(const char* end) {
    retrieve(static_cast<std::size_t>(end - peek()));
}

void Buffer::retrieveAll() {
    readerIndex_ = kCheapPrepend;
    writerIndex_ = kCheapPrepend;
}

std::string Buffer::retrieveAllAsString() {
    return retrieveAsString(readableBytes());
}

std::string Buffer::retrieveAsString(std::size_t len) {
    len = std::min(len, readableBytes());
    std::string result(peek(), len);
    retrieve(len);
    return result;
}

void Buffer::append(const char* data, std::size_t len) {
    ensureWritableBytes(len);
    std::memcpy(beginWrite(), data, len);
    hasWritten(len);
}

void Buffer::append(std::string_view data) {
    append(data.data(), data.size());
}

void Buffer::append(const std::string& data) {
    append(data.data(), data.size());
}

void Buffer::ensureWritableBytes(std::size_t len) {
    if (writableBytes() < len) {
        makeSpace(len);
    }
}

void Buffer::hasWritten(std::size_t len) {
    writerIndex_ += len;
}

ssize_t Buffer::readFd(int fd, int* savedErrno) {
    char extraBuffer[65536];
    struct iovec vec[2];
    const std::size_t writable = writableBytes();

    vec[0].iov_base = beginWrite();
    vec[0].iov_len = writable;
    vec[1].iov_base = extraBuffer;
    vec[1].iov_len = sizeof(extraBuffer);

    const int iovcnt = writable < sizeof(extraBuffer) ? 2 : 1;
    const ssize_t n = ::readv(fd, vec, iovcnt);
    if (n < 0) {
        *savedErrno = errno;
        return n;
    }
    if (static_cast<std::size_t>(n) <= writable) {
        writerIndex_ += static_cast<std::size_t>(n);
    } else {
        writerIndex_ = buffer_.size();
        append(extraBuffer, static_cast<std::size_t>(n) - writable);
    }
    return n;
}

ssize_t Buffer::writeFd(int fd, int* savedErrno) {
    const ssize_t n = ::write(fd, peek(), readableBytes());
    if (n < 0) {
        *savedErrno = errno;
    }
    return n;
}

char* Buffer::begin() noexcept {
    return buffer_.data();
}

const char* Buffer::begin() const noexcept {
    return buffer_.data();
}

void Buffer::makeSpace(std::size_t len) {
    if (writableBytes() + prependableBytes() - kCheapPrepend < len) {
        buffer_.resize(writerIndex_ + len);
        return;
    }

    const std::size_t readable = readableBytes();
    std::copy(begin() + readerIndex_, begin() + writerIndex_, begin() + kCheapPrepend);
    readerIndex_ = kCheapPrepend;
    writerIndex_ = readerIndex_ + readable;
}

}  // namespace mini::net
