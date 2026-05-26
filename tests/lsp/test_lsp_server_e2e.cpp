// End-to-end LSP server tests driven through `InMemoryTransport` +
// `SynchronousExecutor`. Parse jobs land before the next message
// is read, so message ordering is deterministic without sleeps.

#include "lsp_test_helpers.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>

using dss::lsp::testing::LspTestHarness;
using dss::lsp::testing::lspExit;
using dss::lsp::testing::lspInitialize;
using dss::lsp::testing::lspShutdown;
using json = nlohmann::json;

namespace {

[[nodiscard]] std::string lspDidOpen(std::string_view uri,
                                      int               version,
                                      std::string_view  text) {
    std::string s =
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")";
    s += uri;
    s += R"(","languageId":"toy","version":)";
    s += std::to_string(version);
    s += R"(,"text":")";
    s += text;
    s += R"("}}})";
    return s;
}

} // namespace

// ── Lifecycle ─────────────────────────────────────────────────────────

TEST(LspServerE2E, InitializeRespondsWithUtf16PositionEncoding) {
    LspTestHarness h;
    h.push(lspInitialize(1));
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    EXPECT_EQ(h.runUntilExit(), 0);

    auto msgs = h.takeServerMessages();
    ASSERT_EQ(msgs.size(), 2u);
    auto init = json::parse(msgs[0]);
    EXPECT_EQ(init.at("jsonrpc"), "2.0");
    EXPECT_EQ(init.at("id"), 1);
    auto const& caps = init.at("result").at("capabilities");
    EXPECT_EQ(caps.at("positionEncoding"), "utf-16");
    EXPECT_EQ(caps.at("textDocumentSync"), 1);
    auto ack = json::parse(msgs[1]);
    EXPECT_EQ(ack.at("id"), 2);
    EXPECT_TRUE(ack.at("result").is_null());
}

TEST(LspServerE2E, MalformedJsonReturnsParseErrorWithNullId) {
    LspTestHarness h;
    h.push(lspInitialize(1));
    h.push("{not valid json");
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    EXPECT_EQ(h.runUntilExit(), 0);

    auto msgs = h.takeServerMessages();
    ASSERT_EQ(msgs.size(), 3u);
    auto err = json::parse(msgs[1]);
    EXPECT_TRUE(err.at("id").is_null());
    EXPECT_EQ(err.at("error").at("code"), -32700);
}

TEST(LspServerE2E, ExitWithoutShutdownReturnsExitCode1) {
    // LSP §3.6: `exit` arriving without prior `shutdown` is an
    // error and the process exit code must be 1.
    LspTestHarness h;
    h.push(lspInitialize(1));
    h.push(std::string{lspExit});
    EXPECT_EQ(h.runUntilExit(), 1);
}

// ── Document sync ─────────────────────────────────────────────────────

TEST(LspServerE2E, DidChangeRepublishesWithMonotonicVersions) {
    LspTestHarness h;
    h.push(lspInitialize(1));
    h.push(lspDidOpen("file:///e.toy", 1, "1;"));
    h.push(
        R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{)"
        R"("textDocument":{"uri":"file:///e.toy","version":2},)"
        R"("contentChanges":[{"text":"2;"}]}})");
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    EXPECT_EQ(h.runUntilExit(), 0);

    // Exact ordering: init-response, publishDiag(v1), publishDiag(v2), shutdown-ack.
    auto msgs = h.takeServerMessages();
    ASSERT_EQ(msgs.size(), 4u);
    auto p1 = json::parse(msgs[1]);
    auto p2 = json::parse(msgs[2]);
    EXPECT_EQ(p1.at("method"), "textDocument/publishDiagnostics");
    EXPECT_EQ(p1.at("params").at("version"), 1);
    EXPECT_EQ(p1.at("params").at("uri"), "file:///e.toy");
    EXPECT_EQ(p2.at("method"), "textDocument/publishDiagnostics");
    EXPECT_EQ(p2.at("params").at("version"), 2);
    EXPECT_EQ(p2.at("params").at("uri"), "file:///e.toy");
}

TEST(LspServerE2E, DidOpenPublishesDiagnosticsForToySource) {
    LspTestHarness h;
    h.push(lspInitialize(1));
    // "1 + 2;" — toy grammar declares no numberStyle, so the tokenizer
    // rejects `1` and `2` as illegal characters (P_IllegalChar). The
    // parser then sees those Error tokens and emits P_NoAlternativeMatched
    // when scanning for a statement starter. Both flavors land in the
    // published-diagnostics array (F4: LSP now threads tokenizer
    // diagnostics into the Parser).
    h.push(lspDidOpen("file:///hello.toy", 1, "1 + 2;"));
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    EXPECT_EQ(h.runUntilExit(), 0);

    auto msgs = h.takeServerMessages();
    ASSERT_EQ(msgs.size(), 3u);
    auto pub = json::parse(msgs[1]);
    EXPECT_EQ(pub.at("method"), "textDocument/publishDiagnostics");
    auto const& params = pub.at("params");
    EXPECT_EQ(params.at("uri"), "file:///hello.toy");
    EXPECT_EQ(params.at("version"), 1);
    auto const& diags = params.at("diagnostics");
    ASSERT_TRUE(diags.is_array());
    EXPECT_GE(diags.size(), 1u);
    // Lexer diagnostics are produced first (they walk the source
    // left-to-right) and the LSP layer concatenates them ahead of
    // the parser's diagnostics.
    EXPECT_EQ(diags[0].at("code"), "P_IllegalChar");
    // The parser-side P_NoAlternativeMatched is still present in
    // the array; scan for it explicitly.
    bool sawParser = false;
    for (auto const& d : diags) {
        if (d.at("code") == "P_NoAlternativeMatched") { sawParser = true; break; }
    }
    EXPECT_TRUE(sawParser);
}

