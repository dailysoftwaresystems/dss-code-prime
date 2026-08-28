#pragma once

#include "core/substrate/stack_sized_thread.hpp"
#include "core/substrate/thread_pool.hpp"
#include "lsp/json_rpc.hpp"
#include "lsp/lsp_server.hpp"
#include "lsp/schema_cache.hpp"
#include "lsp/transport.hpp"
#include "lsp/workspace_project.hpp"   // fileUriFromPath - the ONE owner this helper forwards to
#include "test_wait_budget.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dss::lsp::testing {

// Test transport: client queues incoming messages by calling
// `pushClientMessage(jsonBody)`; the server reads them via
// `readMessage()`. Server writes go into `serverMessages_`; the
// test inspects them via `takeServerMessages()`.
//
// `readMessage()` blocks until either a message is available OR
// `close()` is called (then returns Eof). This mirrors stdio
// blocking semantics for E2E and replay tests.
class InMemoryTransport final : public LspTransport {
public:
    InMemoryTransport() = default;

    // Server-side: read a framed message (already-unframed body).
    [[nodiscard]] std::expected<std::string, TransportError> readMessage() override {
        std::unique_lock lk{mutex_};
        cv_.wait(lk, [this] {
            return closed_.load(std::memory_order_acquire) || !clientQueue_.empty();
        });
        if (clientQueue_.empty()) {
            return std::unexpected(TransportError::Eof);
        }
        auto body = std::move(clientQueue_.front());
        clientQueue_.pop_front();
        return body;
    }

    [[nodiscard]] std::expected<void, TransportError> writeMessage(std::string_view body) override {
        std::lock_guard lk{mutex_};
        serverMessages_.emplace_back(body);
        return {};
    }

    void close() noexcept override {
        closed_.store(true, std::memory_order_release);
        cv_.notify_all();
    }

    // Client-side: enqueue a raw JSON body (NOT framed; this is what
    // the server's `readMessage` returns).
    void pushClientMessage(std::string body) {
        {
            std::lock_guard lk{mutex_};
            clientQueue_.push_back(std::move(body));
        }
        cv_.notify_one();
    }

    [[nodiscard]] std::vector<std::string> takeServerMessages() {
        std::lock_guard lk{mutex_};
        return std::exchange(serverMessages_, {});
    }

    [[nodiscard]] std::size_t pendingClientMessages() const {
        std::lock_guard lk{mutex_};
        return clientQueue_.size();
    }

private:
    mutable std::mutex          mutex_;
    std::condition_variable     cv_;
    std::deque<std::string>     clientQueue_;
    std::vector<std::string>    serverMessages_;
    std::atomic<bool>           closed_{false};
};

// LspTestHarness: spin up an LspServer on a background thread
// against a fresh InMemoryTransport + SchemaCache +
// SynchronousExecutor. The harness lets tests focus on
// "messages → assertions" without repeating ~5 lines of setup.
//
// Lifetime: the future is kicked off in the ctor; the test pushes
// client messages, then calls `runUntilExit()` to await server
// teardown and return the exit code. Move-only; one harness per
// test.
// The stack the harness gives the server loop. 8 MiB is the DOCUMENTED main-
// thread default on both POSIX hosts this project gates on, and production runs
// `server.run()` on main -- so this is not a generous number, it is the number
// the emulated thread already had. Reserved, not committed: a shallow run
// touches about one page of it.
inline constexpr std::size_t kServerLoopStackBytes = 8u * 1024u * 1024u;

