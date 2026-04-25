#include "mini/net/SignalWatcher.h"

#include "mini/net/EventLoop.h"

#include "mini/base/Logger.h"

#include <cerrno>
#include <cstring>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <unordered_set>

namespace mini::net {

namespace {

/// Ignore SIGPIPE process-wide.  Safe to call repeatedly.
void ignoreSigpipeImpl() {
    struct sigaction sa{};
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (::sigaction(SIGPIPE, &sa, nullptr) != 0) {
        LOG_SYSERR << "SignalWatcher: failed to ignore SIGPIPE: " << std::strerror(errno);
    }
}

}  // namespace

void SignalWatcher::blockSignals(const std::vector<int>& signals) {
    sigset_t mask;
    ::sigemptyset(&mask);
    for (int sig : signals) {
        ::sigaddset(&mask, sig);
    }
    if (::pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
        LOG_SYSERR << "SignalWatcher::blockSignals: pthread_sigmask failed: " << std::strerror(errno);
    }
}

void SignalWatcher::ignoreSigpipe() {
    ignoreSigpipeImpl();
}

SignalWatcher::SignalWatcher(EventLoop* loop, std::vector<int> signals)
    : loop_(loop),
      signals_(std::move(signals)),
      signalfd_(-1) {
    // Ignore SIGPIPE globally (idempotent).
    ignoreSigpipeImpl();

    // Block the monitored signals so they are not delivered via the
    // default handlers and are instead available to signalfd.
    sigset_t mask;
    ::sigemptyset(&mask);
    for (int sig : signals_) {
        ::sigaddset(&mask, sig);
    }
    if (::pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
        LOG_SYSERR << "SignalWatcher: pthread_sigmask failed: " << std::strerror(errno);
    }

    // Create the signalfd.
    signalfd_ = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signalfd_ < 0) {
        LOG_SYSFATAL << "SignalWatcher: signalfd failed: " << std::strerror(errno);
    }

    // Register the signalfd with the EventLoop.
    signalChannel_ = std::make_unique<Channel>(loop_, signalfd_);
    signalChannel_->setReadCallback([this](mini::base::Timestamp) { handleRead(); });
    signalChannel_->enableReading();
}

SignalWatcher::~SignalWatcher() {
    if (!loop_->isInLoopThread()) {
        LOG_FATAL << "SignalWatcher destroyed from non-owner thread";
    }
    signalChannel_->disableAll();
    signalChannel_->remove();
    ::close(signalfd_);

    // NOTE: We intentionally do NOT unblock the signals here.
    // In multi-threaded programs, unblocking signals in the destructor
    // would only affect the calling thread, and could cause signals to
    // be delivered via default handlers in threads that haven't set up
    // their own signalfd.  Callers who need to restore the signal mask
    // should manage that themselves.
}

void SignalWatcher::setSignalCallback(SignalCallback cb) {
    callback_ = std::move(cb);
}

const std::vector<int>& SignalWatcher::signals() const noexcept {
    return signals_;
}

void SignalWatcher::handleRead() {
    loop_->assertInLoopThread();

    while (true) {
        struct signalfd_siginfo si{};
        const ssize_t n = ::read(signalfd_, &si, sizeof(si));
        if (n != static_cast<ssize_t>(sizeof(si))) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            LOG_SYSERR << "SignalWatcher::handleRead: " << std::strerror(errno);
            break;
        }

        if (callback_) {
            callback_(static_cast<int>(si.ssi_signo));
        }
    }
}

}  // namespace mini::net
