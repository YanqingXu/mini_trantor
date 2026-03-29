# mini-trantor

mini-trantor 是一个参考 trantor 思想、以学习和演进为目标的 C++ Reactor 网络库。

它不是单纯“写代码”的项目，而是一个 **Intent 驱动架构** 的实验与工程实践：

- Intent 先行
- Code 作为实现产物
- Tests 作为契约验证
- Diagrams 作为架构解释
- AI 作为受约束的实现协作者

## 当前目标
v1 聚焦：
- Channel
- Poller
- EPollPoller
- EventLoop
- wakeup
- queueInLoop / runInLoop
- 单元测试
- 契约测试

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
- `src/`: 实现代码
- `tests/`: 单元/契约/集成/压力测试
- `docs/`: 文档
- `diagrams/`: 架构图
- `tools/ai/`: AI 生成与校验工具

## 开发顺序
请先阅读：
- `AGENTS.md`
- `intents/architecture/*`
- `rules/*`

再开始生成或修改核心模块。