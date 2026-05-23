#pragma once

#include "lsp/json_rpc.hpp"
#include "lsp/lsp_server.hpp"
#include "lsp/schema_cache.hpp"
#include "lsp/thread_pool.hpp"
#include "lsp/transport.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
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
                  std::make_unique<SynchronousExecutor>(),
                  cache_)
        , exitFuture_(std::async(std::launch::async, [this] {
              return server_.run();
          })) {}

    LspTestHarness(LspTestHarness const&)            = delete;
    LspTestHarness& operator=(LspTestHarness const&) = delete;
    LspTestHarness(LspTestHarness&&)                 = delete;
    LspTestHarness& operator=(LspTestHarness&&)      = delete;

    void push(std::string body) {
        transport_->pushClientMessage(std::move(body));
    }

    // Block until the server's `run()` returns, OR the timeout
    // elapses. Caller is expected to have queued an `exit` notif
    // before calling this. Returns the exit code.
    [[nodiscard]] int runUntilExit(std::chrono::seconds timeout =
                                       std::chrono::seconds(2)) {
        if (exitFuture_.wait_for(timeout) != std::future_status::ready) {
            return -1; // timed out — caller should ASSERT_NE(-1, ...)
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

[[nodiscard]] inline std::string lspShutdown(int id) {
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id)
         + R"(,"method":"shutdown","params":null})";
}

inline constexpr std::string_view lspExit =
    R"({"jsonrpc":"2.0","method":"exit","params":null})";

} // namespace dss::lsp::testing
