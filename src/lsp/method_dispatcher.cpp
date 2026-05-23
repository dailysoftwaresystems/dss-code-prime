#include "lsp/method_dispatcher.hpp"

#include "lsp/json_rpc.hpp"

#include <utility>
#include <variant>

namespace dss::lsp {

void MethodDispatcher::registerRequest(Method m, RequestHandler h) {
    requestHandlers_[m] = std::move(h);
}

void MethodDispatcher::registerNotification(Method m, NotificationHandler h) {
    notificationHandlers_[m] = std::move(h);
}

std::optional<std::string> MethodDispatcher::dispatch(IncomingMessage const& msg) {
    if (auto const* req = std::get_if<Request>(&msg)) {
        auto it = requestHandlers_.find(req->method);
        if (it == requestHandlers_.end() || req->method == Method::Unknown) {
            // JSON-RPC §5.1: -32601 Method not found.
            return JsonRpc::serializeError(req->id, -32601, "Method not found");
        }
        auto result = it->second(*req);
        if (!result.has_value()) {
            // Handler declined to respond now (e.g. async).
            return std::nullopt;
        }
        return JsonRpc::serializeResponse(req->id, *result);
    }
    auto const& notif = std::get<Notification>(msg);
    auto it = notificationHandlers_.find(notif.method);
    if (it == notificationHandlers_.end() || notif.method == Method::Unknown) {
        // LSP §3.1: silently drop unknown notifications.
        return std::nullopt;
    }
    it->second(notif);
    return std::nullopt;
}

} // namespace dss::lsp
