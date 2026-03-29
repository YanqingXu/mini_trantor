# coding_rules.md

## 1. General
- Code must prioritize correctness, clarity, and maintainability
- Avoid over-design in v1
- Public API names should reflect reactor domain concepts clearly
- Implementation should align with corresponding intent file

## 2. Header Rules
- Each core header should begin with a short Chinese explanation block
- Header should declare responsibility clearly
- Avoid leaking unnecessary internal dependencies into headers
- Prefer forward declarations when possible

## 3. Class Design
- One class, one core responsibility
- Avoid “manager” classes without precise domain meaning
- Prefer explicit constructors
- Delete copy when ownership/thread semantics are unclear
- Use move only when semantics are safe and intentional

## 4. Error Handling
- Do not silently ignore critical errors
- Distinguish recoverable vs fatal errors
- Record enough context for debugging
- Do not convert every low-level error into exception throwing in reactor core

## 5. State Modeling
- Prefer enum-based explicit state machines over multiple loosely-coupled bool flags
- State transitions should be narrow and reviewable
- Lifecycle-sensitive state changes should be centralized

## 6. Callback Rules
- Callback execution path should be clear
- Avoid deeply nested callback dispatch logic
- Document callback ordering for lifecycle-sensitive modules
- Avoid invoking user callbacks from ambiguous thread contexts

## 7. Thread Safety
- If a method is loop-thread only, name/document/test it as such
- Cross-thread interaction must go through defined mechanisms
- Do not casually add mutexes to compensate for unclear ownership design

## 8. Comments
- Comments should explain why, not restate obvious code
- Key lifecycle or threading logic should have local comments
- Large design reasoning belongs in intent/rules/docs, not random inline comment blocks

## 9. Testing
- Every public behavior contract should be testable
- Every lifecycle-sensitive module must have contract tests
- Every cross-thread API must have threading-related tests

## 10. AI-Specific Requirement
- Generated code must reference the intent and rules it implements
- Generated code should remain small enough for human audit
- Avoid generating large opaque helper abstractions without explicit intent support