// Intent: dns_resolver / coroutine_task; a resolve awaitable owns one waiting operation.
#include "mini/coroutine/ResolveAwaitable.h"
#include "mini/coroutine/Task.h"
#include "mini/net/DnsResolver.h"
#include "mini/net/EventLoop.h"

#include <cassert>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

using namespace mini::coroutine;
using mini::net::DnsResolver;
using mini::net::EventLoop;

static_assert(!std::is_copy_constructible_v<ResolveAwaitable>);
static_assert(!std::is_copy_assignable_v<ResolveAwaitable>);
static_assert(std::is_move_constructible_v<ResolveAwaitable>);

namespace {
Task<void> waitFor(ResolveAwaitable& operation, int* completed) {
    auto result = co_await operation;
    assert(result);
    ++*completed;
}

void repeatedWaitCannotReplaceTheFirstContinuation() {
    auto resolver = std::make_shared<DnsResolver>(1);
    EventLoop loop;
    auto operation = asyncResolve(resolver, &loop, "localhost", 80);
    int completed = 0;
    auto first = waitFor(operation, &completed);
    first.start();
    auto second = waitFor(operation, &completed);
    second.start();
    assert(second.done());
    bool rejected = false;
    try { second.result(); } catch (const std::logic_error&) { rejected = true; }
    assert(rejected && !first.done() && completed == 0);
    resolver->resolve("localhost", 81, &loop, [&](auto result) {
        assert(result);
        loop.quit();
    });
    loop.loop();
    first.result();
    assert(completed == 1);
}

Task<void> ownOperation(ResolveAwaitable operation, int* completed) {
    auto result = co_await operation;
    assert(result);
    ++*completed;
}

void movedUnarmedAwaitableKeepsItsOperation() {
    auto resolver = std::make_shared<DnsResolver>(1);
    EventLoop loop;
    int completed = 0;
    Task<void> task;
    {
        auto operation = asyncResolve(resolver, &loop, "localhost", 80);
        task = ownOperation(std::move(operation), &completed);
    }
    task.start();
    resolver->resolve("localhost", 81, &loop, [&](auto result) { assert(result); loop.quit(); });
    loop.loop();
    task.result();
    assert(completed == 1);
}

Task<void> ownedRequest(std::shared_ptr<DnsResolver> resolver, EventLoop* loop,
                        CancellationToken token, int* resumed, int* destroyed) {
    struct Probe { int* count; ~Probe() { ++*count; } } probe{destroyed};
    (void)co_await asyncResolve(resolver, loop, "localhost", 80, token);
    ++*resumed;
}

Task<void> parentRequest(std::shared_ptr<DnsResolver> resolver, EventLoop* loop,
                         CancellationToken token, int* resumed, int* destroyed) {
    co_await ownedRequest(std::move(resolver), loop, token, resumed, destroyed);
    ++*resumed;
}

void abandoningAParentDoesNotCancelTheCallersSource() {
    auto resolver = std::make_shared<DnsResolver>(1);
    EventLoop loop;
    CancellationSource source;
    int resumed = 0, destroyed = 0;
    {
        auto task = parentRequest(resolver, &loop, source.token(), &resumed, &destroyed);
        task.start();
    }
    assert(!source.isCancellationRequested() && destroyed == 1);
    resolver->resolve("localhost", 81, &loop, [&](auto result) { assert(result); loop.quit(); });
    loop.loop();
    assert(resumed == 0 && destroyed == 1);
}

Task<void> inheritedRequest(std::shared_ptr<DnsResolver> resolver, EventLoop* loop, bool* cancelled) {
    auto result = co_await asyncResolve(resolver, loop, "localhost", 80);
    *cancelled = !result && result.error() == mini::net::NetError::Cancelled;
}

void inheritedCancellationIsStillObserved() {
    auto resolver = std::make_shared<DnsResolver>(1);
    EventLoop loop;
    CancellationSource source;
    source.cancel();
    bool cancelled = false;
    auto task = inheritedRequest(resolver, &loop, &cancelled);
    task.setCancellationToken(source.token());
    task.start();
    loop.queueInLoop([&] { loop.quit(); });
    loop.loop();
    task.result();
    assert(cancelled);
}

void invalidArgumentsFailBeforeSuspension() {
    auto resolver = std::make_shared<DnsResolver>(1);
    EventLoop loop;
    bool nullResolver = false, nullLoop = false;
    try { (void)asyncResolve({}, &loop, "localhost", 80); }
    catch (const std::invalid_argument&) { nullResolver = true; }
    try { (void)asyncResolve(resolver, nullptr, "localhost", 80); }
    catch (const std::invalid_argument&) { nullLoop = true; }
    assert(nullResolver && nullLoop);
}
} // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "all";
    if (mode == "repeat" || mode == "all") { repeatedWaitCannotReplaceTheFirstContinuation(); }
    if (mode == "move" || mode == "all") { movedUnarmedAwaitableKeepsItsOperation(); }
    if (mode == "abandon" || mode == "all") { abandoningAParentDoesNotCancelTheCallersSource(); }
    if (mode == "inherited" || mode == "all") { inheritedCancellationIsStillObserved(); }
    if (mode == "invalid" || mode == "all") { invalidArgumentsFailBeforeSuspension(); }
}
