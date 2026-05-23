#pragma once

#include "core/export.hpp"
#include "lsp/protocol.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

// JSON-RPC 2.0 / LSP §6 framing + parsing. The framing reads
// `Content-Length: N\r\n\r\n<json>` envelopes from a transport and
// hands the JSON body to the dispatcher. nlohmann/json is isolated
// to `json_rpc.cpp` and the per-handler `.cpp` files — this header
// exposes only `std::string` and `lsp::*` value types.
//
// Parse / serialize are static methods on `JsonRpc` because they
// hold no state. The transport layer (see `transport.hpp`) handles
// the byte stream; the dispatcher (see `method_dispatcher.hpp`)
// owns the parsed `IncomingMessage`.

namespace dss::lsp {

enum class ParseErrorKind : std::uint8_t {
    InvalidJson,
    MissingJsonRpcVersion,
    MissingMethod,
};

struct DSS_EXPORT ParseError {
    ParseErrorKind kind;
    std::string    detail;
};

class DSS_EXPORT JsonRpc {
public:
    JsonRpc() = delete;

    // Parse a complete JSON body into an IncomingMessage. The
    // returned message's `params` field carries the raw JSON text
    // of the `params` member (or empty string if absent) — each
    // handler does its own structured parse.
    //
    // Errors: malformed JSON, missing "jsonrpc" field, missing
    // "method" on a non-response object. Unknown method strings do
    // NOT error here — they parse into `Method::Unknown` and the
    // dispatcher handles them per LSP §3.1.
    [[nodiscard]] static std::expected<IncomingMessage, ParseError>
        parse(std::string_view body);

    // Serialize a successful response with a pre-built `result`
    // JSON text. `resultJson` is inserted verbatim — callers must
    // ensure it is valid JSON (typically built via nlohmann in
    // their handler `.cpp`).
    [[nodiscard]] static std::string serializeResponse(
        LspId const&     id,
        std::string_view resultJson);

    // Serialize an error response. `code` follows JSON-RPC 2.0
    // §5.1 (`-32700 Parse error`, `-32600 Invalid Request`,
    // `-32601 Method not found`, `-32602 Invalid params`,
    // `-32603 Internal error`).
    [[nodiscard]] static std::string serializeError(
        LspId const&     id,
        int              code,
        std::string_view message);

    // Serialize a notification (no `id` field). `method` is the
    // wire-protocol string (e.g. `"textDocument/publishDiagnostics"`).
    [[nodiscard]] static std::string serializeNotification(
        std::string_view method,
        std::string_view paramsJson);
};

// Wrap a payload in the LSP `Content-Length: N\r\n\r\n<body>`
// framing. Returns the full byte string ready to write to stdout.
// Pure function; no I/O.
[[nodiscard]] DSS_EXPORT std::string frameMessage(std::string_view body);

// Attempt to parse a Content-Length framed message from `input`.
// On success, sets `outBody` to the JSON body (header stripped) and
// returns the total number of bytes consumed from `input` (header
// + body). Returns 0 if `input` does not yet contain a complete
// frame (caller should read more bytes). Returns -1 on framing
// error (malformed header). The transport layer uses this to peel
// messages from a streaming buffer.
[[nodiscard]] DSS_EXPORT std::int64_t tryParseFramedMessage(
    std::string_view input,
    std::string&     outBody);

} // namespace dss::lsp
