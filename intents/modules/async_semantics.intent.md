# Module Intent: Unified Async Semantics

## 1. Intent

Unified async semantics define how coroutine-facing asynchronous APIs in
mini-trantor model:

- success
- peer-driven completion
- timeout
- active cancellation
- transport/runtime failure

This intent is broader than any single return type.
It is the contract layer that decides which outcomes exist, how callers
distinguish them, and which outcomes are considered normal completion vs error.

`std::expected<T, E>` is a preferred carrier for many fallible coroutine APIs,
but it is not itself the semantic model.

---

## 2. Responsibilities

- define a shared vocabulary for asynchronous outcomes
- eliminate ambiguous signaling such as:
  - `bool` with undocumented meaning
  - empty container meaning failure
  - exception in one module, sentinel in another, `expected` in a third
- provide a default result-carrier direction for coroutine-facing APIs
- preserve EventLoop scheduling semantics across success, close, timeout,
  cancellation, and error paths

---

## 3. Non-Responsibilities

- does not invent a standalone scheduler
- does not replace module-local invariants for transport or lifecycle
- does not require every API to use the exact same C++ type if the semantic
  surface remains explicit and consistent
- does not erase the difference between cancellation and runtime failure

---

## 4. Semantic Model

The unified async model should let callers distinguish at least:

- success
- peer close
- timeout
- active cancel
- transport/runtime failure
- invalid state / not connected

These are semantic outcomes, not necessarily one enum forever.

Current project direction:
- for value-returning coroutine operations, prefer `std::expected<T, E>`
- for void-like coroutine operations, prefer `std::expected<void, E>`
- avoid using empty containers, empty strings, or plain `bool` as the sole
  failure/cancellation signal when the meaning is not self-describing

---

## 5. Carrier Direction

Preferred default carrier for fallible coroutine-facing APIs:

```cpp
std::expected<T, AsyncError>
```

Where appropriate:
- `T = std::string` for `asyncReadSome`
- `T = void` for `asyncWrite`
- `T = std::vector<InetAddress>` for async resolve

Exceptions may still be used at higher-level domain APIs such as RPC if they
form an intentionally different public contract, but lower-level transport and
bridge layers should not mix multiple ad-hoc signaling styles.

---

## 6. Cancellation Direction

Cancellation is not identical to transport failure.

Cancellation should:
- be explicit
- preserve owner-loop resume semantics
- resume suspended coroutines exactly once
- compose with timeout and close paths
- not be encoded as “looks like success but with empty data”

Timeout is also not identical to cancellation.
If timeout is implemented by cancellation, the user-visible result still needs
to remain distinguishable from user-initiated cancel.

---

## 7. Collaboration

This intent applies to:

- `TcpConnection` awaitables
- `ConnectionAwaiterRegistry`
- `SleepAwaitable`
- `ResolveAwaitable`
- `DnsResolver` callback/awaitable bridge
- `WhenAny` timeout-race style composition

It may later influence:

- RPC coroutine bridge
- HTTP client awaitables
- future signal/shutdown awaitables

---

## 8. Threading Rules

- semantic outcome production still happens on the owner EventLoop thread
- cross-thread cancel requests must marshal back into the owner loop
- close/error/cancel/timeout must not create direct off-thread coroutine resume
- result delivery must remain consistent with existing one-loop-per-thread rules

---

## 9. Ownership Rules

- outcome state shared between callbacks and awaiters must remain explicitly owned
- coroutine handles are borrowed for resume and must be resumed exactly once
- cancellation sources/tokens must not hide cross-thread mutable state without
  explicit synchronization

---

## 10. Failure Semantics

- peer close is explicit, not silently converted to success
- timeout is explicit, not silently converted to generic cancel
- active cancel is explicit, not silently converted to peer close
- invalid-state usage remains explicit
- transport/runtime failure remains explicit

Ambiguous patterns to avoid:
- empty vector means DNS failure
- `false` means cancelled unless the reader already knows that convention
- exception in one bridge layer and sentinel in another without contract reason

---

## 11. Test Contracts

- the same major async outcomes are distinguishable across awaitable families
- timeout race patterns do not leak coroutine handles
- cancel/close/error paths resume suspended awaiters exactly once
- cross-thread cancel still resumes on the owner loop
- DNS failure is explicit and does not rely on empty-result ambiguity

---

## 12. Review Checklist

- Is this a semantic improvement or only a carrier-type change?
- Can the caller distinguish timeout, cancel, peer close, and runtime failure?
- Does the change preserve owner-loop resume semantics?
- Did this remove ambiguity or merely relocate it?
