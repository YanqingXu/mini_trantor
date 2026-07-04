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
#include "mini/net/transport/PathMtuCache.h"
#include "mini/net/transport/TransportManager.h"

#include <cassert>
#include <chrono>
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

void testKcpFrameCodecPayloadBoundary() {
    mini::net::kcp::codec::KcpFrame boundaryFrame;
    boundaryFrame.sessionId = 43;
    boundaryFrame.seq = 7;
    boundaryFrame.flags = mini::net::kcp::codec::kKcpFrameFlagData |
                          mini::net::kcp::codec::kKcpFrameFlagFragment;
    boundaryFrame.payload.assign(mini::net::kcp::codec::kKcpMaxPayloadSize, 'x');

    const auto raw = mini::net::kcp::codec::encodeFrame(boundaryFrame);
    assert(raw.size() == mini::net::kcp::codec::kKcpFrameHeaderSize +
                             mini::net::kcp::codec::kKcpMaxPayloadSize);

    mini::net::kcp::codec::KcpFrame decoded;
    assert(mini::net::kcp::codec::decodeFrame(raw, decoded));
    assert(decoded.sessionId == boundaryFrame.sessionId);
    assert(decoded.seq == boundaryFrame.seq);
    assert(decoded.flags == boundaryFrame.flags);
    assert(decoded.payload.size() == boundaryFrame.payload.size());
    assert(decoded.payload == boundaryFrame.payload);

    boundaryFrame.payload.push_back('y');
    assert(mini::net::kcp::codec::encodeFrame(boundaryFrame).empty());
}

void testKcpFrameCodecSelectiveAckPayload() {
    mini::net::kcp::codec::KcpFrame ackFrame;
    ackFrame.sessionId = 44;
    ackFrame.ack = 6;
    ackFrame.flags = mini::net::kcp::codec::kKcpFrameFlagAck |
                     mini::net::kcp::codec::kKcpFrameFlagSelectiveAck;
    ackFrame.payload = std::string("SAK1", 4);
    ackFrame.payload.push_back('\0');
    ackFrame.payload.push_back('\1');
    ackFrame.payload.push_back('\0');
    ackFrame.payload.push_back('\0');
    ackFrame.payload.push_back('\0');
    ackFrame.payload.push_back('\7');

    const auto raw = mini::net::kcp::codec::encodeFrame(ackFrame);
    assert(!raw.empty());

    mini::net::kcp::codec::KcpFrame decoded;
    assert(mini::net::kcp::codec::decodeFrame(raw, decoded));
    assert(decoded.sessionId == ackFrame.sessionId);
    assert(decoded.ack == ackFrame.ack);
    assert(decoded.flags == ackFrame.flags);
    assert(decoded.payload == ackFrame.payload);
}

void testKcpFrameCodecMtuProbePayload() {
    mini::net::kcp::codec::KcpFrame probeFrame;
    probeFrame.sessionId = 45;
    probeFrame.ack = 3;
    probeFrame.flags = mini::net::kcp::codec::kKcpFrameFlagMtuProbe;
    probeFrame.payload = std::string("MTP1", 4);
    probeFrame.payload.push_back('\0');
    probeFrame.payload.push_back('\0');
    probeFrame.payload.push_back('\4');
    probeFrame.payload.push_back(static_cast<char>(0xB0));
    probeFrame.payload.resize(1200 - mini::net::kcp::codec::kKcpFrameHeaderSize, '\0');

    const auto raw = mini::net::kcp::codec::encodeFrame(probeFrame);
    assert(raw.size() == 1200);

    mini::net::kcp::codec::KcpFrame decoded;
    assert(mini::net::kcp::codec::decodeFrame(raw, decoded));
    assert(decoded.sessionId == probeFrame.sessionId);
    assert(decoded.ack == probeFrame.ack);
    assert(decoded.flags == probeFrame.flags);
    assert(decoded.payload == probeFrame.payload);

    mini::net::kcp::codec::KcpFrame ackFrame;
    ackFrame.sessionId = probeFrame.sessionId;
    ackFrame.ack = 3;
    ackFrame.flags = mini::net::kcp::codec::kKcpFrameFlagAck |
                     mini::net::kcp::codec::kKcpFrameFlagMtuProbe;
    ackFrame.payload = std::string("MTA1", 4);
    ackFrame.payload.push_back('\0');
    ackFrame.payload.push_back('\0');
    ackFrame.payload.push_back('\4');
    ackFrame.payload.push_back(static_cast<char>(0xB0));

    const auto ackRaw = mini::net::kcp::codec::encodeFrame(ackFrame);
    assert(!ackRaw.empty());
    assert(mini::net::kcp::codec::decodeFrame(ackRaw, decoded));
    assert(decoded.flags == ackFrame.flags);
    assert(decoded.payload == ackFrame.payload);
}

