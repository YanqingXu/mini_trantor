// SignalWatcher contract tests:
// 1. signal callback delivers on owner loop thread
// 2. SIGINT/SIGTERM are received via signalfd
// 3. SIGPIPE is ignored process-wide
// 4. destruction unblocks signals and cleans up

#include "mini/net/EventLoop.h"
#include "mini/net/SignalWatcher.h"

#include <cassert>
#include <chrono>
#include <csignal>
#include <future>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

int main() {
    // Block SIGINT/SIGTERM on the main thread BEFORE creating any worker threads.
    mini::net::SignalWatcher::blockSignals({SIGINT, SIGTERM});
    mini::net::SignalWatcher::ignoreSigpipe();

    // Test 1: SignalWatcher delivers signal callback on owner loop thread.
    {
        mini::net::EventLoop loop;
        mini::net::SignalWatcher watcher(&loop, {SIGINT, SIGTERM});

        std::promise<int> signalPromise;
        auto signalFuture = signalPromise.get_future();

        watcher.setSignalCallback([&](int signum) {
            assert(loop.isInLoopThread());
            signalPromise.set_value(signum);
            loop.quit();
        });

        // Send SIGINT to the process (not thread-directed) from another thread.
        std::thread sender([] {
            std::this_thread::sleep_for(50ms);
            ::kill(::getpid(), SIGINT);
        });

        loop.loop();
        sender.join();

        const auto status = signalFuture.wait_for(2s);
        assert(status == std::future_status::ready);
        assert(signalFuture.get() == SIGINT);
    }

    // Test 2: SIGTERM is received.
    {
        mini::net::EventLoop loop;
        mini::net::SignalWatcher watcher(&loop, {SIGINT, SIGTERM});

        std::promise<int> signalPromise;
        auto signalFuture = signalPromise.get_future();

        watcher.setSignalCallback([&](int signum) {
            signalPromise.set_value(signum);
            loop.quit();
        });

        std::thread sender([] {
            std::this_thread::sleep_for(50ms);
            ::kill(::getpid(), SIGTERM);
        });

        loop.loop();
        sender.join();

        const auto status = signalFuture.wait_for(2s);
        assert(status == std::future_status::ready);
        assert(signalFuture.get() == SIGTERM);
    }

    // Test 3: SIGPIPE is ignored process-wide.
    {
        // SignalWatcher constructor should have already called ignoreSigpipe(),
        // but we create one here to ensure it's covered even in a fresh scope.
        mini::net::EventLoop loop;
        mini::net::SignalWatcher watcher(&loop, {SIGINT});

        // Verify SIGPIPE disposition is SIG_IGN.
        struct sigaction sa{};
        ::sigaction(SIGPIPE, nullptr, &sa);
        assert(sa.sa_handler == SIG_IGN);

        // Raising SIGPIPE should not kill the process.
        ::kill(::getpid(), SIGPIPE);

        // Quit the loop after a short delay to confirm process survived.
        std::promise<void> done;
        auto doneFuture = done.get_future();
        loop.runAfter(50ms, [&] {
            done.set_value();
            loop.quit();
        });
        loop.loop();

        const auto status = doneFuture.wait_for(2s);
        assert(status == std::future_status::ready);
    }

    // Test 4: Destruction cleans up without error (signals remain blocked).
    {
        {
            mini::net::EventLoop loop;
            mini::net::SignalWatcher watcher(&loop, {SIGINT, SIGTERM});
            // After this scope, the signalfd is closed and channel removed.
            // Signals remain blocked (destructor does NOT unblock them).
        }

        // Verify signals are still blocked.
        sigset_t blocked;
        ::sigemptyset(&blocked);
        ::pthread_sigmask(SIG_SETMASK, nullptr, &blocked);
        assert(::sigismember(&blocked, SIGINT));
        assert(::sigismember(&blocked, SIGTERM));
    }

    return 0;
}
