# Module Intent: Buffer

## 1. Intent
Buffer is the byte container used by connection read/write paths.
It provides stable readable/writable/prependable regions and a narrow fd I/O bridge
without owning protocol semantics.

---

## 2. Responsibilities
- store inbound and outbound bytes for one connection path
- expose readable/writable/prependable byte accounting
- support append/retrieve operations with predictable index movement
- support scatter-read style growth for efficient socket reads

---

## 3. Non-Responsibilities
- does not own socket or EventLoop
- does not parse framing or protocol boundaries
- does not define backpressure policy

---

## 4. Core Invariants
- readableBytes, writableBytes, and prependableBytes stay internally consistent
- append never invalidates existing readable data semantics
- retrieve moves reader state forward or resets cleanly
- fd read/write helpers report explicit errno on failure

---

## 5. Threading Rules
- Buffer is not internally synchronized
- one connection loop should own mutation of a given Buffer instance

---

## 6. Failure Semantics
- fd I/O failure is reported to caller via return value and saved errno
- growth strategy should stay explicit rather than silently dropping bytes

---

## 7. Test Contracts
- append/retrieve preserve byte ordering
- makeSpace keeps unread data intact
- readFd grows into extra buffer path correctly
- writeFd exposes explicit error reporting

---

## 8. Review Checklist
- Are index transitions still easy to reason about?
- Does growth preserve existing unread bytes?
- Is Buffer remaining protocol-agnostic?
