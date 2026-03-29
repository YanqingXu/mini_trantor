#include "mini/net/EventLoop.h"
#include "mini/net/EventLoopThreadPool.h"

#include <cassert>

int main() {
    {
        mini::net::EventLoop baseLoop;
        mini::net::EventLoopThreadPool pool(&baseLoop, "zero");
        pool.start();

        assert(pool.getNextLoop() == &baseLoop);
        const auto loops = pool.getAllLoops();
        assert(loops.size() == 1);
        assert(loops.front() == &baseLoop);
    }

    {
        mini::net::EventLoop baseLoop;
        mini::net::EventLoopThreadPool pool(&baseLoop, "workers");
        pool.setThreadNum(2);
        pool.start();

        const auto loops = pool.getAllLoops();
        assert(loops.size() == 2);
        assert(loops[0] != nullptr);
        assert(loops[1] != nullptr);
        assert(loops[0] != loops[1]);

        assert(pool.getNextLoop() == loops[0]);
        assert(pool.getNextLoop() == loops[1]);
        assert(pool.getNextLoop() == loops[0]);
    }

    return 0;
}
