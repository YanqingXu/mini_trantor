#include "mini/net/Buffer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/TcpClient.h"
#include "mini/net/TcpServer.h"
#include "mini/coroutine/Task.h"
#include <string>

template<class T> concept OwnsBroadcast = requires(T& server) { server.broadcast(std::string{}); };
template<class T> concept OwnsAoi = requires(T& server) { server.joinBroadcastAoi("s", "a"); };
static_assert(!OwnsBroadcast<mini::net::TcpServer>);
static_assert(!OwnsAoi<mini::net::TcpServer>);

int main() {
    mini::net::EventLoop loop;
    mini::net::TcpServer server(&loop, mini::net::InetAddress(0, true), "consumer");
    mini::net::Buffer buffer;
    buffer.append(std::string_view("package-ok"));
    return buffer.retrieveAllAsString() == "package-ok" ? 0 : 1;
}
