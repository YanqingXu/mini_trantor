#pragma once

// ProtocolConnectionAdapter — IProtocolConnection 的 TcpConnection 适配实现。
//
// 职责：
//   以 weak_ptr<TcpConnection> 为底座，实现 IProtocolConnection 窄接口，
//   为协议层（HTTP / WebSocket / RPC）提供稳定的 transport-facing 依赖面。
//
// 使用方式：
//   在 TcpServer::connectionCallback 中（onConnection 时），构造 adapter 并通过
//   TcpConnection::setContext() 存储为 shared_ptr<ProtocolConnectionAdapter>。
//   协议路径从 any_cast<shared_ptr<ProtocolConnectionAdapter>> 取出，
//   调用 send / shutdown / forceClose / getProtocolContext() 等方法。
//
// 生命周期：
//   - adapter 通过 setContext 存储在 TcpConnection 中
//   - TcpConnection 析构时 context 自然析构，adapter 随之销毁
//   - RPC 等延迟回调捕获 shared_ptr<ProtocolConnectionAdapter>；
//     adapter 内以 weak_ptr<TcpConnection> 保证安全（不阻止连接释放）
//
// 所有权：
//   TcpConnection（通过 std::any context）持有 adapter 的 shared_ptr。
//   adapter 持有 TcpConnection 的 weak_ptr（不形成循环）。
//
// 线程规则：
//   所有方法只能在 owner loop 线程调用。
//
// v5-epsilon

#include "mini/net/Callbacks.h"
#include "mini/net/ProtocolConnection.h"

#include <any>
#include <memory>
#include <string_view>

namespace mini::net {

class TcpConnection;
class EventLoop;

// ---------------------------------------------------------------------------
// ProtocolConnectionAdapter
// ---------------------------------------------------------------------------

class ProtocolConnectionAdapter final : public IProtocolConnection {
public:
    // 构造时捕获 TcpConnectionPtr 的 weak_ptr（避免循环引用）
    explicit ProtocolConnectionAdapter(const TcpConnectionPtr& conn);

    // IProtocolConnection
    void send(std::string_view data) override;
    void shutdown() override;
    void forceClose() override;
    bool connected() const noexcept override;
    EventLoop* getLoop() const noexcept override;
    std::string_view name() const noexcept override;

    void setProtocolContext(std::any ctx) override;
    const std::any& getProtocolContext() const noexcept override;
    std::any& getProtocolContext() noexcept override;

    // 便捷工厂：创建 adapter 并存入 conn->setContext()，返回 shared_ptr。
    // 调用方在 onConnection 时使用，之后通过 getFrom(conn) 取回。
    static std::shared_ptr<ProtocolConnectionAdapter>
    createAndBind(const TcpConnectionPtr& conn);

    // 从已绑定 context 的连接取回 adapter 指针（不拥有）。
    // 如果 context 中没有 adapter，返回 nullptr。
    static ProtocolConnectionAdapter* getFrom(const TcpConnectionPtr& conn);

    // 同上，返回 shared_ptr（可安全捕获到 lambda）。
    static std::shared_ptr<ProtocolConnectionAdapter>
    sharedFrom(const TcpConnectionPtr& conn);

private:
    std::weak_ptr<TcpConnection> conn_;
    std::any protocolContext_;
    std::string name_;  // 预先缓存 name 以确保 conn 销毁后仍可查询
};

}  // namespace mini::net
