# Intent Template: v5-delta Configuration and Observability

## 1. Intent

Describe which runtime knobs or observability hooks are being made explicit and
why the current hardcoded/default-only behavior is insufficient.

---

## 2. In Scope

- options introduced:
- defaults preserved:
- hook points introduced:
- modules touched:

---

## 3. Non-Responsibilities

- does not turn the library into a framework with hidden global config
- does not add observability paths that break loop-thread rules
- does not replace contract tests with logging assertions

---

## 4. Core Invariants

- configuration remains narrow and local
- hook invocation context is documented
- default behavior remains stable when options are not set

---

## 5. Test Contracts

- unit:
- contract:
- integration:

Suggested files:
- `tests/unit/base/test_logger.cpp`
- `tests/contract/net/test_options_contract.cpp`
- `tests/contract/net/test_metrics_hook_contract.cpp`

---

## 6. Review Questions

- Is this option necessary at the library layer?
- Is callback/hook thread context explicit?
- Did this add diagnosability without obscuring ownership?