class LspTestHarness {
public:
    LspTestHarness()
        : transport_(new InMemoryTransport{})
        , server_(std::unique_ptr<LspTransport>{transport_},
                  std::make_unique<substrate::SynchronousExecutor>(),
                  cache_)
        , exitFuture_(exitPromise_.get_future())
        // ★★ A STATED STACK, NOT THE HOST'S DEFAULT. This thread stands in for
        // production's MAIN thread -- `runLspMode` calls `server.run()` on it, and
        // the schema resolve that `didOpen` triggers happens THERE, before any job
        // reaches the executor. `std::async` gave it the host's secondary-thread
        // default instead: 8 MiB on Linux, 512 KiB on macOS. ✔MEASURED 2026-08-25
        // (cycle P34): `buildSchemaFromJsonText` compiles to a **415,360-byte**
        // frame under clang -O0, so all four LSP binaries died `Bus error` on macOS
        // and passed everywhere else. The harness was emulating an 8 MiB thread
        // with 1/16th of its stack.
        // D-TEST-LSP-HARNESS-RAN-THE-SERVER-LOOP-ON-A-HOST-DEFAULT-STACK
        , serverThread_(kServerLoopStackBytes, [this] {
              // Nothing may escape a thread entry, and a `run()` that threw would
              // otherwise leave the promise unsatisfied -- which `runUntilExit`
              // would report as a TIMEOUT, sending the reader to the wrong
              // instrument entirely.
              try {
                  exitPromise_.set_value(server_.run());
              } catch (...) {
                  exitPromise_.set_exception(std::current_exception());
              }
          }) {}

    // ★★ THE HARNESS MUST SURVIVE A FAILING TEST, NOT HANG IT.
    // The server thread is joined HERE, in the destructor body, while the
    // server thread may still be parked in
    // `InMemoryTransport::readMessage()` waiting for a message that will never
    // come. Any test that returns early (a failed `ASSERT_*` before it could
    // push `shutdown`/`exit`, or a `runUntilExit` timeout) therefore used to
    // DEADLOCK the whole binary: no failure report, no other test, just a hung
    // process the CI eventually kills with no log. That is the
    // "silence instead of a verdict" shape this tree keeps deleting, and it was
    // one early `ASSERT` away in every existing e2e test.
    //
    // Closing the transport here — in the destructor BODY, which runs before
    // any member is destroyed — makes `readMessage()` return `Eof`, `run()`
    // return, and the future become ready. `close()` is idempotent, so the
    // normal shutdown+exit path is unaffected. `transport_` is owned by
    // `server_`'s `unique_ptr`, which is still alive at this point.
    //
    // ★ THE JOIN IS EXPLICIT NOW. It used to be implicit in `std::async`'s
    // future destructor; a `StackSizedThread` aborts rather than detaching if it
    // is destroyed while running, so the wait has to be stated. Same ordering as
    // before -- close first, THEN wait -- because waiting on a loop that is still
    // blocked in `readMessage()` is the deadlock this comment exists to prevent.
    ~LspTestHarness() {
        if (transport_) transport_->close();
        if (serverThread_.joinable()) {
            // The lambda already funnels every exception into the promise, so
            // this join does not throw; the catch is here so that a future
            // change to that lambda cannot turn a test failure into a
            // `std::terminate` out of a destructor.
            try {
                serverThread_.join();
            } catch (...) {   // NOLINT(bugprone-empty-catch)
            }
        }
    }

    LspTestHarness(LspTestHarness const&)            = delete;
    LspTestHarness& operator=(LspTestHarness const&) = delete;
    LspTestHarness(LspTestHarness&&)                 = delete;
    LspTestHarness& operator=(LspTestHarness&&)      = delete;

    void push(std::string body) {
        transport_->pushClientMessage(std::move(body));
    }

    // Block until the server's `run()` returns, OR the budget elapses. Caller is
    // expected to have queued an `exit` notif before calling this. Returns the
    // exit code, or -1 on expiry.
    //
    // ★ THE DEFAULT IS `kWaitBudget`, NOT A LITERAL — read the derivation in
    // `test_wait_budget.hpp`. It was `std::chrono::seconds(2)`, and ✔that reddened
    // the `linux-clang-asan` leg on CI run 32585879580 while the same sanitized
    // binary passed in 622 ms on an idle host and took 1912 ms under 3x CPU
    // contention. The wait returns the instant the server exits, so the budget
    // costs a healthy run nothing.
    //
    // ★★ AND EXPIRY REPORTS ITSELF. Every call site compares this against an
    // expected EXIT CODE, so a bare -1 reads as `Which is: -1` — a wrong exit
    // status, which is not what happened. The added failure names the real event;
    // the -1 return is kept so no call site has to change.
    // D-TEST-LSP-WAIT-DEADLINE-IS-SIZED-FOR-AN-IDLE-HOST
    [[nodiscard]] int runUntilExit(
        std::chrono::seconds timeout = dss::test_support::kWaitBudget) {
        if (exitFuture_.wait_for(timeout) != std::future_status::ready) {
            ADD_FAILURE() << "the LSP server did not return from run() within "
                          << timeout.count()
                          << "s of the `exit` notification; this is a TIMEOUT, "
                             "not an exit status — the -1 below is the sentinel";
            return -1;
        }
        return exitFuture_.get();
    }

