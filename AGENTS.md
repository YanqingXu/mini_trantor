# AGENTS.md

## Session Handoff
- At the start of a new session, read [docs/HANDOFF.md](docs/HANDOFF.md) for the
  unfinished goal, last verified state, next task, and local environment recovery.
- Recheck the current user request, goal status, Git state, and CI before continuing.
  A handoff records paused work; it does not itself resume that work.
- Treat proposed interfaces and tests in the handoff as unimplemented unless the
  current repository proves otherwise. Keep `docs/roadmap.md` as the stage authority.
- Update the handoff after completing a bounded task or changing the next step.

---

## Project Identity
mini-trantor is a small but industrial-style C++ reactor network library inspired by trantor.
It is designed for learning, evolution, and AI-assisted development.

This project is NOT only a codebase.
It is an intent-driven system where:
- intent is first-class
- code is derived artifact
- tests are contract enforcement
- diagrams are architecture explanation
- documentation is generated understanding

---

## Primary Goals
1. Build a minimal but correct reactor-style network library
2. Preserve thread-affinity correctness
3. Preserve ownership clarity and lifecycle safety
4. Make coroutine integration possible without breaking reactor semantics
5. Make the codebase friendly to AI-assisted generation and review

---

## Development Principle
Never start from code first when a module is important.
Start from:
1. intent
2. invariants
3. threading rules
4. ownership rules
5. contracts
6. tests
7. implementation

---

## Temporary Artifacts
- Store agent-created temporary artifacts under the repository's `.tmp/` directory.
- Use subdirectories such as `.tmp/build/`, `.tmp/logs/`, and `.tmp/probes/` for
  temporary builds, logs, generated probes, scratch scripts, and archives.
- Store temporary commit/PR descriptions under `.tmp/` as well.
- Do not scatter `build_*` files or directories, logs, or scratch scripts in the
  project root. Keep `.tmp/` ignored by Git and do not commit its contents.

---

## Must Follow
- Always read the relevant intent file before generating or modifying code
- Always obey thread-affinity rules
- Always obey ownership rules
- Always generate tests for public contracts
- Always document lifecycle-sensitive modules with state/sequence diagrams
- Always answer the core-module change gate questions in PR/change descriptions
- Prefer explicit state machines over scattered boolean flags
- Prefer narrow, clear responsibilities over large “god classes”

---

## Forbidden
- Hidden ownership transfer
- Shared mutable state without explicit synchronization or single-thread ownership
- Business logic mixed into reactor core
- Coroutine abstraction that bypasses EventLoop scheduling semantics
- Public APIs without contract tests
- Changes to lifecycle-sensitive code without updating related intent/rules/docs

---

## Module Design Standard
For each core module, define:
- Intent
- Responsibilities
- Non-responsibilities
- Invariants
- Collaboration
- Threading rules
- Failure semantics
- Extension points
- Test contracts
- Review checklist

---

## AI Generation Workflow
When asked to generate a new module:
1. read corresponding intent file
2. read rules/*
3. propose public interface
4. propose invariants and state transitions
5. generate .h
6. generate .cc
7. generate unit tests
8. generate contract tests
9. generate architecture diagram
10. generate Chinese explanation header

---

## Review Workflow
Review in this order:
1. intent correctness
2. contract correctness
3. invariants
4. threading correctness
5. ownership correctness
6. lifecycle correctness
7. code details
8. test completeness

Do not start by reading implementation details first for complex modules.

---

## Core Module Change Gate
Each PR or direct change touching a core module must answer:
1. Which loop/thread owns this module?
2. Who owns it and who releases it?
3. Which callbacks may re-enter?
4. Which operations are allowed cross-thread, and how are they marshaled?
5. Which test file verifies this change?

---

## Coding Style Direction
- modern C++20/23 style where appropriate（使用 `std::expected`、`std::coroutine_handle`、concepts 等 C++20/23 特性）
- keep public interfaces small
- use RAII
- avoid over-template design in v1
- avoid premature optimization that harms clarity
- prefer correctness before cleverness

---

## Current Development Scope (2026-09 reset)
- Read `intents/architecture/reactor_scope_reset.intent.md` and `docs/roadmap.md` first.
- S0: scope reduction and trustworthy build/test/install boundaries.
- S1: coroutine frame lifetime, suspension publication, DNS teardown/re-entry,
  thread startup/shutdown and callback lifecycle correctness before new features.
- S2: minimal TCP contracts and explicit Linux/Windows support boundaries.
- S3: reproducible load evidence before a stable release claim.
- TimerQueue and basic backpressure remain necessary TCP runtime support.
- Game/session/AOI, HTTP/WebSocket/RPC, custom KCP/UDP/PMTU/FEC and business
  metrics exporters are retired from this library, not optional future core work.
- Historical version plans in `docs/archive/` do not authorize feature expansion.
- Keep all retained API tests; never retire a failing test for an API still supported.


## When the user asks to analyze a framework / understand a project / generate source-code reading documentation

The agent must automatically load and apply:

- `rules/framework-understanding-doc.md`

The response must prioritize producing a complete **Framework Understanding Document** rather than a short summary.

If the project is large:
- first provide a high-level overview
- then expand into directory-by-directory and file-by-file analysis

If the user explicitly asks for a **very detailed** explanation:
- cover all core directories
- cover all core files
- provide a recommended reading order
- explain the main call chains and extension points
