// Pins for `JsonRpc` parse/serialize and the streaming framing
// helper `tryParseFramedMessage`. Regression risk lives in
// split-chunk reads, Content-Length variants, and LspId type
// handling.

#include "lsp/json_rpc.hpp"
#include "lsp/protocol.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

using dss::lsp::FrameScanState;
using dss::lsp::frameMessage;
using dss::lsp::IncomingMessage;
using dss::lsp::JsonRpc;
using dss::lsp::LspId;
using dss::lsp::Method;
using dss::lsp::Notification;
using dss::lsp::ParseErrorKind;
using dss::lsp::Request;
using dss::lsp::scanFramedMessage;
using dss::lsp::tryParseFramedMessage;

// ── JsonRpc::parse — request shapes ────────────────────────────────────

TEST(JsonRpc, ParsesInitializeRequest) {
    constexpr std::string_view body =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":null}})";
    auto msg = JsonRpc::parse(body);
    ASSERT_TRUE(msg.has_value());
    ASSERT_TRUE(std::holds_alternative<Request>(*msg));
    auto const& req = std::get<Request>(*msg);
    EXPECT_EQ(req.method, Method::Initialize);
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(req.id));
    EXPECT_EQ(std::get<std::int64_t>(req.id), 1);
    // params is rendered verbatim — handler re-parses.
    EXPECT_EQ(req.params, R"({"rootUri":null})");
}

TEST(JsonRpc, ParsesShutdownRequestWithStringId) {
    constexpr std::string_view body =
        R"({"jsonrpc":"2.0","id":"abc-42","method":"shutdown"})";
    auto msg = JsonRpc::parse(body);
    ASSERT_TRUE(msg.has_value());
    auto const& req = std::get<Request>(*msg);
    EXPECT_EQ(req.method, Method::Shutdown);
    ASSERT_TRUE(std::holds_alternative<std::string>(req.id));
    EXPECT_EQ(std::get<std::string>(req.id), "abc-42");
    EXPECT_EQ(req.params, "");
}

// ── JsonRpc::parse — notification shapes ───────────────────────────────

TEST(JsonRpc, ParsesNotificationWithNoId) {
    constexpr std::string_view body =
        R"({"jsonrpc":"2.0","method":"initialized","params":{}})";
    auto msg = JsonRpc::parse(body);
    ASSERT_TRUE(msg.has_value());
    ASSERT_TRUE(std::holds_alternative<Notification>(*msg));
    auto const& n = std::get<Notification>(*msg);
    EXPECT_EQ(n.method, Method::Initialized);
}

TEST(JsonRpc, ParsesNullIdAsNotification) {
    // LSP §3.1 permits null id on responses but not on requests; we
    // treat null id as a notification (no response expected).
    constexpr std::string_view body =
        R"({"jsonrpc":"2.0","id":null,"method":"exit"})";
    auto msg = JsonRpc::parse(body);
    ASSERT_TRUE(msg.has_value());
    EXPECT_TRUE(std::holds_alternative<Notification>(*msg));
}

// ── JsonRpc::parse — error paths ───────────────────────────────────────

TEST(JsonRpc, RejectsMalformedJson) {
    auto msg = JsonRpc::parse("{not valid");
    ASSERT_FALSE(msg.has_value());
    EXPECT_EQ(msg.error().kind, ParseErrorKind::InvalidJson);
}

TEST(JsonRpc, RejectsMissingJsonRpcVersion) {
    auto msg = JsonRpc::parse(R"({"id":1,"method":"initialize"})");
    ASSERT_FALSE(msg.has_value());
    EXPECT_EQ(msg.error().kind, ParseErrorKind::MissingJsonRpcVersion);
}

TEST(JsonRpc, RejectsMissingMethod) {
    auto msg = JsonRpc::parse(R"({"jsonrpc":"2.0","id":1})");
    ASSERT_FALSE(msg.has_value());
    EXPECT_EQ(msg.error().kind, ParseErrorKind::MissingMethod);
}