TEST(LspServerE2E, DidOpenWithoutExtensionPublishesEmptyDiagnostics) {
    // Pins current behavior: a URI with no recognized extension
    // resolves to no schema, so the parse worker publishes an
    // empty diagnostics array (no schema → nothing to validate).
    LspTestHarness h;
    h.push(lspInitialize(1));
    h.push(lspDidOpen("file:///untitled-1", 1, "anything"));
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    EXPECT_EQ(h.runUntilExit(), 0);

    auto msgs = h.takeServerMessages();
    ASSERT_EQ(msgs.size(), 3u);
    auto pub = json::parse(msgs[1]);
    EXPECT_EQ(pub.at("method"), "textDocument/publishDiagnostics");
    EXPECT_EQ(pub.at("params").at("uri"), "file:///untitled-1");
    EXPECT_TRUE(pub.at("params").at("diagnostics").is_array());
    EXPECT_EQ(pub.at("params").at("diagnostics").size(), 0u);
}

// ── Semantic stub handlers ────────────────────────────────────────────

namespace {

struct StubCase {
    std::string_view method;
    std::string_view expectedResult; // exact JSON the server must emit
};

[[nodiscard]] std::string lspSemanticRequest(std::string_view method, int id) {
    std::string s = R"({"jsonrpc":"2.0","id":)";
    s += std::to_string(id);
    s += R"(,"method":")";
    s += method;
    s += R"(","params":{"textDocument":{"uri":"file:///x.toy"}}})";
    return s;
}

void drive(StubCase const& c) {
    LspTestHarness h;
    h.push(lspInitialize(1));
    h.push(lspSemanticRequest(c.method, 7));
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    EXPECT_EQ(h.runUntilExit(), 0);

    auto msgs = h.takeServerMessages();
    ASSERT_EQ(msgs.size(), 3u) << "init + stub + shutdown-ack";
    auto reply       = json::parse(msgs[1]);
    auto expectedRes = json::parse(c.expectedResult);
    EXPECT_EQ(reply.at("jsonrpc"), "2.0");
    EXPECT_EQ(reply.at("id"),      7);
    EXPECT_EQ(reply.at("result"),  expectedRes);
    EXPECT_FALSE(reply.contains("error")) << "method " << c.method;
}

} // namespace

TEST(LspServerE2E, HoverStubReturnsNull) {
    drive({"textDocument/hover", "null"});
}
TEST(LspServerE2E, CompletionStubReturnsNull) {
    drive({"textDocument/completion", "null"});
}
TEST(LspServerE2E, DefinitionStubReturnsNull) {
    drive({"textDocument/definition", "null"});
}
TEST(LspServerE2E, ReferencesStubReturnsEmptyArray) {
    drive({"textDocument/references", "[]"});
}
TEST(LspServerE2E, RenameStubReturnsNull) {
    drive({"textDocument/rename", "null"});
}
TEST(LspServerE2E, SignatureHelpStubReturnsNull) {
    drive({"textDocument/signatureHelp", "null"});
}

TEST(LspServerE2E, InitializeAdvertisesAllStubCapabilities) {
    LspTestHarness h;
    h.push(lspInitialize(1));
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    EXPECT_EQ(h.runUntilExit(), 0);

    auto msgs = h.takeServerMessages();
    ASSERT_EQ(msgs.size(), 2u);
    auto reply = json::parse(msgs[0]);
    auto const& caps = reply.at("result").at("capabilities");
    EXPECT_EQ(caps.at("hoverProvider"),         true);
    EXPECT_EQ(caps.at("completionProvider"),    json::object());
    EXPECT_EQ(caps.at("definitionProvider"),    true);
    EXPECT_EQ(caps.at("referencesProvider"),    true);
    EXPECT_EQ(caps.at("renameProvider"),        true);
    EXPECT_EQ(caps.at("signatureHelpProvider"), json::object());
}

TEST(LspServerE2E, UnknownRequestReturnsMethodNotFound) {
    LspTestHarness h;
    h.push(lspInitialize(1));
    h.push(R"({"jsonrpc":"2.0","id":99,"method":"made/up/method","params":{}})");
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    EXPECT_EQ(h.runUntilExit(), 0);

    auto msgs = h.takeServerMessages();
    ASSERT_EQ(msgs.size(), 3u);
    auto err = json::parse(msgs[1]);
    EXPECT_EQ(err.at("id"), 99);
    EXPECT_EQ(err.at("error").at("code"), -32601);
}
