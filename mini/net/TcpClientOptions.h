#pragma once

// TcpClientOptions 收敛 TcpClient 的配置参数。
// 值语义、可复制、默认值与 v5-gamma 行为完全一致。
// 必须在 connect() 前设置。

#include "mini/net/ConnectorOptions.h"

namespace mini::net {

struct TcpClientOptions {
    /// 内嵌 Connector 配置。TcpClient 将其传播到内部 Connector。
    ConnectorOptions connector;

    /// 是否在连接断开后自动重连。默认 false。
    /// 与 TcpClient::enableRetry() 等效，两者并存。
    bool retry = false;

    /// 验证选项合法性。不合法时抛 std::invalid_argument。
    void validate() const {
        connector.validate();
    }
};

}  // namespace mini::net
