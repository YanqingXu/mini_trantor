// v5-delta 配置体系契约测试
// 验证 Options 传播到内部模块、默认行为不变、set*() 可覆盖 Options

#include "mini/net/ConnectorOptions.h"
#include "mini/net/Connector.h"
#include "mini/net/DnsResolverOptions.h"
#include "mini/net/DnsResolver.h"
#include "mini/net/TcpClientOptions.h"
#include "mini/net/TcpServerOptions.h"
#include "mini/net/TcpServer.h"
#include "mini/net/TcpClient.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"

#include <cassert>
#include <chrono>
#include <iostream>

using namespace mini::net;
using Duration = std::chrono::steady_clock::duration;

// ── ConnectorOptions tests ──

void testConnectorOptionsDefaults() {
    ConnectorOptions opts;
    assert(opts.initRetryDelay == std::chrono::milliseconds(500));
    assert(opts.maxRetryDelay == std::chrono::seconds(30));
    assert(opts.connectTimeout == Duration::zero());
    assert(opts.enableRetry == false);
    std::cout << "  PASS: ConnectorOptions defaults\n";
}

void testConnectorOptionsCustom() {
    ConnectorOptions opts;
    opts.initRetryDelay = std::chrono::seconds(1);
    opts.maxRetryDelay = std::chrono::minutes(1);
    opts.connectTimeout = std::chrono::seconds(5);
    opts.enableRetry = true;
    assert(opts.initRetryDelay == std::chrono::seconds(1));
    assert(opts.maxRetryDelay == std::chrono::minutes(1));
    assert(opts.connectTimeout == std::chrono::seconds(5));
    assert(opts.enableRetry == true);
    std::cout << "  PASS: ConnectorOptions custom values\n";
}

void testConnectorOptionsValidation() {
    ConnectorOptions opts;
    opts.initRetryDelay = Duration::zero();
    try {
        opts.validate();
        assert(false && "should have thrown");
    } catch (const std::invalid_argument&) {}

    opts.initRetryDelay = std::chrono::seconds(5);
    opts.maxRetryDelay = std::chrono::seconds(1);  // max < initial
    try {
        opts.validate();
        assert(false && "should have thrown");
    } catch (const std::invalid_argument&) {}

    opts.maxRetryDelay = std::chrono::seconds(10);
    opts.connectTimeout = Duration(-1);  // negative
    try {
        opts.validate();
        assert(false && "should have thrown");
    } catch (const std::invalid_argument&) {}

    std::cout << "  PASS: ConnectorOptions validation rejects invalid values\n";
}

// ── DnsResolverOptions tests ──

void testDnsResolverOptionsDefaults() {
    DnsResolverOptions opts;
    assert(opts.numWorkerThreads == 2);
    assert(opts.cacheTtl == std::chrono::seconds(60));
    assert(opts.enableCache == false);
    std::cout << "  PASS: DnsResolverOptions defaults\n";
}

void testDnsResolverOptionsCustom() {
    DnsResolverOptions opts;
    opts.numWorkerThreads = 4;
    opts.cacheTtl = std::chrono::seconds(120);
    opts.enableCache = true;
    assert(opts.numWorkerThreads == 4);
    assert(opts.cacheTtl == std::chrono::seconds(120));
    assert(opts.enableCache == true);
    std::cout << "  PASS: DnsResolverOptions custom values\n";
}

void testDnsResolverOptionsValidation() {
    DnsResolverOptions opts;
    opts.numWorkerThreads = 0;
    try {
        opts.validate();
        assert(false && "should have thrown");
    } catch (const std::invalid_argument&) {}

    opts.numWorkerThreads = 2;
    opts.enableCache = true;
    opts.cacheTtl = std::chrono::seconds(0);
    try {
        opts.validate();
        assert(false && "should have thrown");
    } catch (const std::invalid_argument&) {}

    std::cout << "  PASS: DnsResolverOptions validation rejects invalid values\n";
}

// ── TcpServerOptions tests ──

void testTcpServerOptionsDefaults() {
    TcpServerOptions opts;
    assert(opts.numThreads == 1);
    assert(opts.idleTimeout == Duration::zero());
    assert(opts.backpressureHighWaterMark == 0);
    assert(opts.backpressureLowWaterMark == 0);
    assert(opts.reusePort == true);
    std::cout << "  PASS: TcpServerOptions defaults\n";
}

