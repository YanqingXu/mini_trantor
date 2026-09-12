// 启动结果不能依赖裸指针瞬时非空；失败必须回传，部分线程池必须回收。
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/EventLoopThreadPool.h"
#include "mini/net/TcpServer.h"

#include <atomic>
#include <barrier>
#include <cassert>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace mini::net;
using namespace std::chrono_literals;

template<class E, class F> void rejects(F&& operation) {
    bool rejected = false;
    try { operation(); } catch (const E&) { rejected = true; }
    assert(rejected);
}

void initQuit() {
    EventLoopThread thread([](EventLoop* loop) { loop->quit(); });
    rejects<std::runtime_error>([&] { thread.startLoop(); });
    thread.stop();
    thread.stop();
}

struct InitError : std::runtime_error { InitError() : std::runtime_error("init failure") {} };

void initFailureRetry() {
    int attempts = 0; // worker writes are published by startLoop's wait/join
    EventLoopThread thread([&](EventLoop* loop) {
        assert(loop->isInLoopThread());
        if (++attempts == 1) { throw InitError{}; }
    });
    rejects<InitError>([&] { thread.startLoop(); });
    auto* loop = thread.startLoop();
    auto* sameLoop = thread.startLoop();
    assert(sameLoop == loop && attempts == 2);
    std::promise<void> ran;
    auto ready = ran.get_future();
    loop->queueInLoop([&] { ran.set_value(); });
    const auto status = ready.wait_for(2s);
    assert(status == std::future_status::ready);
    thread.stop();
}

void selfControl() {
    EventLoopThread* owner = nullptr;
    EventLoopThread thread([&](EventLoop*) {
        rejects<std::logic_error>([&] { owner->startLoop(); });
        owner->stop();
    });
    owner = &thread;
    rejects<std::runtime_error>([&] { thread.startLoop(); });

    EventLoopThread running;
    auto* loop = running.startLoop();
    std::promise<void> stopped;
    auto ready = stopped.get_future();
    loop->queueInLoop([&] {
        auto* sameLoop = running.startLoop();
        assert(sameLoop == loop);
        running.stop();
        stopped.set_value();
    });
    const auto status = ready.wait_for(2s);
    assert(status == std::future_status::ready);
    running.stop();
}

void earlyExit() {
    for (int i = 0; i < 100; ++i) {
        EventLoopThread thread([](EventLoop* loop) {
            loop->queueInLoop([loop] { loop->quit(); });
        });
        auto* loop = thread.startLoop();
        // Even if the queued quit already ran, startLoop's borrow is still alive.
        auto handle = loop->handle();
        assert(!loop->isInLoopThread());
        thread.stop();
        const bool accepted = handle.queue([] {});
        assert(!accepted);
    }
}

void concurrentControls() {
    EventLoopThread thread;
    for (int i = 0; i < 100; ++i) {
        std::barrier gate(3);
        std::jthread starter([&] { gate.arrive_and_wait(); thread.startLoop(); });
        std::jthread stopper([&] { gate.arrive_and_wait(); thread.stop(); });
        gate.arrive_and_wait();
        starter.join();
        stopper.join();
        // No raw pointer is used across concurrent stop. A fresh start must work.
        thread.startLoop();
        thread.stop();
    }
}

void poolRollback() {
    EventLoop base;
    EventLoopThreadPool pool(&base, "rollback");
    pool.setThreadNum(3);
    int initialized = 0;
    LoopHandle first;
    rejects<InitError>([&] {
        pool.start([&](EventLoop* loop) {
            if (++initialized == 2) { throw InitError{}; }
            first = loop->handle();
        });
    });
    const bool accepted = first.queue([] {});
    assert(!accepted && initialized == 2);
    auto* fallback = pool.getNextLoop();
    assert(fallback == &base);
    pool.start();
    auto loops = pool.getAllLoops();
    assert(loops.size() == 3);
    pool.start([](EventLoop*) { assert(false && "duplicate start reran init"); });
    assert(pool.getAllLoops() == loops);
    rejects<std::logic_error>([&] { pool.setThreadNum(1); });
    pool.stop();
    pool.stop();
    pool.setThreadNum(0);
    rejects<InitError>([&] { pool.start([](EventLoop*) { throw InitError{}; }); });
    pool.start([&](EventLoop* loop) {
        assert(loop == &base);
        rejects<std::logic_error>([&] { pool.start(); });
        rejects<std::logic_error>([&] { pool.stop(); });
        rejects<std::logic_error>([&] { pool.getNextLoop(); });
        rejects<std::logic_error>([&] { pool.getAllLoops(); });
        rejects<std::logic_error>([&] { pool.setThreadNum(1); });
    });
    pool.stop();
    bool ran = false;
    base.runAfter(0ms, [&] { ran = true; base.quit(); });
    base.loop();
    assert(ran); // stopping a zero-worker pool must not stop its borrowed base
}

