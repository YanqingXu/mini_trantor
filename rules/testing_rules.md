# testing_rules.md

## 1. Testing Philosophy
Tests are not only correctness checks.
They are executable contracts.

## 2. Required Test Categories
Each core module should have:
- unit test
- contract test
- failure-path test
- threading-related test if cross-thread behavior exists

## 3. Unit Test Focus
Unit tests verify:
- local logic
- state transitions
- boundary behavior
- small invariants

## 4. Contract Test Focus
Contract tests verify:
- public API guarantees
- module interaction promises
- lifecycle constraints
- thread-affinity behavior
- callback ordering where relevant

## 5. EventLoop Required Test Examples
- runInLoop executes immediately on same thread
- queueInLoop executes later on loop thread
- cross-thread queueInLoop wakes blocked loop
- quit exits loop safely

## 6. Channel Required Test Examples
- handleEvent dispatches correct callback by revents
- tied owner expired => dangerous callback path blocked
- update/remove workflow respects loop-thread contract

## 7. Poller Required Test Examples
- updateChannel registers correctly
- removeChannel unregisters correctly
- poll returns active channels accurately
- invalid removal path is detected or guarded

## 8. Lifecycle-Sensitive Modules
For lifecycle-sensitive modules, tests should include:
- remove-before-destroy
- callback-after-destroy prevention
- repeated close/error handling guard
- registration state consistency

## 9. AI-Specific Requirement
When generating code, generate tests in the same change set.
No public interface should be added without at least one direct contract assertion.

## 10. Forbidden
- test only happy path for lifecycle-heavy module
- treat coverage as substitute for contract quality
- no-thread test for cross-thread API