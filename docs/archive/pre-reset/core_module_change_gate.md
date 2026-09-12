# 核心模块改动闸门

这份清单用于把 “Intent 驱动” 接进日常开发流水线。

适用范围：
- `EventLoop`
- `Channel`
- `Poller` / `EPollPoller`
- `Buffer`
- `Acceptor`
- `TcpConnection`
- `TcpServer`
- `EventLoopThread`
- `EventLoopThreadPool`
- coroutine bridge 相关代码

## 必填问题

每个核心模块 PR / 直接改动说明都必须回答：

1. 这个模块归属哪个 loop / thread？
2. 谁拥有它，谁释放它？
3. 哪些回调可能重入？
4. 哪些操作允许跨线程，如何投递？
5. 对应哪个测试文件验证？

## 推荐模板

```md
## Intent Reference
- intents/...:
- rules/...:
- stage: v1-alpha | v1-beta | v1-coro-preview

## Core Module Change Gate
1. Loop / Thread:
2. Ownership / Release:
3. Re-entrant Callbacks:
4. Cross-thread Operations:
5. Test File Mapping:
```

## 使用要求

- 缺少这 5 个问题的答案，不应合并核心模块改动。
- 如果改动触及线程模型、生命周期或回调顺序，答案必须同步更新到测试和文档。
- `Test File Mapping` 不能只写“已测试”，必须给出具体文件路径。
