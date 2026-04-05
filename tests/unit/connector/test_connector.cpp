// Unit tests for Connector local invariants: state machine, backoff configuration.
// These tests verify logic without requiring real network connections.

#include "mini/net/Connector.h"
#include "mini/net/EventLoop.h"
#include "mini/net/InetAddress.h"

#include <cassert>
#include <chrono>
#include <memory>

using namespace std::chrono_literals;

int main() {
    // Unit 1: Initial state is kDisconnected
    {
        mini::net::EventLoop loop;
        auto connector = std::make_shared<mini::net::Connector>(
            &loop, mini::net::InetAddress("127.0.0.1", 19999));

        assert(connector->state() == mini::net::Connector::kDisconnected);
        assert(connector->serverAddress().toIp() == "127.0.0.1");
        assert(connector->serverAddress().port() == 19999);
    }

    // Unit 2: setRetryDelay configures backoff parameters
    {
        mini::net::EventLoop loop;
        auto connector = std::make_shared<mini::net::Connector>(
            &loop, mini::net::InetAddress("127.0.0.1", 19999));

        // Should not crash; parameters stored for later use
        connector->setRetryDelay(100ms, 5s);
        assert(connector->state() == mini::net::Connector::kDisconnected);
    }

    // Unit 3: setNewConnectionCallback does not change state
    {
        mini::net::EventLoop loop;
        auto connector = std::make_shared<mini::net::Connector>(
            &loop, mini::net::InetAddress("127.0.0.1", 19999));

        bool called = false;
        connector->setNewConnectionCallback([&](int) { called = true; });
        assert(!called);
        assert(connector->state() == mini::net::Connector::kDisconnected);
    }

    // Unit 4: Destruction in kDisconnected state is safe
    {
        mini::net::EventLoop loop;
        {
            auto connector = std::make_shared<mini::net::Connector>(
                &loop, mini::net::InetAddress("127.0.0.1", 19999));
            // destroy without ever calling start()
        }
        // must not crash
    }

    return 0;
}
