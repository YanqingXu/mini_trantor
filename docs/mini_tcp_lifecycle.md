# mini Tcp 生命周期说明

本说明记录本轮修复后，`TcpServer` 与 `TcpConnection` 的关键生命周期约束。

## 状态图

```mermaid
stateDiagram-v2
    [*] --> Connecting
    Connecting --> Connected: connectEstablished()
    Connected --> Disconnecting: shutdown()
    Connected --> Disconnected: handleClose()/handleError()
    Disconnecting --> Disconnected: handleClose()/handleError()
    Disconnected --> [*]
```

## 关闭路径时序

```mermaid
sequenceDiagram
    participant BaseLoop as TcpServer(base loop)
    participant IoLoop as TcpConnection(loop)
    participant Conn as TcpConnection

    IoLoop->>Conn: handleClose()/handleError()
    Conn->>Conn: disableAll + state=Disconnected
    Conn->>BaseLoop: closeCallback(removeConnection)
    BaseLoop->>BaseLoop: erase from map
    BaseLoop->>IoLoop: queue connectDestroyed()
    IoLoop->>Conn: connectDestroyed()
    Conn->>Conn: remove channel before destruction
```

## Idle Timeout 路径时序

```mermaid
sequenceDiagram
    participant IoLoop as TcpConnection(loop)
    participant Timer as TimerQueue
    participant Conn as TcpConnection
    participant BaseLoop as TcpServer(base loop)

    Timer->>IoLoop: idle timeout callback
    IoLoop->>Conn: forceClose()
    Conn->>Conn: handleClose()
    Conn->>BaseLoop: closeCallback(removeConnection)
    BaseLoop->>BaseLoop: erase from map
    BaseLoop->>IoLoop: queue connectDestroyed()
```

## 当前约束
- `TcpServer` 的延迟移除回调必须先验证 server 生命周期令牌，不能默认假设 `this` 还活着。
- `TcpConnection` 的 awaiter 只允许在 owner loop 线程直接观察内部状态；跨线程调用必须先 marshal 回 loop。
- fatal read/write error 必须走统一关闭路径，避免“仅打印错误但仍保持注册”的半坏状态。
- idle timeout 只是 close 的触发来源之一，不能绕开 `handleClose() -> removeConnection() -> connectDestroyed()` 这条主路径。
