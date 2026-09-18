// Manifold -- work on other threads.
//
// Small on purpose. A game engine needs three things from a job
// system -- run this, run these N in parallel, tell me when they are
// done -- and most of the complexity in a general one is spent on
// things a frame loop does not do.
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace mf {

// A count of outstanding work. Shared, so a job can keep it alive
// after whoever submitted it has moved on.
class JobCounter {
public:
    void add(int n = 1) { count_.fetch_add(n, std::memory_order_relaxed); }
    void finish() { count_.fetch_sub(1, std::memory_order_acq_rel); }
    bool done() const { return count_.load(std::memory_order_acquire) <= 0; }
    int outstanding() const { return count_.load(std::memory_order_acquire); }

private:
    std::atomic<int> count_{0};
};
using JobCounterRef = std::shared_ptr<JobCounter>;

class Jobs {
public:
    // `threads` of 0 leaves one core for the main thread, which is
    // the one that must not be starved.
    static void init(int threads = 0);
    static void shutdown();
    static int worker_count();
    static bool running();

    // Fire and forget, or track with a counter.
    static void submit(std::function<void()> fn, const JobCounterRef &counter = {});

    // Split [0, count) into chunks of at least `grain` and run them.
    // Returns when they are all done -- and the CALLING THREAD JOINS
    // IN rather than idling, which matters because the caller is
    // usually the main thread and it is not allowed to be the slow
    // one.
    static void parallel_for(size_t count, size_t grain,
                             const std::function<void(size_t begin, size_t end)> &fn);

    // Wait on a counter, running other jobs meanwhile so the thread
    // is never blocked behind work it could be doing.
    static void wait(const JobCounterRef &counter);
    static JobCounterRef make_counter();

    static std::string report();
};

}  // namespace mf