// Unknown method strings parse into `Method::Unknown` — the
// dispatcher (not the parser) decides how to respond.
TEST(JsonRpc, UnknownMethodParsesAsUnknownEnum) {
    auto msg = JsonRpc::parse(
        R"({"jsonrpc":"2.0","id":7,"method":"textDocument/codeAction"})");
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(std::get<Request>(*msg).method, Method::Unknown);
}

// ── JsonRpc::serialize* ────────────────────────────────────────────────

TEST(JsonRpc, SerializesResponseWithIntegerId) {
    const auto out = JsonRpc::serializeResponse(LspId{std::int64_t{42}},
                                                 R"({"ok":true})");
    EXPECT_EQ(out, R"({"jsonrpc":"2.0","id":42,"result":{"ok":true}})");
}

TEST(JsonRpc, SerializesResponseWithStringId) {
    const auto out = JsonRpc::serializeResponse(LspId{std::string{"x"}},
                                                 R"(null)");
    EXPECT_EQ(out, R"({"jsonrpc":"2.0","id":"x","result":null})");
}

TEST(JsonRpc, SerializesError) {
    const auto out = JsonRpc::serializeError(LspId{std::int64_t{3}},
                                              -32601, "Method not found");
    EXPECT_EQ(out,
        R"({"jsonrpc":"2.0","id":3,"error":{"code":-32601,"message":"Method not found"}})");
}

