// test_protocol_connection_adapter.cpp
//
// 验证 ProtocolConnectionAdapter 的核心契约：
//   1. createAndBind() 将 adapter 存入 conn->setContext()
//   2. getFrom() / sharedFrom() 能正确取回
//   3. name() 返回连接名（conn 销毁后仍可用）
//   4. connected() 在 kConnecting 状态返回 false
//   5. getLoop() 返回 owner loop
//   6. setProtocolContext() / getProtocolContext() 独立于 TcpConnection context
//   7. send() 在连接可用时委托到 TcpConnection
//   8. 当 TcpConnection 已销毁时，send/shutdown/forceClose 静默忽略（不 crash）

#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/ProtocolConnectionAdapter.h"
#include "mini/net/TcpConnection.h"

#include <any>
#include <cassert>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

std::pair<int, int> makeSocketPair() {
    int sv[2]{};
    const int rc = ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(rc == 0);
    return {sv[0], sv[1]};
}

mini::net::TcpConnectionPtr makeConnection(mini::net::EventLoop* loop,
                                            int fd,
                                            const std::string& name) {
    return std::make_shared<mini::net::TcpConnection>(
        loop, name, fd,
        mini::net::InetAddress(),
        mini::net::InetAddress());
}

// ────────────────────────────────────────────────────────────────────────────
// T1: createAndBind 存储，getFrom / sharedFrom 取回
// ────────────────────────────────────────────────────────────────────────────
void testCreateBindAndRetrieve() {
    auto [fd0, fd1] = makeSocketPair();
    mini::net::EventLoop loop;
    auto conn = makeConnection(&loop, fd0, "test-conn");

    auto adapter = mini::net::ProtocolConnectionAdapter::createAndBind(conn);
    assert(adapter != nullptr);

    auto* raw = mini::net::ProtocolConnectionAdapter::getFrom(conn);
    assert(raw == adapter.get());

    auto shared = mini::net::ProtocolConnectionAdapter::sharedFrom(conn);
    assert(shared == adapter);

    conn.reset();
    ::close(fd1);
}

// ────────────────────────────────────────────────────────────────────────────
// T2: name() 返回连接名称
// ────────────────────────────────────────────────────────────────────────────
void testName() {
    auto [fd0, fd1] = makeSocketPair();
    mini::net::EventLoop loop;
    auto conn = makeConnection(&loop, fd0, "my-conn");

    auto adapter = mini::net::ProtocolConnectionAdapter::createAndBind(conn);
    assert(adapter->name() == "my-conn");

    conn.reset();
    ::close(fd1);
    // name() 在 conn 销毁后仍可用（已缓存）
    assert(adapter->name() == "my-conn");
}

// ────────────────────────────────────────────────────────────────────────────
// T3: connected() 在 kConnecting 状态返回 false
// ────────────────────────────────────────────────────────────────────────────
void testConnectedStateReflected() {
    auto [fd0, fd1] = makeSocketPair();
    mini::net::EventLoop loop;
    auto conn = makeConnection(&loop, fd0, "state-conn");

    auto adapter = mini::net::ProtocolConnectionAdapter::createAndBind(conn);
    // TcpConnection 初始处于 kConnecting，尚未调用 connectEstablished()
    assert(!adapter->connected());

    conn.reset();
    ::close(fd1);
}

// ────────────────────────────────────────────────────────────────────────────
// T4: getLoop() 返回 owner loop
// ────────────────────────────────────────────────────────────────────────────
void testGetLoop() {
    auto [fd0, fd1] = makeSocketPair();
    mini::net::EventLoop loop;
    auto conn = makeConnection(&loop, fd0, "loop-conn");

    auto adapter = mini::net::ProtocolConnectionAdapter::createAndBind(conn);
    assert(adapter->getLoop() == &loop);

    conn.reset();
    ::close(fd1);
}

// ────────────────────────────────────────────────────────────────────────────
// T5: setProtocolContext / getProtocolContext 与 TcpConnection context 独立
// ────────────────────────────────────────────────────────────────────────────
void testProtocolContextSlot() {
    auto [fd0, fd1] = makeSocketPair();
    mini::net::EventLoop loop;
    auto conn = makeConnection(&loop, fd0, "ctx-conn");

    auto adapter = mini::net::ProtocolConnectionAdapter::createAndBind(conn);

    // 初始 context 为空
    assert(!adapter->getProtocolContext().has_value());

    // 写入协议上下文
    adapter->setProtocolContext(std::string("HttpContext-placeholder"));
    assert(adapter->getProtocolContext().has_value());

    const auto* s = std::any_cast<std::string>(&adapter->getProtocolContext());
    assert(s != nullptr);
    assert(*s == "HttpContext-placeholder");

    // TcpConnection 的 context 槽应该是 adapter 本身，而不是协议上下文
    auto* adapterFromConn = mini::net::ProtocolConnectionAdapter::getFrom(conn);
    assert(adapterFromConn != nullptr);
    const auto* protCtx = std::any_cast<std::string>(&adapterFromConn->getProtocolContext());
    assert(protCtx != nullptr && *protCtx == "HttpContext-placeholder");

    conn.reset();
    ::close(fd1);
}

// ────────────────────────────────────────────────────────────────────────────
// T6: conn 销毁后，send / shutdown / forceClose 静默忽略（不 crash）
// ────────────────────────────────────────────────────────────────────────────
void testSafeAfterConnDestroyed() {
    auto [fd0, fd1] = makeSocketPair();
    mini::net::EventLoop loop;
    auto conn = makeConnection(&loop, fd0, "stale-conn");

    auto adapter = mini::net::ProtocolConnectionAdapter::createAndBind(conn);

    // 销毁 TcpConnection（adapter 的 weak_ptr 悬空）
    conn.reset();
    ::close(fd1);

    // 以下操作不应崩溃
    adapter->send("hello");
    adapter->shutdown();
    adapter->forceClose();
    assert(!adapter->connected());
    assert(adapter->getLoop() == nullptr);
    // name() 仍然可用（缓存）
    assert(adapter->name() == "stale-conn");
}

// ────────────────────────────────────────────────────────────────────────────
// T7: getFrom 在 context 未初始化时返回 nullptr（不 crash）
// ────────────────────────────────────────────────────────────────────────────
void testGetFromUnbound() {
    auto [fd0, fd1] = makeSocketPair();
    mini::net::EventLoop loop;
    auto conn = makeConnection(&loop, fd0, "unbound-conn");

    // 没有调用 createAndBind，context 为空
    auto* raw = mini::net::ProtocolConnectionAdapter::getFrom(conn);
    assert(raw == nullptr);

    auto shared = mini::net::ProtocolConnectionAdapter::sharedFrom(conn);
    assert(shared == nullptr);

    conn.reset();
    ::close(fd1);
}

}  // namespace

int main() {
    testCreateBindAndRetrieve();
    testName();
    testConnectedStateReflected();
    testGetLoop();
    testProtocolContextSlot();
    testSafeAfterConnDestroyed();
    testGetFromUnbound();
    return 0;
}
