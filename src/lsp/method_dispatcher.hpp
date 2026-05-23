#pragma once

#include "core/export.hpp"
#include "lsp/protocol.hpp"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

// Method dispatcher — routes `IncomingMessage` to a registered
// handler. Splits request and notification registration because
// LSP §3.1 mandates different fallback behavior for each: unknown
// requests return `-32601 Method not found`; unknown notifications
// are silently dropped.
//
// Handlers return `std::optional<std::string>`:
//   - For requests: empty optional means the handler chose not to
//     respond (rare; usually represents a long-running async op
//     whose response is published later). A populated optional is
//     the JSON-encoded response body — the dispatcher wraps it in
//     the JSON-RPC envelope via `JsonRpc::serializeResponse`.
//   - For notifications: the optional is ignored (no response is
//     ever sent for a notification).
//
// Not thread-safe — designed to be called from the single reader
// thread. Worker threads do not call dispatch directly.

namespace dss::lsp {

class DSS_EXPORT MethodDispatcher {
public:
    using RequestHandler      = std::function<std::optional<std::string>(Request const&)>;
    using NotificationHandler = std::function<void(Notification const&)>;

    MethodDispatcher() = default;

    // Register a handler for a request method. Duplicate registration
    // for the same method is a wiring bug and fatal-aborts — silent
    // overwrite would let a real handler be masked by a stub (or
    // vice versa) at server-construction time.
    void registerRequest(Method m, RequestHandler h);

    // Register a handler for a notification method. Duplicate
    // registration fatal-aborts, same rationale as registerRequest.
    void registerNotification(Method m, NotificationHandler h);

    // Dispatch an incoming message.
    //   - Request: looks up handler; if absent OR method is Unknown,
    //     returns a serialized `-32601 Method not found` error
    //     response. Otherwise calls the handler and wraps its result
    //     via `JsonRpc::serializeResponse`.
    //   - Notification: looks up handler; if absent OR method is
    //     Unknown, returns `std::nullopt` silently. Otherwise calls
    //     the handler and returns `std::nullopt`.
    [[nodiscard]] std::optional<std::string> dispatch(IncomingMessage const& msg);

private:
    std::unordered_map<Method, RequestHandler>      requestHandlers_;
    std::unordered_map<Method, NotificationHandler> notificationHandlers_;
};

} // namespace dss::lsp
