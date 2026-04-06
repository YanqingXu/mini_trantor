# Module Intent: ConnectionTransport

## 1. Intent
ConnectionTransport encapsulates how one TcpConnection performs transport-level
read, write, handshake, and shutdown operations on its owner EventLoop thread.

Its purpose is to separate transport mechanics from TcpConnection's public
lifecycle model. TcpConnection remains the owner-facing connection object,
while ConnectionTransport hides plain TCP vs TLS differences behind one
loop-owned interface.

---

## 2. Responsibilities
- perform non-blocking transport read into the connection input buffer
- perform non-blocking transport write from the connection output buffer
- perform optional transport handshake steps before the connection becomes
  fully ready
- expose whether the transport currently needs read or write interest
- integrate transport shutdown with the existing TcpConnection close path
- surface transport failures back to TcpConnection as explicit error results

---

## 3. Non-Responsibilities
- does not own EventLoop
- does not own TcpConnection lifecycle or public connection state
- does not invoke TcpServer/TcpClient close-removal logic directly
- does not parse application protocols
- does not bypass TcpConnection callback ordering

---

## 4. Core Invariants
- one ConnectionTransport belongs to exactly one TcpConnection and one owner loop
- all transport operations run on the owner loop thread
- transport failure never creates a parallel teardown path; it reports back to
  TcpConnection's normal error/close convergence path
- plain TCP remains the zero-extra-behavior baseline
- TLS mode preserves non-blocking semantics and does not block the loop thread

---

## 5. Collaboration
- TcpConnection owns ConnectionTransport as a loop-owned helper
- TcpConnection asks ConnectionTransport to read, write, handshake, and shutdown
- TlsContext may supply TLS configuration used by a TLS-capable transport mode
- Channel interest changes still occur through TcpConnection/Channel on the owner loop

---

## 6. Threading Rules
- start/upgrade, handshake, read, write, and shutdown execute on owner loop thread only
- cross-thread callers never touch ConnectionTransport directly; they go through
  TcpConnection APIs that marshal back into the loop
- transport-reported WANT_READ/WANT_WRITE style needs must not mutate Channel
  registration off-thread

---

## 7. Ownership Rules
- TcpConnection owns ConnectionTransport
- ConnectionTransport may own transport-private state such as SSL* for TLS mode
- shared transport configuration objects (for example TlsContext) may be kept via
  shared_ptr when required by transport-private state
- transport-private state must be released before TcpConnection destruction completes

---

## 8. Failure Semantics
- transport read/write fatal errors report an explicit error back to TcpConnection
- handshake failure reports error and converges on the normal close path
- transport shutdown failure does not prevent final connection cleanup
- plain TCP and TLS transport both preserve idempotent close/error handling

---

## 9. Extension Points
- plain TCP transport implementation
- TLS transport implementation
- future metrics hooks for bytes read/written or handshake timing

---

## 10. Test Contracts
- plain transport read/write behavior remains unchanged
- TLS transport handshake completes on the owner loop thread
- TLS transport WANT_READ/WANT_WRITE behavior does not break event interest updates
- transport failure still triggers TcpConnection error -> close convergence
- transport shutdown integrates with existing disconnect behavior

---

## 11. Review Checklist
- Does transport logic stay off the public TcpConnection state machine?
- Can transport failure only tear down through TcpConnection?
- Are all transport-private resources released before connection destruction ends?
- Does the plain TCP path remain simple and unsurprising?
