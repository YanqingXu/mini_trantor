#pragma once

// IProtocolConnection — 协议层允许依赖的最小 transport-facing 接口。
//
// 设计目标：
//   协议层（HTTP / WebSocket / RPC）只通过此接口操作底层连接，
//   而不直接依赖 TcpConnection 的宽 public API。
//
// 提供的能力（五类）：
//   1. 发送字节序列
//   2. 请求半关闭或强制关闭
//   3. 访问和维护 per-connection protocol state
//   4. 查询连接是否仍处于可用状态
//   5. 查询 owner loop（用于断言线程亲和）
//
// 不提供的能力（明确排除）：
//   - TLS / SSL 细节
//   - awaiter / coroutine 相关 API
//   - backpressure controller
//   - socket / channel 底层对象
//   - MetricsHook
//
// 线程规则：
//   所有方法只能在 owner loop 线程调用（与 TcpConnection 相同约束）。
//
// 生命周期规则：
//   ProtocolConnectionAdapter 通过 TcpConnection::setContext() 存储，
//   随连接 teardown 自然销毁，不需要手工回收。
//
// v5-epsilon

#include <any>
#include <memory>
#include <string_view>

namespace mini::net {

class EventLoop;

// ---------------------------------------------------------------------------
// IProtocolConnection
// ---------------------------------------------------------------------------

class IProtocolConnection {
public:
    virtual ~IProtocolConnection() = default;

    // 发送字节序列（非阻塞，通过 TcpConnection 的发送缓冲区）
    virtual void send(std::string_view data) = 0;

    // 请求半关闭：发送完缓冲区剩余数据后关闭写端
    virtual void shutdown() = 0;

    // 立即强制关闭连接
    virtual void forceClose() = 0;

    // 连接是否仍处于可发送状态
    virtual bool connected() const noexcept = 0;

    // Owner loop（用于线程亲和断言）
    virtual EventLoop* getLoop() const noexcept = 0;

    // 连接名称（用于日志）
    virtual std::string_view name() const noexcept = 0;

    // Per-connection 协议状态槽（与 TcpConnection::setContext 独立的第二个槽）
    // 协议适配器用此槽存储 HttpContext / WebSocket ConnectionContext / etc.
    virtual void setProtocolContext(std::any ctx) = 0;
    virtual const std::any& getProtocolContext() const noexcept = 0;
    virtual std::any& getProtocolContext() noexcept = 0;
};

using ProtocolConnectionPtr = std::shared_ptr<IProtocolConnection>;

}  // namespace mini::net
