#pragma once

#include "core/export.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// Executor abstraction + two implementations:
//   - ThreadPool: production. N worker threads draining a job
//     queue. Workers block on a condition variable until either a
//     job arrives or the pool is shutting down.
//   - SynchronousExecutor: tests. Runs the submitted job inline on
//     the calling thread. Makes the server's worker-side
//     publishDiagnostics logic deterministic in unit tests.
//
// The interface intentionally exposes only `submit(std::function)`.
// `IExecutor::submit` is `void`-returning — fire-and-forget. The
// LSP server publishes results by capturing the relevant state
// (uri, version, transport pointer, etc.) inside the submitted
// lambda, so the executor doesn't need a return channel.

namespace dss::lsp {

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
class DSS_EXPORT ThreadPool final : public IExecutor {
public:
    explicit ThreadPool(std::size_t workerCount);
    ~ThreadPool() noexcept override;

    void submit(std::function<void()> job) override;
    void shutdown() noexcept override;

private:
    void workerLoop_();

    std::vector<std::thread>           workers_;
    std::queue<std::function<void()>>  jobs_;
    std::mutex                         mutex_;
    std::condition_variable            cv_;
    std::atomic<bool>                  stopping_{false};
};

// Test executor. Runs jobs inline; never spawns a thread.
class DSS_EXPORT SynchronousExecutor final : public IExecutor {
public:
    SynchronousExecutor() = default;

    void submit(std::function<void()> job) override { job(); }
    void shutdown() noexcept override {}
};

} // namespace dss::lsp
