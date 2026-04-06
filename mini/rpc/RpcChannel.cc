#include "mini/rpc/RpcChannel.h"

#include "mini/net/Buffer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/TcpConnection.h"

#include <utility>

namespace mini::rpc {

RpcChannel::RpcChannel(mini::net::EventLoop* loop)
    : loop_(loop) {
}

bool RpcChannel::onMessage(const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf) {
    while (buf->readableBytes() > 0) {
        RpcMessage msg;
        std::size_t consumed = 0;
        auto result = codec::decode(buf->peek(), buf->readableBytes(), msg, consumed);

        if (result == RpcDecodeResult::kIncomplete) {
            break;
        }
        if (result == RpcDecodeResult::kError) {
            return false;
        }

        buf->retrieve(consumed);

        if (msg.msgType == RpcMsgType::kResponse || msg.msgType == RpcMsgType::kError) {
            // Client side: match to pending call.
            auto it = pendingCalls_.find(msg.requestId);
            if (it != pendingCalls_.end()) {
                auto pending = std::move(it->second);
                pendingCalls_.erase(it);

                // Cancel timeout timer if set.
                if (pending.timerId.valid()) {
                    loop_->cancel(pending.timerId);
                }

                if (pending.callback) {
                    if (msg.msgType == RpcMsgType::kError) {
                        pending.callback(msg.payload, "");
                    } else {
                        pending.callback("", msg.payload);
                    }
                }
            }
            // Response without matching request: silently discard.
        } else {
            // Server side: dispatch to request handler.
            if (requestCallback_) {
                std::uint64_t reqId = msg.requestId;
                std::weak_ptr<mini::net::TcpConnection> weakConn = conn;

                auto respond = [weakConn, reqId](std::string_view payload) {
                    auto c = weakConn.lock();
                    if (c) {
                        c->send(codec::encodeResponse(reqId, payload));
                    }
                };

                auto respondError = [weakConn, reqId](std::string_view errorMsg) {
                    auto c = weakConn.lock();
                    if (c) {
                        c->send(codec::encodeError(reqId, errorMsg));
                    }
                };

                requestCallback_(msg.method, msg.payload,
                                 std::move(respond), std::move(respondError));
            }
        }
    }
    return true;
}

void RpcChannel::sendRequest(const mini::net::TcpConnectionPtr& conn,
                              std::string_view method,
                              std::string_view payload,
                              RpcResponseCallback cb,
                              int timeoutMs) {
    std::uint64_t reqId = nextRequestId_++;

    PendingCall pending;
    pending.callback = std::move(cb);

    if (timeoutMs > 0) {
        pending.timerId = loop_->runAfter(
            std::chrono::milliseconds(timeoutMs),
            [this, reqId] {
                auto it = pendingCalls_.find(reqId);
                if (it != pendingCalls_.end()) {
                    auto cb = std::move(it->second.callback);
                    pendingCalls_.erase(it);
                    if (cb) {
                        cb("RPC call timed out", "");
                    }
                }
            });
    }

    pendingCalls_.emplace(reqId, std::move(pending));
    conn->send(codec::encodeRequest(reqId, method, payload));
}

void RpcChannel::failAllPending(const std::string& reason) {
    auto calls = std::move(pendingCalls_);
    pendingCalls_.clear();
    for (auto& [id, pending] : calls) {
        if (pending.timerId.valid()) {
            loop_->cancel(pending.timerId);
        }
        if (pending.callback) {
            pending.callback(reason, "");
        }
    }
}

}  // namespace mini::rpc
