// Contract tests for HTTP client.
//
// 1. onConnection/request callbacks must run on owner EventLoop thread.
// 2. keep-alive 复用下两个串行请求能在同一连接上完成。
// 3. 服务端 Connection: close 后，client 应自动重连后继续发起下一请求。
// 4. enableRetry=true 时，服务器延迟启动情况下应在首次成功连接后执行一次请求。
// 5. enableRetry=false 时，连接失败应快速失败 pending 请求并返回 NotConnected。
// 6. 在 Connection: close 语义下，连接断开窗口中已入队的请求应继续顺序执行。
// 7. enableRetry=false 时，多个 pending 请求应一并失败，且失败原因一致。
// 8. Connection: close 后，如果在重连窗口再入队请求，续发请求仍保持顺序。

#include "mini/http/HttpClient.h"
#include "mini/http/HttpServer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpConnection.h"

#include <cassert>
#include <chrono>
#include <arpa/inet.h>
#include <future>
#include <memory>
#include <netinet/in.h>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

uint16_t allocateTestPort() {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    assert(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    assert(::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = static_cast<socklen_t>(sizeof(addr));
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    const uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

}  // namespace

void testCallbacksRunOnOwnerLoop() {
    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();
    const uint16_t port = allocateTestPort();
    std::promise<void> done;
    auto doneFuture = done.get_future();

    auto server = std::make_unique<mini::http::HttpServer>(
        loop, mini::net::InetAddress(port, true), "http_client_contract_owner_loop");

    auto client = std::make_unique<mini::http::HttpClient>(
        loop, mini::net::InetAddress("127.0.0.1", port), "http_client_contract_owner_loop_client");
    auto* clientRaw = client.get();
    auto* serverRaw = server.get();

    loop->runInLoop([&] {
        server->setHttpCallback([&](const mini::http::HttpRequest&, mini::http::HttpResponse* resp) {
            resp->setStatusCode(mini::http::HttpResponse::k200Ok);
            resp->setBody("ok");
        });
        server->start();

        clientRaw->setConnectionCallback([loop,
                                         clientRaw,
                                         &done](const mini::net::TcpConnectionPtr& conn) {
            loop->assertInLoopThread();
            if (!conn->connected()) {
                return;
            }

            clientRaw->asyncGet(
                "/",
                {},
                [loop, &done](mini::net::Expected<mini::http::HttpResponse> resp) {
                    loop->assertInLoopThread();
                    assert(resp);
                    assert(resp->statusCode() == mini::http::HttpResponse::k200Ok);
                    assert(resp->body() == "ok");
                    done.set_value();
                },
                2000);
        });

        clientRaw->connect();
    });

    assert(doneFuture.wait_for(3s) == std::future_status::ready);
    {
        std::promise<void> cleanupDone;
        auto cleanupFuture = cleanupDone.get_future();
        loop->runInLoop([&] {
            clientRaw->stop();
            serverRaw->stop();
            client.reset();
            server.reset();
            loop->quit();
            cleanupDone.set_value();
        });
        cleanupFuture.wait();
    }
}

void testKeepAliveTwoRequestsInOneConnection() {
    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();
    const uint16_t port = allocateTestPort();
    std::promise<void> done;
    auto doneFuture = done.get_future();

    auto server = std::make_unique<mini::http::HttpServer>(
        loop, mini::net::InetAddress(port, true), "http_client_contract_keepalive");
    auto client = std::make_unique<mini::http::HttpClient>(
        loop, mini::net::InetAddress("127.0.0.1", port), "http_client_contract_keepalive_client");
    auto* clientRaw = client.get();
    auto* serverRaw = server.get();

    int requestCount = 0;
    int connectedCount = 0;

    loop->runInLoop([&] {
        server->setHttpCallback([&](const mini::http::HttpRequest&,
                                    mini::http::HttpResponse* resp) {
            const auto idx = ++requestCount;
            resp->setStatusCode(mini::http::HttpResponse::k200Ok);
            resp->setBody("r" + std::to_string(idx));
            resp->setCloseConnection(false);
        });
        server->start();

        clientRaw->setConnectionCallback([loop,
                                           clientRaw,
                                           &requestCount,
                                           &connectedCount,
                                           &done](const mini::net::TcpConnectionPtr& conn) {
            loop->assertInLoopThread();
            if (!conn->connected()) {
                return;
            }
            ++connectedCount;

            clientRaw->asyncGet(
                "/first",
                {},
                [loop,
                 clientRaw,
                 &requestCount,
                 &done](mini::net::Expected<mini::http::HttpResponse> resp) {
                    loop->assertInLoopThread();
                    assert(resp);
                    assert(resp->statusCode() == mini::http::HttpResponse::k200Ok);
                    assert(resp->body() == "r1");

                    clientRaw->asyncGet(
                        "/second",
                        {},
                            [clientRaw,
                             loop,
                             &done](mini::net::Expected<mini::http::HttpResponse> resp2) {
                            loop->assertInLoopThread();
                            if (!resp2) {
                                done.set_value();
                                return;
                            }
                            assert(resp2->statusCode() == mini::http::HttpResponse::k200Ok);
                            assert(resp2->body() == "r2");
                            done.set_value();
                        },
                        2000);
                },
                2000);
        });

        clientRaw->connect();
    });

    assert(doneFuture.wait_for(3s) == std::future_status::ready);
    std::promise<void> cleanupDone;
    auto cleanupFuture = cleanupDone.get_future();
    loop->runInLoop([&] {
        clientRaw->stop();
        serverRaw->stop();
        client.reset();
        server.reset();
        loop->quit();
        cleanupDone.set_value();
    });
    cleanupFuture.wait();
    assert(requestCount == 2);
    assert(connectedCount == 1);
}

void testReconnectAfterServerClose() {
    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();
    const uint16_t port = allocateTestPort();
    std::promise<void> done;
    auto doneFuture = done.get_future();

    auto server = std::make_unique<mini::http::HttpServer>(
        loop, mini::net::InetAddress(port, true), "http_client_contract_reconnect");
    auto client = std::make_unique<mini::http::HttpClient>(
        loop, mini::net::InetAddress("127.0.0.1", port), "http_client_contract_reconnect_client");
    auto* clientRaw = client.get();
    auto* serverRaw = server.get();

    int requestCount = 0;
    int connectedCount = 0;
    bool firstDispatched = false;

    loop->runInLoop([&] {
        server->setHttpCallback([&](const mini::http::HttpRequest&,
                                    mini::http::HttpResponse* resp) {
            const auto idx = ++requestCount;
            resp->setStatusCode(mini::http::HttpResponse::k200Ok);
            resp->setBody("ok-" + std::to_string(idx));
            resp->setCloseConnection(true);
        });
        server->start();

        clientRaw->setConnectionCallback([loop,
                                       clientRaw,
                                       &requestCount,
                                       &connectedCount,
                                       &done,
                                       &firstDispatched](const mini::net::TcpConnectionPtr& conn) {
            loop->assertInLoopThread();
            if (!conn->connected()) {
                return;
            }
            ++connectedCount;

            if (!firstDispatched) {
                firstDispatched = true;
                clientRaw->asyncGet(
                    "/reconnect_1",
                    {},
                    [loop,
                     clientRaw,
                     &done](mini::net::Expected<mini::http::HttpResponse> resp) {
                        loop->assertInLoopThread();
                        assert(resp);
                        assert(resp->body() == "ok-1");

                        clientRaw->asyncGet(
                            "/reconnect_2",
                            {},
                                [clientRaw,
                                 loop,
                                 &done](mini::net::Expected<mini::http::HttpResponse> resp2) {
                                loop->assertInLoopThread();
                                assert(resp2);
                                assert(resp2->body() == "ok-2");
                                done.set_value();
                            },
                            2000);
                    },
                    2000);
            }
        });

        clientRaw->connect();
    });

    assert(doneFuture.wait_for(3s) == std::future_status::ready);
    std::promise<void> cleanupDone;
    auto cleanupFuture = cleanupDone.get_future();
    loop->runInLoop([&] {
        clientRaw->stop();
        serverRaw->stop();
        client.reset();
        server.reset();
        loop->quit();
        cleanupDone.set_value();
    });
    cleanupFuture.wait();
    assert(requestCount == 2);
    assert(connectedCount >= 2);
}

void testRetryToLateStartedServer() {
    std::promise<void> done;
    auto doneFuture = done.get_future();

    const uint16_t port = allocateTestPort();
    mini::http::HttpClientOptions options;
    options.enableRetry = true;
    options.connector.initRetryDelay = std::chrono::milliseconds(30);
    options.connector.maxRetryDelay = std::chrono::milliseconds(30);
    options.defaultTimeoutMs = 1000;

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    auto server = std::make_unique<mini::http::HttpServer>(
        loop, mini::net::InetAddress(port, true), "http_client_contract_retry_delayed_server");
    auto client = std::make_unique<mini::http::HttpClient>(
        loop, mini::net::InetAddress("127.0.0.1", port), "http_client_contract_retry_delayed_client", options);
    auto* clientRaw = client.get();
    auto* serverRaw = server.get();

    int connectedCount = 0;
    int requestCount = 0;
    bool started = false;

    loop->runInLoop([&] {
        server->setHttpCallback([&](const mini::http::HttpRequest&,
                                    mini::http::HttpResponse* resp) {
            ++requestCount;
            resp->setStatusCode(mini::http::HttpResponse::k200Ok);
            resp->setBody("ok");
        });

        clientRaw->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
            if (conn->connected()) {
                ++connectedCount;
            }
        });

        clientRaw->asyncGet("/", {},
                           [loop, &done](mini::net::Expected<mini::http::HttpResponse> resp) {
            loop->assertInLoopThread();
            assert(resp);
            assert(resp->statusCode() == mini::http::HttpResponse::k200Ok);
            assert(resp->body() == "ok");
            done.set_value();
        }, 2000);

        loop->runAfter(120ms, [serverRaw, &started]() {
            if (started) {
                return;
            }
            started = true;
            serverRaw->start();
        });
    });

    assert(doneFuture.wait_for(4s) == std::future_status::ready);

    std::promise<void> cleanupDone;
    auto cleanupFuture = cleanupDone.get_future();
    loop->runInLoop([&] {
        clientRaw->stop();
        serverRaw->stop();
        client.reset();
        server.reset();
        loop->quit();
        cleanupDone.set_value();
    });
    cleanupFuture.wait();

    assert(requestCount == 1);
    assert(connectedCount >= 1);
}

