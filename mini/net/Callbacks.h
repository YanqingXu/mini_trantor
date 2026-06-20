#pragma once

// Callbacks.h 集中定义网络层公开使用的回调签名。
// 它让连接、消息、关闭与线程初始化的契约类型保持一致。

#include <functional>
#include <cstddef>
#include <memory>
#include <string_view>

namespace mini::net {
namespace transport {
class ITransportChannel;
class ITransportSession;
}

class Buffer;
class EventLoop;
class TcpConnection;

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer*)>;
using LogicMessageCallback = std::function<void(const TcpConnectionPtr&, std::string_view)>;
using HighWaterMarkCallback = std::function<void(const TcpConnectionPtr&, std::size_t)>;
using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;
using CloseCallback = std::function<void(const TcpConnectionPtr&)>;
using ThreadInitCallback = std::function<void(EventLoop*)>;

// 传输层统一回调（Task-01 的抽象衔接点）
using TransportReadCallback =
    std::function<void(std::shared_ptr<transport::ITransportChannel> channel, mini::net::Buffer*)>;
using TransportCloseCallback =
    std::function<void(std::shared_ptr<transport::ITransportSession> session)>;

}  // namespace mini::net
