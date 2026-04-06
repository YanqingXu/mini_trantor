#include "mini/coroutine/Task.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

mini::coroutine::Task<void> echoSession(mini::net::TcpConnectionPtr connection) {
    while (connection->connected()) {
        auto result = co_await connection->asyncReadSome();
        if (!result) {
            break;
        }
        auto writeResult = co_await connection->asyncWrite(std::move(*result));
        if (!writeResult) {
            break;
        }
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    const uint16_t port = argc > 1 ? static_cast<uint16_t>(std::stoi(argv[1])) : 8889;
    const int threads = argc > 2 ? std::stoi(argv[2]) : 0;

    mini::net::EventLoop loop;
    mini::net::TcpServer server(&loop, mini::net::InetAddress(port), "coroutine_echo_server");
    server.setThreadNum(threads);
    server.setConnectionCallback([](const mini::net::TcpConnectionPtr& connection) {
        if (connection->connected()) {
            echoSession(connection).detach();
            return;
        }
        std::cout << "session closed: " << connection->name() << '\n';
    });

    server.start();
    std::cout << "coroutine_echo_server listening on 0.0.0.0:" << port << " with " << threads
              << " worker threads\n";
    loop.loop();
    return EXIT_SUCCESS;
}
