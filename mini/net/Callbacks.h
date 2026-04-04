#pragma once

// Callbacks.h 集中定义网络层公开使用的回调签名。
// 它让连接、消息、关闭与线程初始化的契约类型保持一致。

#include <functional>
#include <memory>

namespace mini::net {

class Buffer;
class EventLoop;
class TcpConnection;

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*)>;
using HighWaterMarkCallback = std::function<void(const TcpConnectionPtr&, std::size_t)>;
using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;
using CloseCallback = std::function<void(const TcpConnectionPtr&)>;
using ThreadInitCallback = std::function<void(EventLoop*)>;

}  // namespace mini::net