void testRetryFailureWithoutRetryPolicy() {
    std::promise<void> done;
    auto doneFuture = done.get_future();

    // Use a free port but do not start any server.
    const uint16_t port = allocateTestPort();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::http::HttpClientOptions options;
    options.enableRetry = false;

    auto client = std::make_unique<mini::http::HttpClient>(
        loop, mini::net::InetAddress("127.0.0.1", port), "http_client_contract_retry_disabled_client", options);
    auto* clientRaw = client.get();

    loop->runInLoop([&] {
        clientRaw->asyncGet("/never", {},
                           [loop, &done](mini::net::Expected<mini::http::HttpResponse> resp) {
            loop->assertInLoopThread();
            assert(!resp);
            assert(resp.error() == mini::net::NetError::NotConnected);
            done.set_value();
        }, 300);
    });

    assert(doneFuture.wait_for(1s) == std::future_status::ready);

    std::promise<void> cleanupDone;
    auto cleanupFuture = cleanupDone.get_future();
    loop->runInLoop([&] {
        clientRaw->stop();
        client.reset();
        loop->quit();
        cleanupDone.set_value();
    });
    cleanupFuture.wait();
}

void testRetryFailureWithoutRetryPolicyQueuedRequests() {
    std::promise<void> done;
    auto doneFuture = done.get_future();

    const uint16_t port = allocateTestPort();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::http::HttpClientOptions options;
    options.enableRetry = false;

    auto client = std::make_unique<mini::http::HttpClient>(
        loop,
        mini::net::InetAddress("127.0.0.1", port),
        "http_client_contract_retry_disabled_queue_client",
        options);
    auto* clientRaw = client.get();

    int failureCount = 0;
    loop->runInLoop([&] {
        clientRaw->asyncGet("/never-first",
                           {},
                           [loop, &done, &failureCount](mini::net::Expected<mini::http::HttpResponse> resp) {
                               loop->assertInLoopThread();
                               assert(!resp);
                               assert(resp.error() == mini::net::NetError::NotConnected);
                               if (++failureCount == 2) {
                                   done.set_value();
                               }
                           },
                           300);

        clientRaw->asyncGet("/never-second",
                           {},
                           [loop, &done, &failureCount](mini::net::Expected<mini::http::HttpResponse> resp) {
                               loop->assertInLoopThread();
                               assert(!resp);
                               assert(resp.error() == mini::net::NetError::NotConnected);
                               if (++failureCount == 2) {
                                   done.set_value();
                               }
                           },
                           300);
    });

    assert(doneFuture.wait_for(1s) == std::future_status::ready);
    assert(failureCount == 2);

    std::promise<void> cleanupDone;
    auto cleanupFuture = cleanupDone.get_future();
    loop->runInLoop([&] {
        clientRaw->stop();
        client.reset();
        loop->quit();
        cleanupDone.set_value();
    });
    cleanupFuture.wait();
}