TEST(JsonRpc, SerializesNotification) {
    const auto out = JsonRpc::serializeNotification(
        "textDocument/publishDiagnostics", R"({"uri":"file:///x","diagnostics":[]})");
    EXPECT_EQ(out,
        R"({"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":{"uri":"file:///x","diagnostics":[]}})");
}

// ── frameMessage / tryParseFramedMessage ───────────────────────────────

TEST(JsonRpc, FrameMessageProducesContentLengthHeader) {
    const auto framed = frameMessage(R"({"x":1})");
    EXPECT_EQ(framed, "Content-Length: 7\r\n\r\n{\"x\":1}");
}

TEST(JsonRpc, FrameRoundTripsThroughTryParse) {
    const auto framed = frameMessage(R"({"x":1})");
    std::string body;
    const auto consumed = tryParseFramedMessage(framed, body);
    EXPECT_EQ(consumed, static_cast<std::int64_t>(framed.size()));
    EXPECT_EQ(body, R"({"x":1})");
}

TEST(JsonRpc, TryParseAcceptsLfOnlySeparator) {
    // Some clients emit `\n\n` instead of `\r\n\r\n`.
    const std::string framed = "Content-Length: 7\n\n{\"x\":1}";
    std::string body;
    const auto consumed = tryParseFramedMessage(framed, body);
    EXPECT_EQ(consumed, static_cast<std::int64_t>(framed.size()));
    EXPECT_EQ(body, R"({"x":1})");
}

TEST(JsonRpc, TryParseReturnsZeroOnIncompleteHeader) {
    std::string body;
    EXPECT_EQ(tryParseFramedMessage("Content-Length: 7\r\n", body), 0);
}

TEST(JsonRpc, TryParseReturnsZeroOnIncompleteBody) {
    std::string body;
    EXPECT_EQ(tryParseFramedMessage("Content-Length: 7\r\n\r\n{\"x\"", body), 0);
}

TEST(JsonRpc, TryParseConsumesExactlyOneFrameWhenMultiplePresent) {
    const auto framed = frameMessage(R"({"a":1})") + frameMessage(R"({"b":2})");
    std::string body;
    const auto consumed = tryParseFramedMessage(framed, body);
    EXPECT_GT(consumed, 0);
    EXPECT_EQ(body, R"({"a":1})");
    // Caller advances `framed` by `consumed` bytes; second call
    // returns the next message.
    std::string body2;
    const auto consumed2 = tryParseFramedMessage(
        std::string_view{framed}.substr(static_cast<std::size_t>(consumed)), body2);
    EXPECT_GT(consumed2, 0);
    EXPECT_EQ(body2, R"({"b":2})");
}

TEST(JsonRpc, TryParseRejectsMalformedContentLength) {
    std::string body;
    EXPECT_EQ(tryParseFramedMessage("Content-Length: abc\r\n\r\n{}", body), -1);
}

TEST(JsonRpc, TryParseRejectsMissingContentLength) {
    std::string body;
    EXPECT_EQ(tryParseFramedMessage("Content-Type: x\r\n\r\n{}", body), -1);
}

// ── scanFramedMessage — the `bytesNeeded` contract ─────────────────────
//
// `tryParseFramedMessage` collapses "incomplete" to a single 0, which is
// all a caller needs when it can read whatever it likes. A caller over a
// BLOCKING stream cannot: it must ask for a count that is certain to
// arrive, and these pins are the promise it relies on. See
// `StdioTransport::readMessage`.

TEST(JsonRpc, ScanReportsExactRemainderOfAPartialBody) {
    const auto framed = frameMessage(R"({"x":1})");   // 7-byte body
    std::string body;
    // Everything but the last three body bytes.
    const auto scan = scanFramedMessage(
        std::string_view{framed}.substr(0, framed.size() - 3), body);
    EXPECT_EQ(scan.state, FrameScanState::Incomplete);
    EXPECT_EQ(scan.bytesNeeded, 3u);
    EXPECT_EQ(scan.consumed, 0u);
}

TEST(JsonRpc, ScanReportsWholeBodyWhenOnlyTheHeaderHasArrived) {
    std::string body;
    const auto scan = scanFramedMessage("Content-Length: 42\r\n\r\n", body);
    EXPECT_EQ(scan.state, FrameScanState::Incomplete);
    EXPECT_EQ(scan.bytesNeeded, 42u);
}

TEST(JsonRpc, ScanCannotSizeAnUnterminatedHeader) {
    // ZERO here does NOT mean "nothing needed" — it means "not knowable
    // yet". A reader must take one byte and ask again; taking a fixed
    // chunk is exactly the defect this field exists to prevent.
    std::string body;
    const auto scan = scanFramedMessage("Content-Length: 7\r\n", body);
    EXPECT_EQ(scan.state, FrameScanState::Incomplete);
    EXPECT_EQ(scan.bytesNeeded, 0u);
}

TEST(JsonRpc, ScanReportsConsumedForOneFrameOfMany) {
    const auto first  = frameMessage(R"({"a":1})");
    const auto framed = first + frameMessage(R"({"b":2})");
    std::string body;
    const auto scan = scanFramedMessage(framed, body);
    EXPECT_EQ(scan.state, FrameScanState::Complete);
    EXPECT_EQ(scan.consumed, first.size());
    EXPECT_EQ(scan.bytesNeeded, 0u);
    EXPECT_EQ(body, R"({"a":1})");
}

TEST(JsonRpc, ScanRefusesAnUnterminatedHeaderPastTheCap) {
    // Without the cap this is an unbounded buffer with no error path:
    // the reader keeps asking for one more byte until the process dies.
    std::string body;
    const std::string junk(dss::lsp::kMaxFrameHeaderBytes + 1, 'A');
    EXPECT_EQ(scanFramedMessage(junk, body).state,
              FrameScanState::MalformedHeader);
    // One byte under the cap is still merely incomplete.
    const std::string shorter(dss::lsp::kMaxFrameHeaderBytes, 'A');
    EXPECT_EQ(scanFramedMessage(shorter, body).state,
              FrameScanState::Incomplete);
}

TEST(JsonRpc, ScanRefusesAnOversizedTerminatedHeader) {
    std::string body;
    const std::string header =
        "Content-Length: 2\r\nX-Pad: " + std::string(dss::lsp::kMaxFrameHeaderBytes, 'p')
        + "\r\n\r\n{}";
    EXPECT_EQ(scanFramedMessage(header, body).state,
              FrameScanState::MalformedHeader);
}
