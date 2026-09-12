# Module Intent: LoopHandle

## Intent
Provide a copyable, non-owning way for a producer to queue work without retaining
a raw EventLoop pointer beyond that loop's lifetime. DNS workers are the first
consumer. This is a posting boundary, not an executor or an owner of the loop.

## Public interface and responsibilities
- EventLoop::handle() snapshots a LoopHandle while the EventLoop is still alive.
- LoopHandle::queue(Functor) always queues, never invokes the callback inline.
- It returns true if enqueued, false if the handle is empty or its loop has closed.
- The same handle can be used by concurrent producers; moving/assigning that
  handle object itself still needs ordinary external synchronization.

## Non-responsibilities
- Does not retain EventLoop, Channel, connection, timer or coroutine frames.
- Does not create threads, migrate callbacks, run fallback code on a caller
  thread, cancel arbitrary operations, or wait for posted callbacks to finish.
- Does not make an already-dangling raw pointer safe when acquiring handle().

## Invariants and ownership
1. Shared posting metadata stores a borrowed loop pointer under a mutex.
2. queue holds that mutex through enqueue and wakeup; loop teardown invalidates
   the pointer under the same mutex before releasing descriptors or other state.
3. loop() closes posting when its final pending queue is empty, atomically with
   respect to handle posting. A post cannot be accepted after that final check.
4. Already accepted work is drained when loop() runs, including quit's nested
   cleanup work. Destroying a never-run loop discards its unexecuted callbacks.
5. Destruction remains owner-thread-only and is forbidden inside running loop().
6. Metadata does not own a frame. Owners must release/cancel network tasks on
   their owner loop before destroying the loop; rejected posts cannot resume them.

## Threading and collaboration
LoopHandle posting and invalidation share a posting mutex. Whenever both posting
and the pending-queue mutex are needed, acquire posting first. Never invoke user
code while either mutex is held. EventLoop retains normal runInLoop/queueInLoop
semantics; this handle adds explicit rejection for long-lived external producers.

DnsResolver snapshots the handle before publishing an operation. Failed late
posts abandon that operation without executing its callback on another thread.
This removes worker/cancellation dereferences of a destroyed callbackLoop. The
caller still controls captured-resource lifetimes and orderly task cleanup.

## Failure semantics
- Empty callbacks are invalid arguments.
- Empty/closed handles reject valid callbacks without invoking them.
- Allocation failure propagates to the posting caller.
- Accepted means enqueued, not completed; no loop lifetime extension is implied.
- A closed handle stays closed; this is consistent with the current one-shot loop.

## Extension points
Use the same posting boundary for other external producers only when their own
operation cancellation/teardown contracts have been defined. Queue capacity and
quitting admission policy remain S2 work; no new backend or scheduling policy.

## Test contracts and review checklist
- test_loop_handle: default/expired rejection, owner-thread execution, posts
  queued before loop(), nested drain, rejection after loop() returns, and
  concurrent posting against owner-thread destruction under ASan/TSan.
- test_dns_lifetime: loop closes before worker completion/cancellation; a
  destroyed resolve frame never resumes. Existing DNS results remain explicit.
- Does any use of the borrowed pointer escape the posting mutex?
- Are callbacks and capture destruction kept outside the posting mutex?
- Does queue acceptance linearize with the final drain check?
- Are loop/frame ownership rules still explicit?