void testEnqueueDuringReconnectWindow() {
    std::promise<void> done;
    auto doneFuture = done.get_future();
    const uint16_t port = allocateTestPort();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    auto server = std::make_unique<mini::http::HttpServer>(
        loop, mini::net::InetAddress(port, true), "http_client_contract_retry_window_server");
    auto client = std::make_unique<mini::http::HttpClient>(
        loop, mini::net::InetAddress("127.0.0.1", port), "http_client_contract_retry_window_client");
    auto* clientRaw = client.get();
    auto* serverRaw = server.get();

    int requestCount = 0;
    int connectedCount = 0;
    bool firstSent = false;

    loop->runInLoop([&] {
        server->setHttpCallback([&](const mini::http::HttpRequest&,
                                    mini::http::HttpResponse* resp) {
            const auto idx = ++requestCount;
            resp->setStatusCode(mini::http::HttpResponse::k200Ok);
            resp->setBody("ok-" + std::to_string(idx));
            resp->setCloseConnection(idx == 1);
        });
        server->start();

        clientRaw->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
            if (!conn->connected()) {
                return;
            }
            ++connectedCount;
            if (firstSent) {
                return;
            }
            firstSent = true;

            clientRaw->asyncGet(
                "/window-1",
                {},
                [clientRaw,
                 loop,
                 &done](mini::net::Expected<mini::http::HttpResponse> resp) {
                    loop->assertInLoopThread();
                    assert(resp);
                    assert(resp->body() == "ok-1");

                    clientRaw->asyncGet(
                        "/window-2",
                        {},
                        [loop, &done](mini::net::Expected<mini::http::HttpResponse> resp2) {
                            loop->assertInLoopThread();
                            assert(resp2);
                            assert(resp2->body() == "ok-2");
                            done.set_value();
                        },
                        2000);
                },
                2000);
        });

        clientRaw->connect();
    });

    assert(doneFuture.wait_for(4s) == std::future_status::ready);

    std::promise<void> cleanupDone;
    auto cleanupFuture = cleanupDone.get_future();
    loop->runInLoop([&] {
        clientRaw->stop();
        serverRaw->stop();
        client.reset();
        server.reset();
        loop->quit();
        cleanupDone.set_value();
    });
    cleanupFuture.wait();

    assert(requestCount == 2);
    assert(connectedCount >= 2);
}

