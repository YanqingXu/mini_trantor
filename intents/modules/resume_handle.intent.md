# Module Intent: guarded coroutine resumption

## Intent
ResumeHandle lets timer/I/O/resolve/combinator callbacks borrow a Task frame
without retaining a right to resume it after its owner destroys it. It protects
the frame boundary; operation registration and EventLoop scheduling remain separate.

## Responsibilities and non-responsibilities
- FrameControl stores a borrowed handle and a shared execution gate.
- ResumeHandle locks the gate, verifies the handle is still valid, then resumes.
- Task and Task::Awaiter invalidate the control before destroying their frame.
- No EventLoop ownership, executor, background thread, cancellation policy or
  implicit frame ownership is introduced. Queued callbacks retain metadata only.

## Invariants
1. A destroyed frame is invalidated before any frame locals/awaiters are destroyed.
2. A tracked resume and owner destruction cannot execute concurrently.
3. Task-to-Task awaited chains share one recursive gate, including symmetric
   continuation transfer; parent destruction cannot race a running child.
4. Detached combinator wrappers have independent frame owners but share their
   parent's gate. They hold a guarded parent resume token, never its frame.
   Sharing prevents opposing parent-to-child start / child-to-parent finish locks.
5. Combinator await_suspend moves launch state to ordinary local variables before
   starting any child. Immediate completion may destroy the awaiting frame.
6. Awaitable registration/destruction still obeys its owner EventLoop. A gate does
   not authorize off-thread Channel/timer/awaiter mutation.

## Ownership and threading rules
Task/Task::Awaiter/detached final suspend remain the only frame owners. The promise
and callbacks may share FrameControl metadata. A control block never destroys a
frame implicitly. The execution gate is a recursive mutex shared by an awaited
Task/combinator tree, not by all tasks or all EventLoops. Coroutine body execution
inside that tree is serialized, while its EventLoops and I/O remain independent.
Weak child metadata allows quiescent adoption to migrate an already-started tree;
it does not retain completed frames. Child links are pruned on subsequent adoption.

Access/move/adoption of the same Task object still requires external synchronization.
Adopting an already-started Task tree requires all descendants to be quiescent and
must obey each active network awaitable's current owner-loop contract.
Callbacks use ResumeHandle; custom awaitables that publish raw handles remain
responsible for external synchronization and unregistration.

## Failure semantics
Late tracked callbacks are no-ops after invalidation. Existing duplicate-registration
errors are unchanged. Destroying a currently executing, non-final frame from inside
its own awaited chain is invalid and fails fast; the gate is not deferred ownership.
Code inside await_suspend must not block waiting for its own continuation to finish.

## Collaboration and extension points
Task exposes frame metadata to typed await_suspend through its promise. Sleep,
TCP, ResolveAwaitable and combinators construct borrowed ResumeHandles before
publishing callbacks. Non-Task promises can use the untracked fallback only with
their own lifetime contract. The guard does not replace operation terminal states.

## Test contracts
- test_combinator_lifetime: destroy pending whenAll/whenAny parents, synchronous
  winner inside a detached parent, and cross-loop completion vs parent destruction.
- Existing sleep/TCP pending-destruction and queued-cancel contracts continue to pass.
- Task start/detach/adoption and move-only Awaiter contracts continue to pass.
- ASan/UBSan verifies no stale frame access; TSan checks concurrent completion.

## Review checklist
- Does every built-in asynchronous resume use a tracked handle when the promise is Task?
- Is invalidation performed before frame destruction and under the same gate?
- Does child adoption preserve symmetric-transfer protection without opposing lock order?
- Do launch locals survive synchronous parent completion?
- Are owner-loop rules and the cost of added metadata/locking documented honestly?