void poolEarlyExitAndAffinity() {
    EventLoop base;
    EventLoopThreadPool pool(&base, "early-exit");
    rejects<std::invalid_argument>([&] { pool.setThreadNum(-1); });
    rejects<std::invalid_argument>([] { EventLoopThreadPool invalid(nullptr, "null"); });
    pool.setThreadNum(1);
    pool.start();
    auto* loop = pool.getNextLoop();
    std::promise<void> quit;
    auto ready = quit.get_future();
    loop->queueInLoop([&] { loop->quit(); quit.set_value(); });
    const auto status = ready.wait_for(2s);
    assert(status == std::future_status::ready);
    rejects<std::runtime_error>([&] { pool.getNextLoop(); });
    std::jthread wrongThread([&] {
        rejects<std::runtime_error>([&] { pool.getAllLoops(); });
        rejects<std::runtime_error>([&] { pool.stop(); });
        rejects<std::logic_error>([&] { pool.setThreadNum(0); });
    });
    wrongThread.join();
    pool.stop();
}

void requestAllBeforeJoin() {
    EventLoop base;
    EventLoopThreadPool pool(&base, "stop-all");
    pool.setThreadNum(2);
    pool.start();
    auto loops = pool.getAllLoops();
    std::promise<void> released;
    auto releaseReady = released.get_future();
    std::promise<void> installed;
    auto installReady = installed.get_future();
    struct Signal {
        std::promise<void>* promise;
        ~Signal() { promise->set_value(); }
    };
    loops[1]->queueInLoop([&] {
        auto signal = std::make_shared<Signal>(&released);
        loops[1]->runAfter(1h, [signal] {});
        installed.set_value();
    });
    loops[0]->queueInLoop([&] {
        // Worker 0 can finish only when worker 1 releases its timer at teardown.
        // Joining worker 0 before requesting worker 1's stop would deadlock.
        const auto status = releaseReady.wait_for(2s);
        assert(status == std::future_status::ready);
    });
    const auto status = installReady.wait_for(2s);
    assert(status == std::future_status::ready);
    pool.stop();
}

void serverStartupRetry() {
    EventLoop base;
    TcpServer server(&base, InetAddress(0), "startup-retry");
    server.setThreadNum(2);
    server.setThreadInitCallback([](EventLoop*) { throw InitError{}; });
    rejects<InitError>([&] { server.start(); });
    std::atomic<int> initialized{0};
    server.setThreadInitCallback([&](EventLoop*) { ++initialized; });
    server.start();
    assert(initialized == 2);
    server.stop();
}

void configureBeforeHandoff() {
    EventLoopThread baseThread;
    auto* base = baseThread.startLoop();
    auto server = std::make_unique<TcpServer>(base, InetAddress(0), "handoff");
    server->setThreadNum(1);
    std::atomic<int> initialized{0};
    server->setThreadInitCallback([&](EventLoop*) { ++initialized; });
    std::promise<void> done;
    auto ready = done.get_future();
    base->queueInLoop([&] {
        server->start();
        server->stop();
        server.reset();
        done.set_value();
    });
    const auto status = ready.wait_for(2s);
    assert(status == std::future_status::ready && initialized == 1);
    baseThread.stop();
}

int main(int argc, char** argv) {
    const std::string_view test = argc > 1 ? argv[1] : "all";
    if (test == "quit" || test == "all") { initQuit(); }
    if (test == "throw" || test == "all") { initFailureRetry(); }
    if (test == "all") {
        selfControl();
        earlyExit();
        concurrentControls();
        poolRollback();
        poolEarlyExitAndAffinity();
        requestAllBeforeJoin();
        serverStartupRetry();
        configureBeforeHandoff();
    }
}
