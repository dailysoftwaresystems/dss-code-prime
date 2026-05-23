// ThreadPool + SynchronousExecutor: basic submit/drain semantics
// and exception isolation (a throwing job must not kill the worker).

#include "lsp/thread_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

using dss::lsp::SynchronousExecutor;
using dss::lsp::ThreadPool;

TEST(SynchronousExecutor, RunsJobInline) {
    SynchronousExecutor ex;
    int hits = 0;
    ex.submit([&] { ++hits; });
    EXPECT_EQ(hits, 1);
    ex.shutdown();
    EXPECT_EQ(hits, 1);
}

TEST(ThreadPool, SubmitDrainsBeforeShutdown) {
    ThreadPool pool{2};
    std::atomic<int> hits{0};
    constexpr int kJobs = 32;
    for (int i = 0; i < kJobs; ++i) {
        pool.submit([&] { hits.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.shutdown();
    EXPECT_EQ(hits.load(), kJobs);
}

TEST(ThreadPool, WorkerSurvivesThrowingJob) {
    ThreadPool pool{1};
    std::atomic<int> hits{0};
    pool.submit([] { throw std::runtime_error{"boom"}; });
    // Subsequent jobs must still run — the worker must catch and continue.
    std::promise<void> p;
    auto fut = p.get_future();
    pool.submit([&] {
        hits.fetch_add(1, std::memory_order_relaxed);
        p.set_value();
    });
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(hits.load(), 1);
    pool.shutdown();
}

TEST(ThreadPool, ShutdownIsIdempotent) {
    ThreadPool pool{1};
    pool.shutdown();
    pool.shutdown(); // must not crash / double-join
}
