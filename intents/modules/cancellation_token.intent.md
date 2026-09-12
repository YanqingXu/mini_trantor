# Module Intent: CancellationToken / Source / Registration

## Intent and responsibilities
Provide cooperative cancellation state and registration ownership. Source requests
cancellation, Token observes it, Registration removes an unclaimed callback.
This module owns callback metadata, never EventLoop, coroutine frames or connections.

## Invariants and state
- State is Active -> Cancelled, once. Each stored callback is either Removed by its
  registration or Claimed by the first cancel call; a claimed callback is invoked once.
- Late registration invokes immediately on the registering thread. Cancellation itself
  invokes on the requesting thread; network awaitables must marshal to their owner loop.
- No callback invocation OR destruction of its captures occurs under the state mutex.
- reset removes only unclaimed work; it neither waits for nor suppresses already claimed
  callbacks. Captures must use lifetime guards where callbacks may race owner cleanup.
- A registration object is move-only and needs exclusive access for reset/move/destruction.
  Distinct registrations/tokens/sources sharing state may be used concurrently.
- A moved-from source is inert: its token is empty, query is false, cancel is a no-op.

## Collaboration and ownership
Sources/tokens share state. Registrations hold state until reset/destruction. The first
cancel call transfers the callback container to its stack, unlocks, then invokes/releases.
reset extracts a callback node under the mutex and destroys the node after unlocking.
User callback captures may register, cancel or release other registrations reentrantly.
State sharing does not permit concurrent mutation of the same source/token wrapper.

## Failure semantics
Cancellation is terminal even if observers throw. Invoke every claimed callback, retain
the first exception in the unspecified callback order, then rethrow it to cancel's caller.
Repeated cancel is a no-op. Late-registration exceptions propagate to its caller directly.
Callback capture destructors follow ordinary C++ noexcept destruction requirements.

WhenAny must still request every loser cancellation and resume its parent if an observer
throws. Preserve an existing winner exception; otherwise report the first cancellation
exception through the parent Task. This differs from a losing Task's own stored exception,
which does not replace the winner. Cancellation remains cooperative, not forced destruction.

## Extension points and contracts
registerCallback is the synchronous observer boundary; no scheduler or executor is added.
`tests/contract/coroutine/test_cancellation_reentry.cpp` verifies capture destruction
reentry, observer exception fanout, late registration, inert moved-from sources and
WhenAny value/void winner publication despite cancellation failure.
Existing cancellation/sleep/DNS/TCP and combinator contracts remain enabled.

## Review checklist
- Are callbacks and capture destruction outside locks?
- Does failure still notify every claimed observer and publish a terminal result?
- Are callback thread and frame/connection ownership explicit?
- Does registration reset avoid implying a completion barrier it does not provide?
