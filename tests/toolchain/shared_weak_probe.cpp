// Standalone toolchain diagnostic. No mini-trantor code or headers are used.
#include <memory>
#include <thread>

int main() {
#ifdef MINI_TSAN_NEGATIVE_CONTROL
    int value = 0;
    std::thread worker([&] { value = 1; });
    value = 2; // Intentional race: the diagnostic must exit with TSan status 66.
    worker.join();
    return value == 0;
#else
    for (int i = 0; i < 2000; ++i) {
        auto strong = std::make_shared<int>(i);
        std::weak_ptr<int> weak = strong;
        std::thread worker([strong = std::move(strong)]() mutable { strong.reset(); });
        weak.reset();
        worker.join();
    }
#endif
}
