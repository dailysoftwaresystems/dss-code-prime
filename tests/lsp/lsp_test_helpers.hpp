#pragma once

#include "core/substrate/thread_pool.hpp"
#include "lsp/json_rpc.hpp"
#include "lsp/lsp_server.hpp"
#include "lsp/schema_cache.hpp"
#include "lsp/transport.hpp"
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
class LspTestHarness {
public:
    LspTestHarness()
        : transport_(new InMemoryTransport{})
        , server_(std::unique_ptr<LspTransport>{transport_},
                  std::make_unique<substrate::SynchronousExecutor>(),
                  cache_)
        , exitFuture_(std::async(std::launch::async, [this] {
              return server_.run();
          })) {}

    // ★★ THE HARNESS MUST SURVIVE A FAILING TEST, NOT HANG IT.
    // Members destruct in REVERSE declaration order, so `exitFuture_` — an
    // `std::async` future, whose destructor BLOCKS until the task finishes —
    // goes first, while the server thread is still parked in
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
    ~LspTestHarness() {
        if (transport_) transport_->close();
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
    InMemoryTransport* transport_; // owned by server_'s unique_ptr<LspTransport>
    SchemaCache        cache_;
    LspServer          server_;
    std::future<int>   exitFuture_;
};

// Canonical wire-message builders. These are used so heavily across
// the e2e + replay tests that inlining them would be pure noise.
[[nodiscard]] inline std::string lspInitialize(int id) {
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id)
         + R"(,"method":"initialize","params":{}})";
}

// `file:///…` URI for a local path — the spelling every LSP client uses for a
// workspace folder. Backslashes become forward slashes and a Windows drive path
// gains the extra leading slash the URI grammar requires (`C:\d` →
// `file:///C:/d`), so a fixture built here round-trips through
// `dss::lsp::pathFromFileUri`.
[[nodiscard]] inline std::string fileUriFromPath(
    std::filesystem::path const& p) {
    auto s = p.generic_string();
    return (!s.empty() && s.front() == '/') ? "file://" + s
                                            : "file:///" + s;
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
