#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"
#include "mini/net/Buffer.h"
#include "mini/net/TcpConnection.h"
#include "mini/net/TcpServer.h"

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    const uint16_t port = argc > 1 ? static_cast<uint16_t>(std::stoi(argv[1])) : 8888;
    const int threads = argc > 2 ? std::stoi(argv[2]) : 0;

    mini::net::EventLoop loop;
    mini::net::TcpServer server(&loop, mini::net::InetAddress(port), "echo_server");
    server.setThreadNum(threads);
    server.setConnectionCallback([](const mini::net::TcpConnectionPtr& connection) {
        std::cout << (connection->connected() ? "connected: " : "disconnected: ") << connection->name()
                  << " peer=" << connection->peerAddress().toIpPort() << '\n';
    });
    server.setMessageCallback([](const mini::net::TcpConnectionPtr& connection, mini::net::Buffer* buffer) {
        connection->send(buffer->retrieveAllAsString());
    });

    server.start();
    std::cout << "echo_server listening on 0.0.0.0:" << port << " with " << threads << " worker threads\n";
    loop.loop();
    return EXIT_SUCCESS;
}