void testQueuedRequestsAfterConnectionClose() {
    std::promise<void> done;
    auto doneFuture = done.get_future();
    const uint16_t port = allocateTestPort();

    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    auto server = std::make_unique<mini::http::HttpServer>(
        loop, mini::net::InetAddress(port, true), "http_client_contract_retry_queue");
    auto client = std::make_unique<mini::http::HttpClient>(
        loop, mini::net::InetAddress("127.0.0.1", port), "http_client_contract_retry_queue_client");
    auto* clientRaw = client.get();
    auto* serverRaw = server.get();

    int requestCount = 0;
    int connectedCount = 0;
    bool queued = false;

    loop->runInLoop([&] {
        server->setHttpCallback([&](const mini::http::HttpRequest&,
                                    mini::http::HttpResponse* resp) {
            const auto idx = ++requestCount;
            resp->setStatusCode(mini::http::HttpResponse::k200Ok);
            resp->setBody("ok-" + std::to_string(idx));
            resp->setCloseConnection(true);
        });
        server->start();

        clientRaw->setConnectionCallback([&](const mini::net::TcpConnectionPtr& conn) {
            if (!conn->connected()) {
                return;
            }
            ++connectedCount;
            if (queued) {
                return;
            }
            queued = true;

            clientRaw->asyncGet(
                "/queued-1",
                {},
                [loop](mini::net::Expected<mini::http::HttpResponse> resp) {
                    loop->assertInLoopThread();
                    assert(resp);
                    assert(resp->body() == "ok-1");
                },
                2000);
            clientRaw->asyncGet(
                "/queued-2",
                {},
                [loop, &done](mini::net::Expected<mini::http::HttpResponse> resp2) {
                    loop->assertInLoopThread();
                    assert(resp2);
                    assert(resp2->body() == "ok-2");
                    done.set_value();
                },
                2000);
        });

        clientRaw->connect();
    });

    assert(doneFuture.wait_for(4s) == std::future_status::ready);
    std::promise<void> cleanupDone;
    auto cleanupFuture = cleanupDone.get_future();
    loop->runInLoop([&] {
        clientRaw->stop();
        serverRaw->stop();
        client.reset();
        server.reset();
        loop->quit();
        cleanupDone.set_value();
    });
    cleanupFuture.wait();
    assert(requestCount == 2);
    assert(connectedCount >= 2);
}

int main() {
    testCallbacksRunOnOwnerLoop();
    testKeepAliveTwoRequestsInOneConnection();
    testReconnectAfterServerClose();
    testRetryToLateStartedServer();
    testRetryFailureWithoutRetryPolicy();
    testRetryFailureWithoutRetryPolicyQueuedRequests();
    testEnqueueDuringReconnectWindow();
    testQueuedRequestsAfterConnectionClose();
    return 0;
}
