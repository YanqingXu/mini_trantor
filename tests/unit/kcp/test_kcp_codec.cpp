// test_kcp_codec.cpp
//
// 验证 KCP 帧编解码与 Task-03 的可靠发送时序基础。
// 1. codec：magic/version/flags/sessionId/seq/ack/payloadLen 的封包解码。
// 2. transport 生命周期：跨线程 send/close + Session 映射回收。

#include "mini/net/EventLoopThread.h"
#include "mini/net/InetAddress.h"
#include "mini/net/kcp/KcpCodec.h"
#include "mini/net/kcp/KcpSession.h"
#include "mini/net/kcp/KcpTransport.h"
#include "mini/net/transport/TransportManager.h"

#include <cassert>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace {

void testKcpFrameCodec() {
    const mini::net::kcp::codec::KcpFrame inFrame{
        .sessionId = 42,
        .seq = 100,
        .ack = 98,
        .flags = mini::net::kcp::codec::kKcpFrameFlagData | mini::net::kcp::codec::kKcpFrameFlagAck,
        .payload = std::string("frame-payload")};

    const auto raw = mini::net::kcp::codec::encodeFrame(inFrame);
    assert(!raw.empty());

    mini::net::kcp::codec::KcpFrame outFrame;
    assert(mini::net::kcp::codec::decodeFrame(raw, outFrame));
    assert(outFrame.sessionId == inFrame.sessionId);
    assert(outFrame.seq == inFrame.seq);
    assert(outFrame.ack == inFrame.ack);
    assert(outFrame.flags == inFrame.flags);
    assert(outFrame.payload == inFrame.payload);

    std::string badMagic = raw;
    badMagic[0] ^= 0xFF;
    assert(!mini::net::kcp::codec::decodeFrame(badMagic, outFrame));

    assert(!mini::net::kcp::codec::decodeFrame(
        std::string_view(raw.data(), mini::net::kcp::codec::kKcpFrameHeaderSize - 1),
        outFrame));

    assert(!mini::net::kcp::codec::decodeFrame(std::string(raw.begin(), raw.end()) + "x", outFrame));
}

void testTransportEndpointSessionContext() {
    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::net::kcp::KcpTransport transport(
        loop,
        mini::net::InetAddress(0, true),
        "unit-kcp-transport");
    transport.start();

    std::promise<mini::net::transport::TransportSessionId> openSessionPromise;
    auto openSessionFuture = openSessionPromise.get_future();
    std::promise<std::shared_ptr<mini::net::kcp::KcpSession>> sessionPromise;
    auto sessionFuture = sessionPromise.get_future();

    loop->queueInLoop([&] {
        auto session = transport.openSession(mini::net::InetAddress("127.0.0.1", 12345));
        if (session) {
            openSessionPromise.set_value(session->sessionId());
            sessionPromise.set_value(session);
        } else {
            openSessionPromise.set_value(mini::net::transport::kInvalidTransportSessionId);
            sessionPromise.set_value(nullptr);
        }
    });

    const auto sessionId = openSessionFuture.get();
    const auto session = sessionFuture.get();
    assert(sessionId != mini::net::transport::kInvalidTransportSessionId);
    assert(session != nullptr);
    assert(session->sessionId() == sessionId);
    assert(session->transportKind() == mini::net::transport::TransportKind::kKcp);
    assert(session->connected());
    assert(transport.getSession(sessionId) != nullptr);
    assert(transport.sessionCount() == 1);

    mini::net::transport::TransportManager manager(loop);
    const auto id = manager.registerEndpoint(session);
    assert(id == sessionId);
    assert(manager.hasEndpoint(id));

    std::thread crossThreadSend([&] {
        manager.send(id, "kcp-cross-thread-payload");
    });
    crossThreadSend.join();

    std::thread crossThreadClose([&] {
        manager.close(id);
    });
    crossThreadClose.join();

    auto closeObservedPromise = std::make_shared<std::promise<bool>>();
    auto closeObservedFuture = closeObservedPromise->get_future();
    loop->queueInLoop([session, p = closeObservedPromise]() mutable {
        p->set_value(!session->connected());
    });
    assert(closeObservedFuture.get());
    assert(manager.deregisterEndpoint(id));
    assert(!manager.hasEndpoint(id));

    transport.stop();
}

}  // namespace

int main() {
    testKcpFrameCodec();
    testTransportEndpointSessionContext();
    return 0;
}
