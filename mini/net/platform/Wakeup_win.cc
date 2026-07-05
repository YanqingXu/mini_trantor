#include "mini/net/platform/Wakeup.h"

#include "mini/net/SocketsOps.h"

namespace mini::net::platform {

WakeupFdPair createWakeupFds() {
    SocketFd fds[2]{kInvalidSocket, kInvalidSocket};
    sockets::createSocketPairOrDie(fds);
    return {.readFd = fds[0], .writeFd = fds[1]};
}

void closeWakeupFds(WakeupFdPair fds) {
    sockets::close(fds.readFd);
    if (fds.writeFd != fds.readFd) {
        sockets::close(fds.writeFd);
    }
}

ssize_t writeWakeup(SocketFd fd) {
    const unsigned char one = 1;
    return sockets::write(fd, &one, sizeof(one));
}

bool drainWakeup(SocketFd fd) {
    bool drained = false;
    char buffer[64];
    while (true) {
        const ssize_t n = sockets::read(fd, buffer, sizeof(buffer));
        if (n > 0) {
            drained = true;
            continue;
        }
        if (n < 0 && sockets::isWouldBlock(sockets::lastError())) {
            return drained;
        }
        return drained || n == 0;
    }
}

}  // namespace mini::net::platform
