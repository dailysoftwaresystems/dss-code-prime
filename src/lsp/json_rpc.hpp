#pragma once

#include "core/export.hpp"
#include "lsp/protocol.hpp"

#include <cstddef>
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

// The largest header region (everything before the `\r\n\r\n`
// separator) this framing will accept. A real client's header is
// well under a hundred bytes; anything past this is a peer that will
// never terminate its header, and the alternative to refusing is an
// unbounded buffer that grows until the process dies. Refusing is the
// fail-loud direction: the reader gets `MalformedHeader` and can say
// so, instead of hanging with no diagnosis available to anyone.
inline constexpr std::size_t kMaxFrameHeaderBytes = 8192;

enum class FrameScanState : std::uint8_t {
    Complete,         // a whole frame is present; see `consumed`
    Incomplete,       // more bytes are required; see `bytesNeeded`
    MalformedHeader,  // no Content-Length, unparseable value, or over the cap
};

// What one scan of a streaming buffer learned about the frame at its
// front. The `bytesNeeded` field is the reason this type exists: LSP
// framing is SELF-DESCRIBING, so once the header has been read the
// exact size of the remainder is known — and a reader over a blocking
// stream must ask for that exact count, never a fixed chunk size it
// merely hopes will arrive. See `scanFramedMessage`.
struct DSS_EXPORT FrameScan {
    FrameScanState state = FrameScanState::Incomplete;

    // `Complete` only: bytes to erase from the front of the buffer
    // (header + separator + body). Zero in every other state.
    std::size_t consumed = 0;

    // `Incomplete` only: how many ADDITIONAL bytes the frame is known
    // to require. ZERO means the header separator has not arrived yet,
    // so the requirement is not yet knowable — a reader must then take
    // the only amount a well-behaved peer is certain to send next,
    // which is one byte. Zero in every other state.
    std::size_t bytesNeeded = 0;
};

// Scan `input` for one Content-Length framed message. On
// `FrameScanState::Complete`, `outBody` holds the JSON body with the
// header stripped. `outBody` is not modified in any other state.
//
// THE ONE HEADER PARSER. `tryParseFramedMessage` below is a thin
// projection of this result, and `StdioTransport::readMessage` drives
// its byte reads from `bytesNeeded` — so "what does an LSP frame look
// like" is answered in exactly one place, and a reader can never hold
// a drifted second opinion about how many bytes a frame still owes it.
[[nodiscard]] DSS_EXPORT FrameScan scanFramedMessage(
    std::string_view input,
    std::string&     outBody);

// Attempt to parse a Content-Length framed message from `input`.
// On success, sets `outBody` to the JSON body (header stripped) and
// returns the total number of bytes consumed from `input` (header
// + body). Returns 0 if `input` does not yet contain a complete
// frame (caller should read more bytes). Returns -1 on framing
// error (malformed header).
//
// A projection of `scanFramedMessage` that discards `bytesNeeded`.
// Callers that must SIZE a read cannot use it — see the transport.
[[nodiscard]] DSS_EXPORT std::int64_t tryParseFramedMessage(
    std::string_view input,
    std::string&     outBody);

} // namespace dss::lsp
