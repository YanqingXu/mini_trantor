// Test harness contract: Release must retain checks AND assert setup expressions.
#include <cassert>
#include <cstdlib>
#ifdef NDEBUG
#error Contract tests require assertions in every build configuration
#endif
int main() {
    bool evaluated = false;
    assert((evaluated = true));
    return evaluated ? EXIT_SUCCESS : EXIT_FAILURE;
}
