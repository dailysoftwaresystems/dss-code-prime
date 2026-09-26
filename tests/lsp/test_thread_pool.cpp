// ThreadPool + SynchronousExecutor: basic submit/drain semantics
// and exception isolation (a throwing job must not kill the worker).

#include "core/substrate/thread_pool.hpp"
#include "core/types/config_document_memo.hpp"
#include "core/types/grammar_schema.hpp"
#include "test_wait_budget.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

using dss::substrate::SynchronousExecutor;
using dss::substrate::ThreadPool;

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
    ASSERT_EQ(fut.wait_for(dss::test_support::kWaitBudget),
              std::future_status::ready);
    EXPECT_EQ(hits.load(), 1);
    pool.shutdown();
}

TEST(ThreadPool, ShutdownIsIdempotent) {
    ThreadPool pool{1};
    pool.shutdown();
    pool.shutdown(); // must not crash / double-join
}

// ── P68 round 12 (D-SUBSTRATE-WORKER-THREADS-TAKE-THE-HOST-DEFAULT-STACK) ──────
// A pool job is MAIN-THREAD work: the driver builds a whole compilation unit —
// parse, semantic analysis, MIR — on a worker, and the LSP server a parse. The
// heaviest single frame that work is known to reach is a COLD config-schema build
// (`buildSchemaFromJsonText`, 415,360 bytes under clang -O0, P34). So a cold build
// must survive on a worker, and this job performs one: the memo is cleared first
// and the build is required to have happened (the miss count). ★ The workers were
// plain `std::thread`s, which take the host's default secondary-thread stack — 512
// KiB on macOS — and a cold build on one dies `Bus error` on macos-arm64-debug
// (✔MEASURED 2026-09-25). Mac Debug is the only leg that can tell; every other leg
// runs this green either way, which is why the pool STATES its workers' stack.
TEST(ThreadPool, AColdSchemaBuildRunsOnAWorker) {
    dss::detail::ConfigDocumentMemo<dss::GrammarSchema>::clear();
    dss::detail::ConfigDocumentMemoStore::resetStats();
    std::promise<bool> built;
    auto fut = built.get_future();
    ThreadPool pool{1};
    pool.submit([&] { built.set_value(dss::GrammarSchema::loadShipped("c").has_value()); });
    ASSERT_EQ(fut.wait_for(dss::test_support::kWaitBudget), std::future_status::ready);
    EXPECT_TRUE(fut.get()) << "the shipped C document did not load on a pool worker";
    pool.shutdown();
    EXPECT_GE(dss::detail::ConfigDocumentMemo<dss::GrammarSchema>::stats().misses, 1u)
        << "the worker BUILT nothing — the memo was warm, so the frame this pins "
           "was never on the worker's stack";
}
