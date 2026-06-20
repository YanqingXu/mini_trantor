## Intent Reference
- intents/...:
- rules/...:
- stage: `v1-alpha` / `v1-beta` / `v1-coro-preview` / `v5-zeta` / `v6-alpha`

## Module State Impact
- [ ] Stable module touched
- [ ] Beta module touched
- [ ] Experimental module touched
- Notes:

## Core Module Change Gate
1. 这个模块归属哪个 loop / thread？
2. 谁拥有它，谁释放它？
3. 哪些回调可能重入？
4. 哪些操作允许跨线程，如何投递？
5. 对应哪个测试文件验证？

## Tests
- [ ] `unit`
- [ ] `contract`
- [ ] `integration`
- [ ] `lifecycle`
- [ ] `threading`
- [ ] `protocol`
- Evidence:

## Engineering Guardrails
- [ ] ASan / UBSan considered or run
- [ ] TSan considered or run for cross-thread behavior
- [ ] Fuzz considered or run for protocol/parser changes
- [ ] Benchmark considered or run for performance-sensitive changes
- [ ] install + find_package path unaffected
- Evidence:

## Docs / Diagram
- [ ] intent updated
- [ ] rules updated
- [ ] docs/diagram updated if lifecycle-sensitive
