// 取消回调及捕获释放均允许重入；观察者异常不能截断通知或挂起组合器父任务。
#include "mini/coroutine/CancellationToken.h"
#include "mini/coroutine/SleepAwaitable.h"
#include "mini/coroutine/WhenAny.h"
#include "mini/net/EventLoop.h"

#include <cassert>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace mini::coroutine;
using namespace std::chrono_literals;
using mini::net::EventLoop;

struct ObserverError : std::runtime_error { ObserverError() : std::runtime_error("observer") {} };
struct WinnerError : std::runtime_error { WinnerError() : std::runtime_error("winner") {} };

void releaseMayRegister() {
    CancellationSource source;
    int destroyed = 0, called = 0;
    struct Capture {
        CancellationToken token;
        int* destroyed;
        ~Capture() {
            auto registration = token.registerCallback([] {});
            ++*destroyed;
        }
    };
    auto registration = source.token().registerCallback(
        [capture = std::make_shared<Capture>(source.token(), &destroyed), &called] { ++called; });
    registration.reset();
    assert(destroyed == 1 && called == 0);
    source.cancel();
    assert(called == 0);
}

void cancellationFailureStillNotifiesAll() {
    CancellationSource source;
    int called = 0;
    std::vector<CancellationRegistration> registrations;
    for (int i = 0; i < 3; ++i) {
        registrations.push_back(source.token().registerCallback([&] {
            ++called;
            if (called == 1) { throw ObserverError{}; }
        }));
    }
    bool caught = false;
    try { source.cancel(); } catch (const ObserverError&) { caught = true; }
    assert(caught && called == 3 && source.isCancellationRequested());
    source.cancel();
    assert(called == 3);
    caught = false;
    try {
        auto registration = source.token().registerCallback([] { throw ObserverError{}; });
    } catch (const ObserverError&) { caught = true; }
    assert(caught);

    auto moved = std::move(source);
    assert(!source.token() && !source.isCancellationRequested());
    source.cancel();
    assert(moved.isCancellationRequested());
}

struct CurrentToken {
    CancellationToken token;
    bool await_ready() const noexcept { return false; }
    template<class Promise> bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        token = handle.promise().cancellationToken();
        return false;
    }
    CancellationToken await_resume() { return std::move(token); }
};

template<class T> Task<T> loser(EventLoop* loop, int* notified, int* destroyed) {
    struct Probe { int* count; ~Probe() { ++*count; } } probe{destroyed};
    auto token = co_await CurrentToken{};
    auto registration = token.registerCallback([notified] { ++*notified; throw ObserverError{}; });
    const auto result = co_await asyncSleep(loop, 10s);
    assert(!result && result.error() == mini::net::NetError::Cancelled);
    if constexpr (std::is_void_v<T>) { co_return; } else { co_return 0; }
}

template<class T> Task<T> winner(bool fails) {
    if (fails) { throw WinnerError{}; }
    if constexpr (std::is_void_v<T>) { co_return; } else { co_return 7; }
}

template<class T> void whenAnyCancellationFailure(bool winnerFails) {
    EventLoop loop;
    int notified = 0, destroyed = 0;
    auto parent = whenAny(loser<T>(&loop, &notified, &destroyed),
                          loser<T>(&loop, &notified, &destroyed), winner<T>(winnerFails));
    parent.start();
    assert(parent.done());
    bool observed = false;
    try { (void)parent.result(); }
    catch (const WinnerError&) { observed = winnerFails; }
    catch (const ObserverError&) { observed = !winnerFails; }
    assert(observed && notified == 2);
    // Cancellation only queued sleep completions; reclaim children on their owner.
    loop.queueInLoop([&] { loop.quit(); });
    loop.loop();
    assert(destroyed == 2);
}

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "all";
    if (mode == "release" || mode == "all") { releaseMayRegister(); }
    if (mode == "fanout" || mode == "all") { cancellationFailureStillNotifiesAll(); }
    if (mode == "any" || mode == "all") {
        whenAnyCancellationFailure<int>(false);
        whenAnyCancellationFailure<void>(false);
        whenAnyCancellationFailure<int>(true);
        whenAnyCancellationFailure<void>(true);
    }
}
