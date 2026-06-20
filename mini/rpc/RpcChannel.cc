#include "mini/rpc/RpcChannel.h"

#include "mini/net/Buffer.h"
#include "mini/net/framing/PacketFramer.h"
#include "mini/net/EventLoop.h"
#include "mini/net/ProtocolConnectionAdapter.h"
#include "mini/net/TcpConnection.h"  // lifecycle: onMessage signature
#include <any>
#include <array>

#include <utility>

namespace mini::rpc {

RpcChannel::RpcChannel(mini::net::EventLoop* loop)
    : loop_(loop) {
}

bool RpcChannel::onMessage(const mini::net::TcpConnectionPtr& conn, mini::net::Buffer* buf) {
    static constexpr std::size_t kMaxFramesPerRead = 32;
    static const mini::net::framing::PacketFramer kFrameFramer;

    const auto dispatchMessage = [this, &conn](const RpcMessage& msg) {
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
        } else if (requestCallback_) {
            // Server side: dispatch to request handler.
            const auto reqId = msg.requestId;
            auto proto = mini::net::ProtocolConnectionAdapter::sharedFrom(conn);

            auto respond = [proto, reqId](std::string_view payload) {
                if (proto) {
                    proto->send(codec::encodeResponse(reqId, payload));
                }
            };

            auto respondError = [proto, reqId](std::string_view errorMsg) {
                if (proto) {
                    proto->send(codec::encodeError(reqId, errorMsg));
                }
            };

            requestCallback_(msg.method, msg.payload, std::move(respond), std::move(respondError));
        }
    };

    while (buf->readableBytes() > 0) {
        std::array<mini::net::framing::Packet, kMaxFramesPerRead> packets{};
        const auto batch = kFrameFramer.decodeBatch(
            buf->peek(),
            buf->readableBytes(),
            packets.data(),
            packets.size(),
            kMaxFramesPerRead);

        if (batch.status == mini::net::framing::PacketDecodeState::kInvalid ||
            batch.status == mini::net::framing::PacketDecodeState::kOverLimit) {
            return false;
        }

        for (std::size_t i = 0; i < batch.frameCount; ++i) {
            if (packets[i].header.msgId != codec::kRpcMsgId) {
                return false;
            }

            RpcMessage msg;
            if (codec::decodePayload(packets[i].payload, msg) != RpcDecodeResult::kComplete) {
                return false;
            }

            dispatchMessage(msg);
        }

        buf->retrieve(batch.consumed);

        if (batch.status == mini::net::framing::PacketDecodeState::kNeedMore) {
            if (batch.hitLimit && batch.frameCount > 0) {
                auto connShared = conn;
                loop_->queueInLoop([connShared, buf] {
                    if (!connShared->connected()) {
                        return;
                    }

                    auto* channel = std::any_cast<RpcChannel>(&connShared->getContext());
                    if (!channel) {
                        auto* adapter = mini::net::ProtocolConnectionAdapter::getFrom(connShared);
                        if (!adapter) {
                            return;
                        }

                        channel = std::any_cast<RpcChannel>(&adapter->getProtocolContext());
                        if (!channel) {
                            return;
                        }
                    }

                    channel->onMessage(connShared, buf);
                });
            }
            break;
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
