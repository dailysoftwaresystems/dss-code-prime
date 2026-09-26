#pragma once

#include "core/export.hpp"
#include "core/substrate/stack_sized_thread.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

// Executor abstraction + two implementations (host/OS utility — NOT a
// language/CPU/object-format concept, so it lives in `dss::substrate`
// alongside `large_stack_call`):
//   - ThreadPool: production. N worker threads draining a job
//     queue. Workers block on a condition variable until either a
//     job arrives or the pool is shutting down.
//   - SynchronousExecutor: tests / a deterministic single-threaded
//     baseline. Runs the submitted job inline on the calling thread.
//
// Two consumers today, both scheduling INDEPENDENT jobs that publish
// their results by side effect (never a return channel — `submit` is
// `void`-returning, fire-and-forget):
//   * the LSP server (parse jobs; each captures uri/version/transport
//     and publishes $/diagnostic when it finishes);
//   * the driver's per-CU build loop (D-PERF-4-CU-PARALLELISM; each job
//     builds ONE compilation unit's MIR into its own result slot + its
//     own scratch DiagnosticReporter, so the jobs share no mutable
//     state and the caller joins + merges deterministically).
//
// The interface intentionally exposes only `submit(std::function)`.
// Callers that need to wait for a batch to finish own their own
// completion primitive (a `std::latch`, a counter), so the executor
// stays a minimal "run this callable somewhere" contract shared by
// both consumers.

namespace dss::substrate {

class DSS_EXPORT IExecutor {
public:
    virtual ~IExecutor() noexcept = default;

    IExecutor(IExecutor const&)            = delete;
    IExecutor& operator=(IExecutor const&) = delete;
    IExecutor(IExecutor&&)                 = delete;
    IExecutor& operator=(IExecutor&&)      = delete;

    // Submit a job for execution. ThreadPool: enqueues for an
    // available worker. SynchronousExecutor: runs inline.
    virtual void submit(std::function<void()> job) = 0;

    // Block until in-flight jobs complete and signal workers to
    // exit. Idempotent; safe to call before destruction.
    virtual void shutdown() noexcept = 0;

protected:
    IExecutor() noexcept = default;
};

// Production executor. `workerCount` worker threads spawned at
// construction; each runs a loop pulling from the shared job queue.
//
// ★ EACH WORKER'S STACK IS STATED: `kMainThreadClassStackBytes`, never the host's
// default. The jobs are main-thread work — an LSP parse, a CU's parse, semantic
// analysis and MIR, and any schema build those reach — and the host default is
// 8 MiB on Linux, 1 MiB on Windows and 512 KiB on macOS. ✔MEASURED 2026-09-25
// (P68 round 12, D-SUBSTRATE-WORKER-THREADS-TAKE-THE-HOST-DEFAULT-STACK): a cold
// schema build on a plain `std::thread` dies `Bus error` on macos-arm64-debug
// every run; these workers were plain `std::thread`s.
class DSS_EXPORT ThreadPool final : public IExecutor {
public:
    explicit ThreadPool(std::size_t workerCount);
    ~ThreadPool() noexcept override;

    void submit(std::function<void()> job) override;
    void shutdown() noexcept override;

private:
    void workerLoop_();

    std::vector<StackSizedThread>      workers_;
    std::queue<std::function<void()>>  jobs_;
    std::mutex                         mutex_;
    std::condition_variable            cv_;
    std::atomic<bool>                  stopping_{false};
};

// Test / serial-baseline executor. Runs jobs inline; never spawns a
// thread. Because `submit` runs the job to completion before it
// returns, a caller's completion latch is already satisfied by the
// time the submit loop finishes — the deterministic single-threaded
// reference the pool path is compared against.
class DSS_EXPORT SynchronousExecutor final : public IExecutor {
public:
    SynchronousExecutor() = default;

    void submit(std::function<void()> job) override { job(); }
    void shutdown() noexcept override {}
};

} // namespace dss::substrate
