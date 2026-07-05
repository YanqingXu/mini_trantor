#pragma once

// SocketTypes 集中定义网络句柄与平台 socket 头文件。
// 上层 reactor 代码只使用 SocketFd，不直接依赖 POSIX fd 或 WinSock SOCKET。

#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif

#include <BaseTsd.h>
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

using socklen_t = int;
using ssize_t = SSIZE_T;

namespace mini::net {

using SocketFd = SOCKET;
using sa_family_t = ADDRESS_FAMILY;

inline constexpr SocketFd kInvalidSocket = INVALID_SOCKET;

}  // namespace mini::net

#else

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

namespace mini::net {

using SocketFd = int;
using ::sa_family_t;

inline constexpr SocketFd kInvalidSocket = -1;

}  // namespace mini::net

#endif
