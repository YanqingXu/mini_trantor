#pragma once

// HttpServer 是 TcpServer 的 HTTP/1.1 协议适配器。
// 它为每个连接维护 HttpContext，将原始字节流解析为 HttpRequest，
// 调用用户的 HttpCallback，然后序列化 HttpResponse 发送出去。
// 遵守 one-loop-per-thread 线程模型。

#include "mini/http/HttpRequest.h"
#include "mini/http/HttpResponse.h"
#include "mini/net/Callbacks.h"
#include "mini/net/TcpServer.h"

#include <functional>
#include <string>

namespace mini::net {
class EventLoop;
class InetAddress;
}  // namespace mini::net

namespace mini::http {

using HttpCallback = std::function<void(const HttpRequest&, HttpResponse*)>;

class HttpServer {
public:
    HttpServer(mini::net::EventLoop* loop,
               const mini::net::InetAddress& listenAddr,
               std::string name,
               bool reusePort = true);

    void setHttpCallback(HttpCallback cb) { httpCallback_ = std::move(cb); }
    void setThreadNum(int numThreads) { server_.setThreadNum(numThreads); }

    void start();

private:
    void onConnection(const mini::net::TcpConnectionPtr& conn);
    void onMessage(const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf);
    void onRequest(const mini::net::TcpConnectionPtr& conn, const HttpRequest& req);

    mini::net::TcpServer server_;
    HttpCallback httpCallback_;
};

}  // namespace mini::http
