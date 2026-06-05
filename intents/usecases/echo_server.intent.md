# Usecase Intent: Echo Server

## 1. Intent
Build a minimal echo server on top of mini-trantor to validate:
- accept path
- connection creation
- read callback
- write/send path
- connection teardown

## 2. Why It Exists
The echo server is the first full-path integration usecase.
It validates whether the reactor core can support a real connection lifecycle.

## 3. Scope
- single-thread v1 acceptable
- plain TCP only
- no protocol framing
- no coroutine required in first version

## 4. Expected Flow
1. accept new connection
2. create TcpConnection
3. register Channel
4. on readable, read bytes
5. send back same bytes
6. on peer close/error, teardown safely

## 5. Validation Goal
This usecase should validate:
- EventLoop dispatch works
- Poller registration works
- Channel callback path works
- connection teardown path is safe