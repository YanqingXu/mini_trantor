#pragma once

// RpcPoolOptions 收敛 RPC 连接池配置参数。
// - minConnections: 池中最少常驻连接数
// - maxConnections: 池中最大连接数（包含创建中的可用连接）
// - createOnDemand: 队列有积压时是否按需建新连接（直到 maxConnections）
// - connector: 透传底层 Connector 配置（重试、超时、退避）
//
// 设计上，连接生命周期与重试仍沿用 TcpClient/TcpClientOptions 语义。

#include "mini/net/ConnectorOptions.h"

#include <cstddef>
#include <stdexcept>

namespace mini::rpc {

struct RpcPoolOptions {
    /// 最小常驻连接数。最少 1。
    std::size_t minConnections = 1;

    /// 最大连接数。必须 >= minConnections。
    std::size_t maxConnections = 1;

    /// 当池中存在待发/排队请求，是否允许创建额外连接补齐并发。
    bool createOnDemand = false;

    /// 透传底层连接器参数（包含 retry、重试间隔、connectTimeout）。
    mini::net::ConnectorOptions connector{};

    /// 参数合法性检查；用于 pool 生命周期启动前。
    void validate() const {
        if (minConnections == 0) {
            throw std::invalid_argument("RpcPoolOptions: minConnections must be > 0");
        }
        if (maxConnections < minConnections) {
            throw std::invalid_argument("RpcPoolOptions: maxConnections must be >= minConnections");
        }
        if (maxConnections == 0) {
            throw std::invalid_argument("RpcPoolOptions: maxConnections must be > 0");
        }
        connector.validate();
    }
};

}  // namespace mini::rpc
