#include "mini/net/Buffer.h"

#include <cassert>
#include <cerrno>
#include <string>
#include <unistd.h>

int main() {
    mini::net::Buffer buffer;
    buffer.append("hello", 5);
    assert(buffer.readableBytes() == 5);
    assert(buffer.retrieveAsString(2) == "he");
    assert(buffer.readableBytes() == 3);

    std::string payload(4096, 'x');
    int fds[2];
    const int rc = ::pipe(fds);
    assert(rc == 0);
    const ssize_t written = ::write(fds[1], payload.data(), payload.size());
    assert(written == static_cast<ssize_t>(payload.size()));

    int savedErrno = 0;
    const ssize_t n = buffer.readFd(fds[0], &savedErrno);
    assert(n == static_cast<ssize_t>(payload.size()));
    assert(buffer.readableBytes() == 3 + payload.size());

    std::string data = buffer.retrieveAllAsString();
    assert(data.size() == 3 + payload.size());
    assert(data.substr(0, 3) == "llo");
    assert(data.substr(3) == payload);

    savedErrno = 0;
    assert(buffer.writeFd(-1, &savedErrno) == -1);
    assert(savedErrno == EBADF);

    savedErrno = 0;
    assert(buffer.readFd(-1, &savedErrno) == -1);
    assert(savedErrno == EBADF);

    ::close(fds[0]);
    ::close(fds[1]);
    return 0;
}