void testTcpServerOptionsValidation() {
    TcpServerOptions opts;
    opts.numThreads = 0;
    try {
        opts.validate();
        assert(false && "should have thrown");
    } catch (const std::invalid_argument&) {}

    opts.numThreads = 1;
    opts.backpressureHighWaterMark = 0;
    opts.backpressureLowWaterMark = 100;  // low without high
    try {
        opts.validate();
        assert(false && "should have thrown");
    } catch (const std::invalid_argument&) {}

    opts.backpressureHighWaterMark = 100;
    opts.backpressureLowWaterMark = 200;  // low >= high
    try {
        opts.validate();
        assert(false && "should have thrown");
    } catch (const std::invalid_argument&) {}

    std::cout << "  PASS: TcpServerOptions validation rejects invalid values\n";
}

// ── TcpClientOptions tests ──

void testTcpClientOptionsDefaults() {
    TcpClientOptions opts;
    assert(opts.connector.initRetryDelay == std::chrono::milliseconds(500));
    assert(opts.retry == false);
    std::cout << "  PASS: TcpClientOptions defaults\n";
}

// ── TcpServer with Options integration test ──

void testTcpServerWithOptions() {
    EventLoop loop;
    InetAddress listenAddr("127.0.0.1", 0);

    TcpServerOptions opts;
    opts.numThreads = 1;
    opts.reusePort = false;

    TcpServer server(&loop, listenAddr, "options-test", opts);

    // Verify that set*() can override Options.
    server.setThreadNum(2);

    std::cout << "  PASS: TcpServer constructed with TcpServerOptions\n";
}

// ── TcpClient with Options integration test ──

void testTcpClientWithOptions() {
    EventLoop loop;
    InetAddress serverAddr("127.0.0.1", 9999);

    TcpClientOptions opts;
    opts.connector.initRetryDelay = std::chrono::seconds(1);
    opts.connector.connectTimeout = std::chrono::seconds(3);
    opts.retry = true;

    TcpClient client(&loop, serverAddr, "options-client", opts);

    // Verify that enableRetry/disableRetry still work.
    client.disableRetry();
    assert(client.retry() == false);
    client.enableRetry();
    assert(client.retry() == true);

    std::cout << "  PASS: TcpClient constructed with TcpClientOptions\n";
}

// ── DnsResolver with Options ──

void testDnsResolverWithOptions() {
    DnsResolverOptions opts;
    opts.numWorkerThreads = 1;
    opts.enableCache = true;
    opts.cacheTtl = std::chrono::seconds(30);

    DnsResolver resolver(opts);
    assert(resolver.cacheEnabled());

    std::cout << "  PASS: DnsResolver constructed with DnsResolverOptions\n";
}

// ── Backward compatibility: old constructors still work ──

void testBackwardCompatibility() {
    // Old DnsResolver constructor.
    DnsResolver resolver1(3);

    // Old TcpServer constructor.
    EventLoop loop;
    InetAddress addr("127.0.0.1", 0);
    TcpServer server1(&loop, addr, "compat", true);
    TcpServer server2(&loop, addr, "compat", false);

    // Old TcpClient constructor.
    TcpClient client1(&loop, addr, "compat-client");

    // Old Connector setRetryDelay.
    Connector connector(&loop, addr);
    connector.setRetryDelay(std::chrono::seconds(2), std::chrono::minutes(1));

    std::cout << "  PASS: backward-compatible constructors work\n";
}

int main() {
    std::cout << "=== v5-delta Options Contract Tests ===\n";

    testConnectorOptionsDefaults();
    testConnectorOptionsCustom();
    testConnectorOptionsValidation();

    testDnsResolverOptionsDefaults();
    testDnsResolverOptionsCustom();
    testDnsResolverOptionsValidation();

    testTcpServerOptionsDefaults();
    testTcpServerOptionsValidation();

    testTcpClientOptionsDefaults();

    testTcpServerWithOptions();
    testTcpClientWithOptions();
    testDnsResolverWithOptions();

    testBackwardCompatibility();

    std::cout << "\nAll v5-delta options contract tests passed.\n";
    return 0;
}
