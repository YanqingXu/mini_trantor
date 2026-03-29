# review_rules.md

## 1. Review Order
For important modules, review in this order:
1. intent
2. public contract
3. invariants
4. thread-affinity correctness
5. ownership correctness
6. lifecycle correctness
7. failure semantics
8. implementation details
9. tests

## 2. Review Focus
Review should ask:
- Is the module solving the right problem?
- Are boundaries clear?
- Are invariants explicit?
- Are thread rules preserved?
- Is destruction safe?
- Are test contracts sufficient?

## 3. High-Risk Areas
The following areas always require focused review:
- cross-thread scheduling
- callback dispatch
- remove-before-destroy logic
- registration consistency
- state transitions
- coroutine suspend/resume integration points

## 4. PR Standard
Each PR for a core module should ideally contain:
- intent reference
- public interface
- implementation
- tests
- diagram/doc updates if lifecycle-sensitive

## 5. Review Checklist Example
- Does this change violate existing intent?
- Does it add hidden ownership?
- Does it create a non-owner-thread mutation path?
- Does it break callback ordering?
- Does it weaken remove-before-destroy discipline?
- Does it require updating docs/tests/diagram?

## 6. Forbidden
- review only code diff without intent context
- approve complex lifecycle changes without tests
- approve thread-affinity changes without explicit rule update