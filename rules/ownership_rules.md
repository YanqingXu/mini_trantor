# ownership_rules.md

## 1. Core Principle
Ownership must be explicit.
A module should either:
- own an object
- borrow an object
- observe an object
It must not blur these roles.

## 2. EventLoop
- EventLoop owns its Poller
- EventLoop owns wakeup-related resources it creates
- EventLoop does not own business-layer connection semantics

## 3. Poller
- Poller does not own Channel
- Poller only maintains registration/mapping relationship
- Poller backend state must not outlive EventLoop

## 4. Channel
- Channel does not own fd by default
- Channel belongs logically to one EventLoop
- Channel may observe an upper-layer owner through tie/weak_ptr mechanism
- Channel callback dispatch must respect observed owner lifetime

## 5. TcpConnection
- TcpConnection owns its input/output buffer members
- TcpConnection does not own EventLoop
- TcpConnection lifecycle should be coordinated through shared ownership where needed
- Callback-triggered lifetime risks must be explicitly guarded

## 6. Timer / Scheduled Tasks
- Timer containers own timer metadata
- Scheduled callbacks do not imply ownership of arbitrary target objects
- Cancellation semantics must be explicit

## 7. Cross-Layer Rule
- Lower reactor layers do not own higher business objects
- Higher layers may own reactor-layer wrappers, but their destruction path must respect thread/lifecycle rules

## 8. Destruction Rule
- Destruction of lifecycle-sensitive objects must not violate owner-thread assumptions
- “remove before destroy” must be enforced where registration exists
- No object should remain registered in Poller after its effective destruction path begins

## 9. Forbidden
- Implicit transfer through raw pointer handoff with no documented owner
- Shared ownership used as a substitute for lifecycle design
- Poller owning Channel
- Channel owning EventLoop