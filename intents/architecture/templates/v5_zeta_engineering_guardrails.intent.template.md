# Intent Template: v5-zeta Engineering Guardrails

## 1. Intent

Describe which project-level safety or verification mechanism is being added and
which regression class it is meant to catch earlier.

---

## 2. In Scope

- CI jobs:
- sanitizer jobs:
- fuzz targets:
- benchmark targets:
- install/package verification:

---

## 3. Non-Responsibilities

- does not alter runtime semantics
- does not replace contract quality with raw coverage numbers
- does not hide failing tests behind optional paths

---

## 4. Required Checks

- build succeeds from clean checkout
- tests run under intended jobs
- install and `find_package` path are validated
- lifecycle-sensitive modules have sanitizer coverage

---

## 5. Validation

- workflows added:
- commands run:
- remaining blind spots:

---

## 6. Review Questions

- What new class of regressions will this guardrail catch?
- Is the new guardrail cheap enough to keep running continuously?
- Which high-risk modules are still not covered?
