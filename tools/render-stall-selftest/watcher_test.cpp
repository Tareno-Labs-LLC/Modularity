#include "../../src/ShaderFileWatcher.h"
#include <atomic>
#include <cassert>
#include <future>
#include <iostream>

using Time = ShaderFileWatcher::Time;
static std::promise<void> started, releaseRead, barrierRead;
static std::atomic<int> reads{0};
static std::shared_future<void> releaseFuture = releaseRead.get_future().share();
static Time slowRead(const std::string&, std::error_code&) {
    if (reads.fetch_add(1) == 0) {
        started.set_value();
        releaseFuture.wait();
    } else barrierRead.set_value();
    return Time(Time::duration(2));
}
int main() {
    ShaderFileWatcher watcher(slowRead);
    const Time original(Time::duration(1));
    assert(watcher.poll("blocked-file", original) == original);
    started.get_future().wait();
    // The worker is stuck in I/O. Every GL-thread poll must still complete.
    const auto begin = ShaderFileWatcher::Clock::now();
    for (int i = 0; i < 10000; ++i)
        assert(watcher.poll("blocked-file", original) == original);
    const auto ms = std::chrono::duration<double, std::milli>(ShaderFileWatcher::Clock::now() - begin).count();
    assert(ms < 100);
    // A forced reload must not be undone by an older in-flight timestamp result.
    const Time newer(Time::duration(3));
    watcher.seed("blocked-file", newer);
    watcher.poll("barrier-file", original);
    releaseRead.set_value();
    barrierRead.get_future().wait();
    assert(watcher.poll("blocked-file", original) == newer);
    std::cout << "PASS: 10000 timestamp polls completed in " << ms
              << " ms while filesystem read was blocked; forced reload preserves newer baseline\n";
}
