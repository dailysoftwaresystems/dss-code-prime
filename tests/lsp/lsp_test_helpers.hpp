#pragma once

#include "lsp/json_rpc.hpp"
#include "lsp/transport.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
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

} // namespace dss::lsp::testing
