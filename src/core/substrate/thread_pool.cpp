#include "core/substrate/thread_pool.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <utility>

namespace dss::substrate {

ThreadPool::ThreadPool(std::size_t workerCount) {
    const auto count = std::max<std::size_t>(1, workerCount);
    workers_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        workers_.emplace_back([this] { workerLoop_(); });
    }
}

ThreadPool::~ThreadPool() noexcept {
    shutdown();
}

void ThreadPool::submit(std::function<void()> job) {
    {
        std::lock_guard lk{mutex_};
        if (stopping_.load(std::memory_order_acquire)) return;
        jobs_.push(std::move(job));
    }
    cv_.notify_one();
}

void ThreadPool::shutdown() noexcept {
    {
        std::lock_guard lk{mutex_};
        stopping_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
}

void ThreadPool::workerLoop_() {
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock lk{mutex_};
            cv_.wait(lk, [this] {
                return stopping_.load(std::memory_order_acquire) || !jobs_.empty();
            });
            // Drain remaining jobs even when stopping — callers may
            // have queued urgent work just before shutdown.
            if (jobs_.empty()) return;
            job = std::move(jobs_.front());
            jobs_.pop();
        }
        // Swallow worker exceptions to keep the pool alive. Log to
        // stderr so a bad_alloc / nlohmann throw / future parser
        // invariant violation doesn't become invisible — without
        // this, the user sees "no diagnostics" with no trace at all.
        // (Batch callers pair each job with an RAII completion signal
        // that fires on unwind too, so a throwing job cannot deadlock
        // a caller waiting on the batch.)
        try {
            job();
        } catch (std::exception const& e) {
            std::fprintf(stderr, "[substrate/thread_pool] job threw: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "[substrate/thread_pool] job threw (unknown)\n");
        }
    }
}

} // namespace dss::substrate