    [[nodiscard]] std::vector<std::string> takeServerMessages() {
        return transport_->takeServerMessages();
    }

    [[nodiscard]] InMemoryTransport& transport() noexcept { return *transport_; }
    [[nodiscard]] SchemaCache&       schemaCache()    noexcept { return cache_; }

private:
    // ⚠ DECLARATION ORDER IS THE CONSTRUCTION ORDER, and it is load-bearing:
    // `serverThread_` is LAST because its callable touches `server_` and
    // `exitPromise_`, which must both be fully built before a worker can read
    // them. It is destroyed FIRST for the same reason -- though the destructor
    // body above has already joined it by then.
    InMemoryTransport* transport_; // owned by server_'s unique_ptr<LspTransport>
    SchemaCache        cache_;
    LspServer          server_;
    std::promise<int>  exitPromise_;
    std::future<int>   exitFuture_;
    substrate::StackSizedThread serverThread_;
};

// Canonical wire-message builders. These are used so heavily across
// the e2e + replay tests that inlining them would be pure noise.
[[nodiscard]] inline std::string lspInitialize(int id) {
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id)
         + R"(,"method":"initialize","params":{}})";
}

// `file:///…` URI for a local path — the spelling every LSP client uses for a
// workspace folder.
//
// ⚠ IT FORWARDS TO PRODUCTION NOW, AND THAT IS THE POINT
// (D-LSP-POSITIONS-RESOLVED-IN-SYNTHESIZED-PREPROCESSOR-COORDINATES). This used
// to be a SECOND implementation, and it was the ONLY one that existed anywhere:
// `src/` had `pathFromFileUri` but no inverse, so production could not name a
// file at all and `locationJson` stamped the request's uri onto every result —
// which is how a definition inside a header was reported as living in the open
// document. A helper that exists only in the test tree is a helper the product
// cannot use, and the tests then measure their own copy rather than the
// shipping one.
//
// The old body also did LESS than it claimed: no percent-encoding at all, so a
// fixture path containing a space produced a uri that `pathFromFileUri` decoded
// back to a DIFFERENT path — the round-trip this comment asserted was never
// actually closed. `dss::lsp::fileUriFromPath` does encode, and
// `WorkspaceProject.FileUriRoundTrip` pins the three shapes.
[[nodiscard]] inline std::string fileUriFromPath(
    std::filesystem::path const& p) {
    return dss::lsp::fileUriFromPath(p);
}

// `initialize` naming workspace folders — the ONLY message that carries them,
// and therefore the only way an editor can learn the workspace's compile
// target. `lspInitialize` (params `{}`) is the deliberate no-workspace control.
[[nodiscard]] inline std::string lspInitializeWithRoots(
    int id, std::vector<std::filesystem::path> const& roots) {
    std::string folders;
    for (auto const& r : roots) {
        if (!folders.empty()) folders += ",";
        folders += R"({"uri":")" + fileUriFromPath(r) + R"(","name":"ws"})";
    }
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id)
         + R"(,"method":"initialize","params":{"workspaceFolders":[)"
         + folders + R"(]}})";
}

[[nodiscard]] inline std::string lspShutdown(int id) {
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id)
         + R"(,"method":"shutdown","params":null})";
}

inline constexpr std::string_view lspExit =
    R"({"jsonrpc":"2.0","method":"exit","params":null})";

} // namespace dss::lsp::testing