void testKcpFrameCodecXorParityPayload() {
    mini::net::kcp::codec::KcpFrame parityFrame;
    parityFrame.sessionId = 46;
    parityFrame.flags = mini::net::kcp::codec::kKcpFrameFlagXorParity;
    parityFrame.payload = std::string("XRP1", 4);
    parityFrame.payload.push_back('\0');
    parityFrame.payload.push_back('\0');
    parityFrame.payload.push_back('\0');
    parityFrame.payload.push_back('\1');
    parityFrame.payload.push_back('\0');
    parityFrame.payload.push_back('\2');
    parityFrame.payload.push_back('\0');
    parityFrame.payload.push_back('\4');
    parityFrame.payload.append("meta-parity");

    const auto raw = mini::net::kcp::codec::encodeFrame(parityFrame);
    assert(!raw.empty());

    mini::net::kcp::codec::KcpFrame decoded;
    assert(mini::net::kcp::codec::decodeFrame(raw, decoded));
    assert(decoded.sessionId == parityFrame.sessionId);
    assert(decoded.flags == parityFrame.flags);
    assert(decoded.payload == parityFrame.payload);
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
    auto registerDone = std::make_shared<std::promise<bool>>();
    auto registerDoneFuture = registerDone->get_future();
    loop->queueInLoop([&manager, id, registerDone]() {
        registerDone->set_value(manager.hasEndpoint(id));
    });
    assert(registerDoneFuture.get());

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
    auto deregisterDone = std::make_shared<std::promise<bool>>();
    auto deregisterDoneFuture = deregisterDone->get_future();
    loop->queueInLoop([&manager, id, deregisterDone]() {
        deregisterDone->set_value(!manager.hasEndpoint(id));
    });
    assert(deregisterDoneFuture.get());

    transport.stop();
}

void testSessionDetachedOnTransportStop() {
    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::net::kcp::KcpTransport transport(
        loop,
        mini::net::InetAddress(0, true),
        "unit-kcp-stop-detach");
    transport.start();

    std::promise<std::shared_ptr<mini::net::kcp::KcpSession>> sessionPromise;
    auto sessionFuture = sessionPromise.get_future();
    loop->queueInLoop([&] {
        sessionPromise.set_value(transport.openSession(mini::net::InetAddress("127.0.0.1", 12346)));
    });

    const auto session = sessionFuture.get();
    assert(session != nullptr);
    assert(session->connected());
    assert(session->hasOwner());
    assert(session->getLoop() == loop);
    assert(transport.sessionCount() == 1);

    std::promise<void> stoppedPromise;
    auto stoppedFuture = stoppedPromise.get_future();
    loop->queueInLoop([&] {
        transport.stop();
        stoppedPromise.set_value();
    });
    stoppedFuture.get();

    assert(transport.sessionCount() == 0);
    assert(!session->connected());
    assert(!session->hasOwner());
    assert(session->getLoop() == nullptr);

    session->send("after-stop");
    session->forceClose();
}

