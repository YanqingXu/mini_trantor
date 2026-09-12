#include "mini/net/LoopHandle.h"

#include "mini/net/EventLoop.h"

#include <stdexcept>
#include <utility>

namespace mini::net {

bool LoopHandle::queue(Functor callback) const {
    if (!callback) { throw std::invalid_argument("LoopHandle::queue requires a callback"); }
    auto state = state_;
    if (!state) { return false; }
    // Prepare before locking. Rejection or allocation failure must destroy the
    // callback's captures only after the posting lock has been released.
    EventLoop::PendingFunctor pending{std::move(callback), mini::base::now()};
    std::lock_guard lock(state->mutex);
    if (!state->loop) { return false; }
    state->loop->queuePrepared(std::move(pending));
    return true;
}

} // namespace mini::net
