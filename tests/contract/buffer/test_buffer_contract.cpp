#include "mini/net/Buffer.h"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <string>
#include <unistd.h>

int main() {
    // Contract 1: readFd reads data from a real fd and grows buffer correctly
    {
        int fds[2];
        assert(::pipe(fds) == 0);

        const std::string payload = "hello from pipe";
        const ssize_t written = ::write(fds[1], payload.data(), payload.size());
        assert(written == static_cast<ssize_t>(payload.size()));
        ::close(fds[1]);

        mini::net::Buffer buf;
        int savedErrno = 0;
        const ssize_t n = buf.readFd(fds[0], &savedErrno);
        assert(n == static_cast<ssize_t>(payload.size()));
        assert(buf.readableBytes() == payload.size());
        assert(std::string(buf.peek(), buf.readableBytes()) == payload);

        ::close(fds[0]);
    }

    // Contract 2: readFd into extra buffer path (data exceeds initial writable space)
    {
        int fds[2];
        assert(::pipe(fds) == 0);

        // fill a large payload that exceeds initial buffer writable space
        const std::size_t largeSize = mini::net::Buffer::kInitialSize + 512;
        std::string payload(largeSize, 'X');
        // pipe buffer is typically 64KB on Linux, so this should fit
        const ssize_t written = ::write(fds[1], payload.data(), payload.size());
        assert(written == static_cast<ssize_t>(payload.size()));
        ::close(fds[1]);

        mini::net::Buffer buf;
        // consume some writable space first to force the scatter-read extra path
        const std::string filler(mini::net::Buffer::kInitialSize - 10, 'Y');
        buf.append(filler);
        buf.retrieveAll();

        int savedErrno = 0;
        const ssize_t n = buf.readFd(fds[0], &savedErrno);
        assert(n == static_cast<ssize_t>(payload.size()));
        assert(buf.readableBytes() == payload.size());
        assert(std::string(buf.peek(), buf.readableBytes()) == payload);

        ::close(fds[0]);
    }

    // Contract 3: readFd reports explicit errno on failure (bad fd)
    {
        mini::net::Buffer buf;
        int savedErrno = 0;
        const ssize_t n = buf.readFd(-1, &savedErrno);
        assert(n < 0);
        assert(savedErrno == EBADF);
    }

    // Contract 4: writeFd writes readable bytes to a real fd
    {
        int fds[2];
        assert(::pipe(fds) == 0);

        mini::net::Buffer buf;
        const std::string payload = "write through buffer";
        buf.append(payload);

        int savedErrno = 0;
        const ssize_t n = buf.writeFd(fds[1], &savedErrno);
        assert(n == static_cast<ssize_t>(payload.size()));
        ::close(fds[1]);

        char readBuf[64] = {};
        const ssize_t r = ::read(fds[0], readBuf, sizeof(readBuf));
        assert(r == static_cast<ssize_t>(payload.size()));
        assert(std::string(readBuf, static_cast<std::size_t>(r)) == payload);

        ::close(fds[0]);
    }

    // Contract 5: writeFd reports explicit errno on failure (bad fd)
    {
        mini::net::Buffer buf;
        buf.append(std::string_view("data"));
        int savedErrno = 0;
        const ssize_t n = buf.writeFd(-1, &savedErrno);
        assert(n < 0);
        assert(savedErrno == EBADF);
    }

    // Contract 6: readFd on EOF returns 0
    {
        int fds[2];
        assert(::pipe(fds) == 0);
        ::close(fds[1]);  // close write end immediately

        mini::net::Buffer buf;
        int savedErrno = 0;
        const ssize_t n = buf.readFd(fds[0], &savedErrno);
        assert(n == 0);
        assert(buf.readableBytes() == 0);

        ::close(fds[0]);
    }

    return 0;
}
