# TlsContext 源码拆解

## 1. 类定位

* **角色**: 高级功能模块（Advanced）
* **层级**: Net 层辅助设施，封装 OpenSSL
* TlsContext 是 **TLS 上下文工厂**——封装 `SSL_CTX` 的创建和配置，构造后为只读对象，可安全跨线程共享

## 2. 解决的问题

* **核心问题**: 将 OpenSSL 的 `SSL_CTX` 初始化、证书加载、验证配置封装为安全的 RAII 对象
* 如果没有 TlsContext:
  - 每个 TcpConnection 都要直接操作 OpenSSL API
  - SSL_CTX 的初始化/销毁容易出错（忘记 free、重复 init）
  - 证书配置代码散落在各处

## 3. 对外接口（API）

| 方法 | 场景 | 调用者 |
|------|------|--------|
| `newServerContext(certPath, keyPath)` | 创建服务端 TLS 上下文 | TcpServer |
| `newClientContext()` | 创建客户端 TLS 上下文 | TcpClient |
| `setCaCertPath(caFile, caPath)` | 设置 CA 证书路径 | 用户 |
| `setVerifyPeer(verify)` | 开启/关闭对端证书验证 | 用户 |
| `nativeHandle()` | 获取底层 SSL_CTX* | TcpConnection 内部 |

### 工厂模式

* 构造函数为 private，只能通过 `newServerContext` / `newClientContext` 创建
* 返回 `shared_ptr<TlsContext>`，支持多连接共享

## 4. 核心成员变量

```
TlsContext
├── ctx_: SSL_CTX*    // OpenSSL 上下文指针（唯一管理）
```

* 析构时调用 `SSL_CTX_free(ctx_)`
* 不可拷贝（deleted copy ctor/assign）

## 5. 执行流程

### 5.1 服务端上下文创建

```
TlsContext::newServerContext(certPath, keyPath)
  → ensureOpenSslInit()                           // 全局初始化（once）
  → SSL_CTX_new(TLS_server_method())              // 创建 SSL_CTX
  → SSL_CTX_set_min_proto_version(TLS1_2_VERSION) // 最低 TLS 1.2
  → SSL_CTX_use_certificate_chain_file(certPath)  // 加载证书链
  → SSL_CTX_use_PrivateKey_file(keyPath, PEM)     // 加载私钥
  → SSL_CTX_check_private_key()                   // 校验证书-私钥匹配
  → return shared_ptr<TlsContext>(new TlsContext(ctx))
```

### 5.2 客户端上下文创建

```
TlsContext::newClientContext()
  → ensureOpenSslInit()
  → SSL_CTX_new(TLS_client_method())
  → SSL_CTX_set_min_proto_version(TLS1_2_VERSION)
  → SSL_CTX_set_default_verify_paths()            // 加载系统默认 CA
  → return shared_ptr<TlsContext>(new TlsContext(ctx))
```

### 5.3 TcpConnection 使用 TlsContext

```
TcpServer/TcpClient 持有 shared_ptr<TlsContext>
  → 新连接建立时:
    → SSL_new(tlsContext->nativeHandle())  // 为每个连接创建 SSL 对象
    → SSL_set_fd(ssl, connFd)
    → SSL_accept() / SSL_connect()         // 握手
```

## 6. 关键交互关系

```
┌────────────┐  newServerContext   ┌─────────────┐
│ TcpServer  │───────────────────▶│ TlsContext  │
└────────────┘                    │ (shared_ptr)│
                                  └──────┬──────┘
┌────────────┐  newClientContext          │ nativeHandle()
│ TcpClient  │───────────────────▶       │
└────────────┘                           ▼
                                  ┌─────────────┐
                                  │ SSL_CTX     │  (OpenSSL)
                                  └──────┬──────┘
                                         │ SSL_new
                                         ▼
                                  ┌─────────────┐
                                  │ SSL object  │  (per connection)
                                  └─────────────┘
```

* **上游**: TcpServer（服务端），TcpClient（客户端）
* **下游**: OpenSSL SSL_CTX API
* **使用方**: TcpConnection 通过 `nativeHandle()` 获取 SSL_CTX 创建 per-connection SSL 对象

## 7. 关键设计点

### 7.1 全局初始化保护

```cpp
struct OpenSslInitializer {
    OpenSslInitializer() {
        OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);
    }
};
void ensureOpenSslInit() {
    static OpenSslInitializer init;  // Meyer's singleton
}
```

* OpenSSL 初始化只能执行一次
* 使用 static local + 构造函数保证线程安全

### 7.2 强制 TLS 1.2+

```cpp
SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
```

* 禁止 SSLv3, TLS 1.0, TLS 1.1（已知不安全）

### 7.3 工厂 + shared_ptr

* 构造失败时 throw runtime_error，不会泄漏半初始化的 SSL_CTX
* 每一步失败都先 `SSL_CTX_free(ctx)` 再 throw
* 返回 `shared_ptr` 允许多个 TcpConnection 共享同一上下文

### 7.4 构造后只读

* `newServerContext` / `newClientContext` 完成所有配置
* `setCaCertPath` / `setVerifyPeer` 是可选的后续配置
* 之后通过 `nativeHandle()` 只读访问 → 线程安全

## 8. 潜在问题

| 问题 | 描述 | 严重程度 |
|------|------|----------|
| setCaCertPath 非线程安全 | 配置阶段调用，不应在连接建立后调用 | 低 |
| 无 SNI 支持 | 缺少 SSL_CTX_set_tlsext_servername_callback | 低 |
| 无 ALPN 配置 | HTTP/2 需要 ALPN negotiation | 低（v1 不需要） |
| 错误信息仅用 ERR_get_error | 多线程下 OpenSSL error queue 可能混乱 | 低 |

## 9. 面试角度总结

| 问题 | 答案要点 |
|------|----------|
| 为什么用工厂方法而不是构造函数？ | 创建可能失败（OpenSSL 初始化、证书加载），工厂方法可以安全清理后抛异常 |
| SSL_CTX 和 SSL 的关系？ | SSL_CTX 是全局配置（共享），SSL 是 per-connection 对象 |
| 为什么强制 TLS 1.2+？ | TLS 1.0/1.1 有已知安全漏洞，现代最佳实践 |
| TlsContext 是否线程安全？ | 构造后只读，SSL_CTX 本身在 OpenSSL 1.1+ 中读操作线程安全 |