void testCrossThreadOpenSessionMarshalsToOwnerLoop() {
    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::net::kcp::KcpTransport transport(
        loop,
        mini::net::InetAddress(0, true),
        "unit-kcp-cross-thread-open");
    transport.start();

    auto session = transport.openSession(mini::net::InetAddress("127.0.0.1", 12347));
    assert(session != nullptr);
    assert(session->connected());
    assert(session->hasOwner());
    assert(session->getLoop() == loop);
    assert(transport.getSession(session->sessionId()) == session);
    assert(transport.sessionCount() == 1);

    transport.stop();
    auto stoppedSession = transport.openSession(mini::net::InetAddress("127.0.0.1", 12348));
    assert(stoppedSession == nullptr);
}

void testKcpTransportOptionsAreNormalized() {
    mini::net::EventLoopThread loopThread;
    auto* loop = loopThread.startLoop();

    mini::net::kcp::KcpTransportOptions options;
    options.initialRto = std::chrono::milliseconds(0);
    options.maxRto = std::chrono::milliseconds(1);
    options.maxRetransmissions = 0;
    options.maxDatagramPayloadSize = 1;
    options.maxApplicationPayloadSize = 0;
    options.minDatagramPayloadSize = 1;
    options.mtuProbeStepBytes = 0;
    options.mtuProbeMaxRetries = 0;
    options.mtuProbeInterval = std::chrono::milliseconds(0);
    options.mtuProbeBlackholeCooldown = std::chrono::milliseconds(0);
    options.enableMtuPathCache = true;
    options.sharedMtuPathCache = std::make_shared<mini::net::transport::PathMtuCache>();
    options.enablePlatformPathMtuSignals = true;
    options.enableRawIcmpPathMtuSignals = true;
    options.enablePathMtuSignalAuthentication = true;
    options.minCongestionWindow = 0;
    options.initialCongestionWindow = 0;
    options.maxCongestionWindow = 0;
    options.redundantCopyCount = 0;
    options.enableXorParityRecovery = true;
    options.xorParityGroupSize = 1;

    mini::net::kcp::KcpTransport transport(
        loop,
        mini::net::InetAddress(0, true),
        "unit-kcp-options",
        true,
        options);

    const auto& normalized = transport.options();
    assert(normalized.initialRto == std::chrono::milliseconds(25));
    assert(normalized.maxRto == normalized.initialRto);
    assert(normalized.maxRetransmissions == 4);
    assert(normalized.maxDatagramPayloadSize == mini::net::kcp::KcpTransport::kDefaultMtuBytes);
    assert(normalized.maxApplicationPayloadSize == mini::net::kcp::KcpTransport::kMaxApplicationPayloadSize);
    assert(normalized.minDatagramPayloadSize == mini::net::kcp::KcpTransport::kDefaultMtuBytes);
    assert(normalized.mtuProbeStepBytes == 200);
    assert(normalized.mtuProbeMaxRetries == 1);
    assert(normalized.mtuProbeInterval == std::chrono::milliseconds(100));
    assert(normalized.mtuProbeBlackholeCooldown == std::chrono::milliseconds(1000));
    assert(normalized.enableMtuPathCache);
    assert(normalized.sharedMtuPathCache == options.sharedMtuPathCache);
    assert(normalized.enablePlatformPathMtuSignals);
    assert(normalized.enableRawIcmpPathMtuSignals);
    assert(normalized.enablePathMtuSignalAuthentication);
    assert(normalized.minCongestionWindow == 1);
    assert(normalized.initialCongestionWindow == 1);
    assert(normalized.maxCongestionWindow == 1);
    assert(normalized.redundantCopyCount == 1);
    assert(normalized.enableXorParityRecovery);
    assert(normalized.xorParityGroupSize == 4);

    transport.stop();
}

}  // namespace

int main() {
    testKcpFrameCodec();
    testKcpFrameCodecPayloadBoundary();
    testKcpFrameCodecSelectiveAckPayload();
    testKcpFrameCodecMtuProbePayload();
    testKcpFrameCodecXorParityPayload();
    testTransportEndpointSessionContext();
    testSessionDetachedOnTransportStop();
    testCrossThreadOpenSessionMarshalsToOwnerLoop();
    testKcpTransportOptionsAreNormalized();
    return 0;
}
