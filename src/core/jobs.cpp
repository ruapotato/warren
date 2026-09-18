#include "jobs.h"

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "core/log.h"

namespace wr {
namespace {

struct Job {
    std::function<void()> fn;
    JobCounterRef counter;
};

struct Pool {
    std::vector<std::thread> workers;
    std::deque<Job> queue;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::atomic<uint64_t> submitted{0};
    std::atomic<uint64_t> completed{0};
    std::atomic<int> busy{0};
};

Pool &pool() {
    static Pool p;
    return p;
}

// Take one job if there is one. Returns false when the queue is empty,
// so a caller can decide whether to sleep or to carry on.
bool take_one(Job *out) {
    Pool &p = pool();
    std::lock_guard<std::mutex> lock(p.mutex);
    if (p.queue.empty()) return false;
    *out = std::move(p.queue.front());
    p.queue.pop_front();
    return true;
}

void run(Job &job) {
    Pool &p = pool();
    p.busy.fetch_add(1, std::memory_order_relaxed);
    if (job.fn) job.fn();
    if (job.counter) job.counter->finish();
    p.completed.fetch_add(1, std::memory_order_relaxed);
    p.busy.fetch_sub(1, std::memory_order_relaxed);
}

void worker_loop() {
    Pool &p = pool();
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(p.mutex);
            p.wake.wait(lock, [&] { return p.stopping || !p.queue.empty(); });
            if (p.stopping && p.queue.empty()) return;
            job = std::move(p.queue.front());
            p.queue.pop_front();
        }
        run(job);
    }
}

}  // namespace

void Jobs::init(int threads) {
    Pool &p = pool();
    if (!p.workers.empty()) return;
    if (threads <= 0) {
        unsigned hw = std::thread::hardware_concurrency();
        // ONE CORE IS LEFT FOR THE MAIN THREAD. Saturating every core
        // with workers means the thread that has to finish the frame
        // is competing with the work it is waiting for.
        threads = int(hw > 2 ? hw - 1 : 1);
    }
    p.stopping = false;
    p.workers.reserve(size_t(threads));
    for (int i = 0; i < threads; i++) p.workers.emplace_back(worker_loop);
    WR_INFO("jobs: %d worker threads", threads);
}

void Jobs::shutdown() {
    Pool &p = pool();
    if (p.workers.empty()) return;
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.stopping = true;
    }
    p.wake.notify_all();
    for (std::thread &t : p.workers)
        if (t.joinable()) t.join();
    p.workers.clear();
    std::lock_guard<std::mutex> lock(p.mutex);
    p.queue.clear();
}

int Jobs::worker_count() { return int(pool().workers.size()); }
bool Jobs::running() { return !pool().workers.empty(); }
JobCounterRef Jobs::make_counter() { return std::make_shared<JobCounter>(); }

void Jobs::submit(std::function<void()> fn, const JobCounterRef &counter) {
    Pool &p = pool();
    if (counter) counter->add(1);
    p.submitted.fetch_add(1, std::memory_order_relaxed);
    // NO WORKERS MEANS RUN IT HERE. A headless test or a tool that
    // never called init must still work, and silently dropping the
    // job would be the worst of the options.
    if (p.workers.empty()) {
        Job job{std::move(fn), counter};
        run(job);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.queue.push_back({std::move(fn), counter});
    }
    p.wake.notify_one();
}

void Jobs::wait(const JobCounterRef &counter) {
    if (!counter) return;
    // Help rather than idle.
    while (!counter->done()) {
        Job job;
        if (take_one(&job))
            run(job);
        else
            std::this_thread::yield();
    }
}

void Jobs::parallel_for(size_t count, size_t grain,
                        const std::function<void(size_t, size_t)> &fn) {
    if (!count || !fn) return;
    if (grain == 0) grain = 1;
    Pool &p = pool();
    // Small enough, or nowhere to put it: just do it.
    if (count <= grain || p.workers.empty()) {
        fn(0, count);
        return;
    }
    // One chunk per worker where that is at least `grain`, so the
    // split matches the machine rather than the data.
    size_t chunks = std::min(count / grain, size_t(p.workers.size()) + 1);
    if (chunks < 1) chunks = 1;
    size_t per = (count + chunks - 1) / chunks;

    JobCounterRef counter = make_counter();
    for (size_t i = 1; i < chunks; i++) {
        size_t begin = i * per;
        size_t end = std::min(begin + per, count);
        if (begin >= end) break;
        submit([&fn, begin, end] { fn(begin, end); }, counter);
    }
    // The caller takes the first chunk itself.
    fn(0, std::min(per, count));
    wait(counter);
}

std::string Jobs::report() {
    Pool &p = pool();
    size_t queued;
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        queued = p.queue.size();
    }
    char b[192];
    std::snprintf(b, sizeof(b),
                  "jobs: %zu workers, %zu queued, %d running, %llu submitted, "
                  "%llu completed",
                  p.workers.size(), queued, p.busy.load(),
                  (unsigned long long)p.submitted.load(),
                  (unsigned long long)p.completed.load());
    return b;
}

}  // namespace wr
