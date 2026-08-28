#include "lsp/json_rpc.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace dss::lsp {

namespace {

using json = nlohmann::json;

// Extract the `id` from a JSON-RPC message into the typed variant.
// Accepts number (integer / float — we coerce to int64), string, or
// absent (notification). LSP §3.1 permits null `id` on responses
// but not on requests; we treat null as absent (notification).
[[nodiscard]] LspId parseId(json const& obj) {
    auto it = obj.find("id");
    if (it == obj.end() || it->is_null()) {
        return std::monostate{};
    }
    if (it->is_number_integer()) {
        return it->get<std::int64_t>();
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    if (it->is_number_float()) {
        return static_cast<std::int64_t>(it->get<double>());
    }
    return std::monostate{};
}

// Render `params` back to a JSON text string. The dispatcher hands
// this off verbatim to handlers; each handler re-parses what it
// needs. This is the "JSON text at the boundary" convention that
// keeps nlohmann out of every other TU.
[[nodiscard]] std::string renderParams(json const& obj) {
    auto it = obj.find("params");
    if (it == obj.end() || it->is_null()) return {};
    return it->dump();
}

[[nodiscard]] std::string renderId(LspId const& id) {
    if (std::holds_alternative<std::monostate>(id)) return "null";
    if (std::holds_alternative<std::int64_t>(id)) {
        return std::to_string(std::get<std::int64_t>(id));
    }
    // String id — quote and escape via nlohmann's serializer so
    // unusual characters (control bytes, quotes, backslashes) are
    // emitted correctly. JSON string fragments are well-formed JSON
    // values, so this dump is safe to splice into the envelope.
    return json(std::get<std::string>(id)).dump();
}

} // namespace

std::expected<IncomingMessage, ParseError> JsonRpc::parse(std::string_view body) {
    json obj;
    try {
        obj = json::parse(body);
    } catch (json::parse_error const& e) {
        return std::unexpected(ParseError{ParseErrorKind::InvalidJson, e.what()});
    } catch (std::exception const& e) {
        return std::unexpected(ParseError{ParseErrorKind::InvalidJson, e.what()});
    }

    if (!obj.is_object() || !obj.contains("jsonrpc")
        || !obj.at("jsonrpc").is_string()
        || obj.at("jsonrpc").get<std::string>() != "2.0") {
        return std::unexpected(ParseError{
            ParseErrorKind::MissingJsonRpcVersion,
            "expected \"jsonrpc\":\"2.0\""});
    }

    auto methodIt = obj.find("method");
    if (methodIt == obj.end() || !methodIt->is_string()) {
        return std::unexpected(ParseError{
            ParseErrorKind::MissingMethod,
            "expected string \"method\""});
    }
    const auto methodStr = methodIt->get<std::string>();
    const auto method    = parseMethod(methodStr);
    auto       params    = renderParams(obj);

    // Notification iff `id` is absent (or null). Requests carry an
    // id even with `Method::Unknown` — the dispatcher emits the
    // `-32601 Method not found` error response in that case.
    auto idIt = obj.find("id");
    const bool isNotification = (idIt == obj.end() || idIt->is_null());
    if (isNotification) {
        return Notification{method, std::move(params)};
    }
    return Request{method, parseId(obj), std::move(params)};
}

std::string JsonRpc::serializeResponse(LspId const& id, std::string_view resultJson) {
    // Build the envelope by string assembly rather than nlohmann
    // because `resultJson` is already a serialized JSON value and
    // re-parsing+serializing would be wasteful (and would normalize
    // formatting in ways the golden-file replay harness would
    // detect as drift).
    return std::format(R"({{"jsonrpc":"2.0","id":{},"result":{}}})",
                       renderId(id), resultJson);
}

std::string JsonRpc::serializeError(LspId const& id, int code, std::string_view message) {
    return std::format(R"({{"jsonrpc":"2.0","id":{},"error":{{"code":{},"message":{}}}}})",
                       renderId(id), code, json(std::string{message}).dump());
}

std::string JsonRpc::serializeNotification(std::string_view method, std::string_view paramsJson) {
    return std::format(R"({{"jsonrpc":"2.0","method":{},"params":{}}})",
                       json(std::string{method}).dump(), paramsJson);
}

std::string frameMessage(std::string_view body) {
    return std::format("Content-Length: {}\r\n\r\n{}", body.size(), body);
}

FrameScan scanFramedMessage(std::string_view input, std::string& outBody) {
    constexpr FrameScan kMalformed{FrameScanState::MalformedHeader, 0, 0};
    // `bytesNeeded == 0` on an Incomplete scan is the "not yet knowable"
    // answer: the separator has not arrived, so nothing can be said about
    // the body's size. The reader's contract for that case is in the
    // header — take one byte and ask again.
    constexpr FrameScan kNeedHeaderBytes{FrameScanState::Incomplete, 0, 0};

    // Find the header/body separator. LSP §6.1 mandates `\r\n\r\n`
    // but some clients emit just `\n\n` — accept both.
    auto headerEnd = input.find("\r\n\r\n");
    std::size_t sepLen = 4;
    if (headerEnd == std::string_view::npos) {
        headerEnd = input.find("\n\n");
        sepLen    = 2;
        if (headerEnd == std::string_view::npos) {
            // A header that is already longer than any header can be is a
            // peer that will never terminate one. Refuse LOUDLY rather than
            // keep buffering: the caller's alternative is unbounded growth
            // with no error it could ever report.
            if (input.size() > kMaxFrameHeaderBytes) return kMalformed;
            return kNeedHeaderBytes; // incomplete frame; caller reads more bytes
        }
    }
    if (headerEnd > kMaxFrameHeaderBytes) return kMalformed;

    // Parse the headers. Only `Content-Length` is required; other
    // headers are tolerated and ignored.
    const auto headerSv = input.substr(0, headerEnd);
    std::size_t contentLength = 0;
    bool gotLength = false;
    std::size_t pos = 0;
    while (pos < headerSv.size()) {
        auto eol = headerSv.find_first_of("\r\n", pos);
        if (eol == std::string_view::npos) eol = headerSv.size();
        const auto line = headerSv.substr(pos, eol - pos);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            auto name = line.substr(0, colon);
            auto value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            if (name == "Content-Length") {
                std::size_t n = 0;
                auto [ptr, ec] = std::from_chars(value.data(),
                                                  value.data() + value.size(), n);
                if (ec == std::errc{} && ptr == value.data() + value.size()) {
                    contentLength = n;
                    gotLength     = true;
                } else {
                    return kMalformed; // malformed Content-Length
                }
            }
        }
        // Advance past the EOL — single `\n` or `\r\n`.
        pos = (eol < headerSv.size() && headerSv[eol] == '\r'
               && eol + 1 < headerSv.size() && headerSv[eol + 1] == '\n')
              ? eol + 2
              : eol + 1;
    }
    if (!gotLength) return kMalformed; // header without Content-Length

    const auto bodyStart = headerEnd + sepLen;
    const auto frameEnd  = bodyStart + contentLength;
    if (input.size() < frameEnd) {
        // The EXACT remainder. This is the number the transport reads,
        // and the whole reason a fixed chunk size was wrong: a reader
        // that asks a blocking stream for more than this waits for bytes
        // the peer has no reason to send.
        return FrameScan{FrameScanState::Incomplete, 0,
                         frameEnd - input.size()};
    }
    outBody.assign(input.data() + bodyStart, contentLength);
    return FrameScan{FrameScanState::Complete, frameEnd, 0};
}

std::int64_t tryParseFramedMessage(std::string_view input, std::string& outBody) {
    const auto scan = scanFramedMessage(input, outBody);
    switch (scan.state) {
        case FrameScanState::Complete:
            return static_cast<std::int64_t>(scan.consumed);
        case FrameScanState::MalformedHeader:
            return -1;
        case FrameScanState::Incomplete:
            break;
    }
    return 0;
}

} // namespace dss::lsp
