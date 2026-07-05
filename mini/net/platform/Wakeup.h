#pragma once

// EventLoop wakeup 的平台抽象。
// Linux 使用 eventfd；Windows 使用 WinSock loopback socket pair。

#include "mini/net/SocketTypes.h"

namespace mini::net::platform {

struct WakeupFdPair {
    SocketFd readFd{kInvalidSocket};
    SocketFd writeFd{kInvalidSocket};
};

WakeupFdPair createWakeupFds();
void closeWakeupFds(WakeupFdPair fds);
ssize_t writeWakeup(SocketFd fd);
bool drainWakeup(SocketFd fd);

}  // namespace mini::net::platform
