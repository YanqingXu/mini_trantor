// 验证 Task-01 统一传输抽象的最小契约：
// 1. TransportEndpoint 代理 TcpConnection 且支持会话上下文与ID。
// 2. ProtocolConnectionAdapter 通过 TransportEndpoint 构造，不再绑定 ITransport 细节。
// 3. TransportManager 支持 endpoint 注册/查询/注销和跨线程友好的 marshal 接口。

#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/ProtocolConnectionAdapter.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/transport/TransportEndpoint.h"
#include "mini/net/transport/TransportManager.h"

#include <cassert>
#include <cstddef>
#include <string>
#include <utility>
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
        loop, name, fd, mini::net::InetAddress(), mini::net::InetAddress());
}

void testTransportEndpointSessionContext() {
    auto [fd0, fd1] = makeSocketPair();
    mini::net::EventLoop loop;
    auto conn = makeConnection(&loop, fd0, "ep-session");

    auto endpoint = mini::net::transport::TransportEndpoint::create(
        conn, 2001, mini::net::transport::TransportKind::kTcp);
    assert(endpoint->sessionId() == 2001);
    assert(endpoint->transportKind() == mini::net::transport::TransportKind::kTcp);
    assert(endpoint->name() == "ep-session");

    endpoint->setTransportContext(std::string("ep-ctx"));
    assert(endpoint->getTransportContext().has_value());
    assert(endpoint->getTransportContext().type() == typeid(std::string));

    conn.reset();
    assert(endpoint->name() == "ep-session"); // name 已缓存
    assert(!endpoint->connected());

    ::close(fd1);
}

void testProtocolConnectionAdapterBindAndContext() {
    auto [fd0, fd1] = makeSocketPair();
    mini::net::EventLoop loop;
    auto conn = makeConnection(&loop, fd0, "proto-adapter");

    auto adapter = mini::net::ProtocolConnectionAdapter::createAndBind(conn);
    assert(adapter != nullptr);
    assert(adapter->name() == "proto-adapter");

    auto* raw = mini::net::ProtocolConnectionAdapter::getFrom(conn);
    assert(raw == adapter.get());

    adapter->setProtocolContext(std::string("http-state"));
    assert(adapter->getProtocolContext().type() == typeid(std::string));
    assert(adapter->sessionId() == mini::net::transport::kInvalidTransportSessionId);
    adapter->setSessionId(3001);
    assert(adapter->sessionId() == 3001);
    assert(adapter->transportKind() == mini::net::transport::TransportKind::kTcp);

    conn.reset();
    ::close(fd1);
}

void testTransportManagerLifecycle() {
    auto [fd0, fd1] = makeSocketPair();
    mini::net::EventLoop loop;
    auto conn = makeConnection(&loop, fd0, "manager");

    mini::net::transport::TransportManager manager(&loop);

    auto endpoint = mini::net::transport::TransportEndpoint::create(conn);
    const auto id = manager.registerEndpoint(endpoint);
    assert(id != mini::net::transport::kInvalidTransportSessionId);
    assert(manager.hasEndpoint(id));
    assert(manager.getEndpoint(id).get() == endpoint.get());
    assert(manager.endpointCount() == 1);

    manager.send(id, "ping");
    manager.close(id);
    assert(manager.deregisterEndpoint(id));
    assert(!manager.hasEndpoint(id));
    assert(manager.endpointCount() == 0);

    conn.reset();
    ::close(fd1);
}

}  // namespace

int main() {
    testTransportEndpointSessionContext();
    testProtocolConnectionAdapterBindAndContext();
    testTransportManagerLifecycle();
    return 0;
}
