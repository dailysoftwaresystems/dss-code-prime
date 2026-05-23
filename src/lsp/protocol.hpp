#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// LSP protocol value types — the C++ representation of the JSON-RPC
// messages this server reads / writes. NO `<nlohmann/json.hpp>` in
// this header; the JSON layer is isolated to `json_rpc.cpp` and the
// per-method handler `.cpp` files. Consumers of `protocol.hpp` see
// only `std`/strong-ID types.
//
// LSP §3 message taxonomy:
//   - Request:      jsonrpc + id + method + params; expects a Response.
//   - Notification: jsonrpc + method + params; expects nothing back.
//   - Response:     jsonrpc + id + (result XOR error).
//
// `IncomingMessage` is the variant the dispatcher works with. The
// outgoing `Response`/`Notification` are built by per-handler logic
// and serialized via `JsonRpc::serializeResponse` /
// `serializeNotification`.

namespace dss::lsp {

// Strongly-typed scoped method id. Strings are matched once at the
// dispatcher boundary; the rest of the server works with the enum.
// `Unknown` is the sentinel — dispatch turns it into the appropriate
// LSP error (`-32601 Method not found`) for requests and silently
// drops it for notifications, per LSP §3.1.
enum class Method : std::uint8_t {
    Unknown = 0,
    // Lifecycle
    Initialize,
    Initialized,
    Shutdown,
    Exit,
    // Document sync
    TextDocumentDidOpen,
    TextDocumentDidChange,
    TextDocumentDidClose,
    TextDocumentDidSave,
    // Semantic request methods — handlers live in `LspServer`.
    TextDocumentHover,
    TextDocumentCompletion,
    TextDocumentDefinition,
    TextDocumentReferences,
    TextDocumentRename,
    TextDocumentSignatureHelp,
};

// LSP `id` can be a number, a string, or absent (notifications).
// `std::monostate` represents the absent case.
using LspId = std::variant<std::int64_t, std::string, std::monostate>;

// MessagePayload is the raw JSON text of the `params` field. Each
// handler parses it independently — keeping nlohmann out of this
// header is what forces this. Callers that want structured access
// invoke nlohmann inside their own `.cpp`.
using MessagePayload = std::string;

struct DSS_EXPORT Request {
    Method         method = Method::Unknown;
    LspId          id;
    MessagePayload params;
};

struct DSS_EXPORT Notification {
    Method         method = Method::Unknown;
    MessagePayload params;
};

using IncomingMessage = std::variant<Request, Notification>;

// LSP `Position` — 0-based line + character. `character` is encoded
// in UTF-16 code units per the encoding negotiated during
// `initialize` (PA5a advertises `positionEncoding: "utf-16"`).
struct DSS_EXPORT Position {
    std::uint32_t line      = 0;
    std::uint32_t character = 0;
};

struct DSS_EXPORT Range {
    Position start;
    Position end;
};

// LSP severities are 1-indexed (LSP §6.5). The numeric values match
// the wire protocol so JSON serialization is a direct cast.
enum class DiagnosticSeverity : std::uint8_t {
    Error       = 1,
    Warning     = 2,
    Information = 3,
    Hint        = 4,
};

struct DSS_EXPORT Diagnostic {
    Range              range;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string        code;     // e.g. "P_UnexpectedToken"
    std::string        source = "dss-code-prime";
    std::string        message;
};

struct DSS_EXPORT PublishDiagnosticsParams {
    std::string                  uri;
    std::optional<std::int32_t>  version;
    std::vector<Diagnostic>      diagnostics;
};

// Helper: parse a method string into the enum. Returns
// `Method::Unknown` for any string not in the scoped set. Pure
// function; safe to call from any thread.
[[nodiscard]] DSS_EXPORT Method parseMethod(std::string_view raw) noexcept;

// Helper: render an enum back to its wire string. Returns the
// canonical string for known methods, empty string_view for
// `Unknown`.
[[nodiscard]] DSS_EXPORT std::string_view methodName(Method m) noexcept;

} // namespace dss::lsp
