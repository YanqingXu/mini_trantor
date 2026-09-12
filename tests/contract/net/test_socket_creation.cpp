// TCP socket 创建合同：可恢复失败保留错误，成功返回可由调用方释放的非阻塞句柄。
// 对应 intents/modules/platform_runtime.intent.md，不依赖 EventLoop 或外部服务。

#include "mini/net/Socket.h"
#include "mini/net/SocketsOps.h"

#include <cassert>
#include <cerrno>
#include <limits>

#ifndef _WIN32
#include <fcntl.h>
#endif

using namespace mini::net;

namespace {

void testNonblockingTcpSocket() {
    const SocketFd descriptor = sockets::createNonblocking(AF_INET);
    assert(sockets::isValid(descriptor));
    Socket owner(descriptor);

    int type = 0;
    socklen_t typeLength = static_cast<socklen_t>(sizeof(type));
#ifdef _WIN32
    const int typeResult = ::getsockopt(descriptor, SOL_SOCKET, SO_TYPE,
                                      reinterpret_cast<char*>(&type), &typeLength);
#else
    const int typeResult = ::getsockopt(descriptor, SOL_SOCKET, SO_TYPE,
                                      &type, &typeLength);
    const int statusFlags = ::fcntl(descriptor, F_GETFL);
    const int descriptorFlags = ::fcntl(descriptor, F_GETFD);
    assert(statusFlags >= 0 && (statusFlags & O_NONBLOCK) != 0);
    assert(descriptorFlags >= 0 && (descriptorFlags & FD_CLOEXEC) != 0);
#endif
    assert(typeResult == 0 && type == SOCK_STREAM);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    const int bindResult = ::bind(descriptor, reinterpret_cast<const sockaddr*>(&address),
                                  static_cast<socklen_t>(sizeof(address)));
    assert(bindResult == 0);
    const int listenResult = ::listen(descriptor, 1);
    assert(listenResult == 0);

    // A listener with no connecting peer must return immediately on both platforms.
    sockaddr_storage peer{};
    sockets::setLastError(0);
    const SocketFd accepted = sockets::accept(descriptor, &peer);
    const int acceptError = sockets::lastError();
    assert(accepted == kInvalidSocket);
    assert(sockets::isWouldBlock(acceptError));
}

void testInvalidFamilyPreservesError() {
    constexpr auto invalidFamily = (std::numeric_limits<sa_family_t>::max)();
    sockets::setLastError(0);
    const SocketFd descriptor = sockets::createNonblocking(invalidFamily);
    const int creationError = sockets::lastError();
    assert(descriptor == kInvalidSocket);
#ifdef _WIN32
    assert(creationError == WSAEAFNOSUPPORT);
#else
    assert(creationError == EAFNOSUPPORT);
#endif
}

}  // namespace

int main() {
    testNonblockingTcpSocket();
    testInvalidFamilyPreservesError();
}
