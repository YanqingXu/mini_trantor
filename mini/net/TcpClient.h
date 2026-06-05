#pragma once

// TcpClient 是客户端 TCP 连接管理器，与 TcpServer 对称。
// 它通过 Connector 发起连接，管理连接建立后的 TcpConnection，
// 支持可配置的重连策略，所有状态变更在 owner loop 线程执行。
// 可选支持 TLS：通过 enableSsl() 配置后，新连接自动执行 TLS 握手。
// 可选支持 hostname 连接：通过 DnsResolver 异步解析后自动建立连接。
// v5-delta: 支持 TcpClientOptions、ConnectorEvent/ConnectionEvent/TlsEvent hook。

#include "mini/base/MetricsHook.h"
#include "mini/base/noncopyable.h"
#include "mini/net/Callbacks.h"
#include "mini/net/InetAddress.h"
#include "mini/net/TcpClientOptions.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace mini::net {

class Connector;
class DnsResolver;
class EventLoop;
class TlsContext;

class TcpClient : private mini::base::noncopyable {
public:
    TcpClient(EventLoop* loop, const InetAddress& serverAddr, std::string name);

    /// Hostname-based constructor. DNS resolution happens asynchronously
    /// when connect() is called, using the provided (or global) DnsResolver.
    TcpClient(EventLoop* loop, std::string hostname, uint16_t port,
              std::string name, std::shared_ptr<DnsResolver> resolver = nullptr);

    /// Options-based constructor. Configures Connector via TcpClientOptions.
    TcpClient(EventLoop* loop, const InetAddress& serverAddr, std::string name, TcpClientOptions options);

    ~TcpClient();

    /// Connect to the server. Safe to call cross-thread.
    void connect();

    /// Disconnect from the server. Safe to call cross-thread.
    void disconnect();

    /// Stop connector (cancel pending connect/retry). Safe to call cross-thread.
    void stop();

    /// Enable automatic reconnect on disconnect.
    void enableRetry() noexcept;
    void disableRetry() noexcept;
    bool retry() const noexcept;

    /// Enable TLS for this client. Must be called before connect().
    void enableSsl(std::shared_ptr<TlsContext> tlsContext, std::string hostname = "");

    const std::string& name() const noexcept;
    EventLoop* getLoop() const noexcept;
    TcpConnectionPtr connection() const;

    void setConnectionCallback(ConnectionCallback cb);
    void setMessageCallback(MessageCallback cb);
    void setWriteCompleteCallback(WriteCompleteCallback cb);

    // ── Metrics hooks (v5-delta) ──

    /// Set connector event hook. Fires on owner loop thread.
    void setConnectorEventCallback(ConnectorEventCallback cb);

    /// Set connection lifecycle event hook. Fires on owner loop thread.
    void setConnectionEventCallback(ConnectionEventCallback cb);

    /// Set TLS handshake event hook. Fires on owner loop thread.
    void setTlsEventCallback(TlsEventCallback cb);

private:
    void newConnection(int sockfd);
    void removeConnection(const TcpConnectionPtr& conn);
    void initConnector(const InetAddress& serverAddr);
    void initConnector(const InetAddress& serverAddr, const ConnectorOptions& options);
    void resolveAndConnect();

    EventLoop* loop_;
    std::string name_;
    std::shared_ptr<Connector> connector_;  // may be null until resolved for hostname-based
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;

    // Metrics hooks (v5-delta)
    ConnectorEventCallback connectorEventCallback_;
    ConnectionEventCallback connectionEventCallback_;
    TlsEventCallback tlsEventCallback_;

    bool retry_;
    bool connect_;
    int nextConnId_;
    mutable std::mutex mutex_;
    TcpConnectionPtr connection_;  // guarded by mutex_
    std::shared_ptr<TlsContext> tlsContext_;
    std::string tlsHostname_;

    // Hostname-based connect support
    std::string hostname_;
    uint16_t port_{0};
    std::shared_ptr<DnsResolver> resolver_;
    std::shared_ptr<bool> resolveGuard_;  // scope guard for pending DNS callback

    // Options (for hostname-based re-resolve with options)
    std::optional<ConnectorOptions> connectorOptions_;
};

}  // namespace mini::net
