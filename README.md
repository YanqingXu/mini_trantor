# mini-trantor

mini-trantor 是一个参考 trantor 思想、以学习和演进为目标的 C++ Reactor 网络库。

它不是单纯“写代码”的项目，而是一个 **Intent 驱动架构** 的实验与工程实践：

- Intent 先行
- Code 作为实现产物
- Tests 作为契约验证
- Diagrams 作为架构解释
- AI 作为受约束的实现协作者

## 当前目标
v1 分三段收口：
- `v1-alpha`：同步 Reactor 主链路稳定
- `v1-beta`：线程模型稳定
- `v1-coro-preview`：协程桥接跑通

## 核心理念
对于重要模块，不先写代码，先写：
1. intent
2. invariants
3. threading rules
4. ownership rules
5. contract tests
6. implementation

## 目录说明
- `intents/`: 设计意图与模块宪法
- `rules/`: 项目级约束规则
- `mini/`: 当前主线实现
- `tests/`: 按 `unit/`、`contract/`、`integration/` 分层的测试入口
- `docs/`: 文档
- `diagrams/`: 架构图
- `tools/ai/`: AI 生成与校验工具

## 核心模块改动闸门
每个核心模块 PR / 改动都必须回答这 5 个问题：
1. 这个模块归属哪个 loop / thread？
2. 谁拥有它，谁释放它？
3. 哪些回调可能重入？
4. 哪些操作允许跨线程，如何投递？
5. 对应哪个测试文件验证？

## 开发顺序
请先阅读：
- `AGENTS.md`
- `intents/architecture/*`
- `rules/*`

再开始生成或修改核心模块。
