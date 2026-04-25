// Acceptor::stop() unit test:
// Verifies that stop() disables listening without destroying the Acceptor,
// and that listening() returns false after stop().

#include "mini/net/Acceptor.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"

#include <cassert>

using namespace std::chrono_literals;

int main() {
    // Test: stop() transitions listening from true to false.
    {
        mini::net::EventLoop loop;
        mini::net::InetAddress listenAddr(0, true);
        mini::net::Acceptor acceptor(&loop, listenAddr, true);

        assert(!acceptor.listening());

        loop.runInLoop([&] {
            acceptor.listen();
            assert(acceptor.listening());

            acceptor.stop();
            assert(!acceptor.listening());

            loop.quit();
        });

        loop.loop();
    }

    // Test: stop() is idempotent when not listening.
    {
        mini::net::EventLoop loop;
        mini::net::InetAddress listenAddr(0, true);
        mini::net::Acceptor acceptor(&loop, listenAddr, true);

        loop.runInLoop([&] {
            assert(!acceptor.listening());
            acceptor.stop();  // Should be a no-op.
            assert(!acceptor.listening());

            loop.quit();
        });

        loop.loop();
    }

    // Test: stop() is idempotent when already stopped.
    {
        mini::net::EventLoop loop;
        mini::net::InetAddress listenAddr(0, true);
        mini::net::Acceptor acceptor(&loop, listenAddr, true);

        loop.runInLoop([&] {
            acceptor.listen();
            acceptor.stop();
            assert(!acceptor.listening());

            acceptor.stop();  // Second stop is a no-op.
            assert(!acceptor.listening());

            loop.quit();
        });

        loop.loop();
    }

    return 0;
}
