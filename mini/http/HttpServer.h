#pragma once

// HttpServer 是 TcpServer 的 HTTP/1.1 协议适配器。
// 它为每个连接维护 HttpContext，将原始字节流解析为 HttpRequest，
// 调用用户的 HttpCallback，然后序列化 HttpResponse 发送出去。
// 遵守 one-loop-per-thread 线程模型。
//
// v5-epsilon: 内部 send / shutdown 路径已迁移到 IProtocolConnection 窄接口，
// 不再直接依赖 TcpConnection 宽 API。

#include "mini/http/HttpRequest.h"
#include "mini/http/HttpResponse.h"
#include "mini/net/Callbacks.h"
#include "mini/net/ProtocolConnection.h"
#include "mini/net/TcpServer.h"

#include <memory>
#include <functional>
#include <string>

namespace mini::net {
class EventLoop;
class InetAddress;
class ProtocolConnectionAdapter;
}  // namespace mini::net

namespace mini::http {

using HttpCallback = std::function<void(const HttpRequest&, HttpResponse*)>;
using TransportSessionHook = std::function<void(const mini::net::TcpConnectionPtr&,
                                               std::weak_ptr<mini::net::ProtocolConnectionAdapter>,
                                               bool)>;

class HttpServer {
public:
    HttpServer(mini::net::EventLoop* loop,
               const mini::net::InetAddress& listenAddr,
               std::string name,
               bool reusePort = true);

    void setHttpCallback(HttpCallback cb) { httpCallback_ = std::move(cb); }
    void setThreadNum(int numThreads) { server_.setThreadNum(numThreads); }
    void setTransportSessionHook(TransportSessionHook hook);

    void start();
    void stop();

private:
    void onConnection(const mini::net::TcpConnectionPtr& conn);
    void onMessage(const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf);
    void onRequest(mini::net::IProtocolConnection* proto, const HttpRequest& req);

    mini::net::TcpServer server_;
    HttpCallback httpCallback_;
    TransportSessionHook transportSessionHook_;
};

}  // namespace mini::http
