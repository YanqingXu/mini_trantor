#pragma once

// Buffer 是连接读写路径上的字节容器，负责维护可读/可写/可预留区域。
// 它不解析协议，也不做线程同步，默认由所属连接的 loop 线程独占修改。

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mini::net {

class Buffer {
public:
    static constexpr std::size_t kCheapPrepend = 8;
    static constexpr std::size_t kInitialSize = 1024;

    Buffer();

    std::size_t readableBytes() const noexcept;
    std::size_t writableBytes() const noexcept;
    std::size_t prependableBytes() const noexcept;

    const char* peek() const noexcept;
    char* beginWrite() noexcept;
    const char* beginWrite() const noexcept;

    void retrieve(std::size_t len);
    void retrieveUntil(const char* end);
    void retrieveAll();
    std::string retrieveAllAsString();
    std::string retrieveAsString(std::size_t len);

    void append(const char* data, std::size_t len);
    void append(std::string_view data);
    void append(const std::string& data);

    void ensureWritableBytes(std::size_t len);
    void hasWritten(std::size_t len);

    ssize_t readFd(int fd, int* savedErrno);
    ssize_t writeFd(int fd, int* savedErrno);

private:
    char* begin() noexcept;
    const char* begin() const noexcept;
    void makeSpace(std::size_t len);

    std::vector<char> buffer_;
    std::size_t readerIndex_;
    std::size_t writerIndex_;
};

}  // namespace mini::net
