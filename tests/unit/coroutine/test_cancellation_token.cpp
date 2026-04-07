#include "mini/coroutine/CancellationToken.h"

#include <cassert>

int main() {
    // Unit 1: fresh token is not cancelled
    {
        mini::coroutine::CancellationSource source;
        auto token = source.token();
        assert(token);
        assert(!token.isCancellationRequested());
        assert(!source.isCancellationRequested());
    }

    // Unit 2: cancel propagates to token
    {
        mini::coroutine::CancellationSource source;
        auto token = source.token();
        source.cancel();
        assert(source.isCancellationRequested());
        assert(token.isCancellationRequested());
    }

    // Unit 3: registered callback fires on cancel
    {
        mini::coroutine::CancellationSource source;
        auto token = source.token();
        bool fired = false;
        auto registration = token.registerCallback([&] { fired = true; });
        source.cancel();
        assert(fired);
        registration.reset();
    }

    // Unit 4: reset registration suppresses callback
    {
        mini::coroutine::CancellationSource source;
        auto token = source.token();
        bool fired = false;
        auto registration = token.registerCallback([&] { fired = true; });
        registration.reset();
        source.cancel();
        assert(!fired);
    }

    // Unit 5: late registration fires immediately
    {
        mini::coroutine::CancellationSource source;
        auto token = source.token();
        source.cancel();
        bool fired = false;
        auto registration = token.registerCallback([&] { fired = true; });
        assert(fired);
        registration.reset();
    }

    return 0;
}
