// SE7 acceptance: the LSP semantic request handlers (hover / definition /
// references / rename / completion / signatureHelp) backed by the cached
// SemanticModel. Driven end-to-end through InMemoryTransport +
// SynchronousExecutor, so the parse+analyze worker runs inline before the
// next message is read (deterministic, no sleeps).
//
// Documents use the shipped c-subset grammar (functions + locals + calls),
// resolved by the `.c` URI extension via the SchemaCache.
//
// Tests use STRICT asserts — exact counts and ranges where the source
// shape is deterministic, exact rendered labels for hover and
// signatureHelp. See the per-test comments for the precise byte/column
// positions the assertions pin against.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "lsp/document_store.hpp"
#include "lsp_test_helpers.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>
#include <string_view>

using dss::lsp::DocumentStore;
using dss::lsp::testing::LspTestHarness;
using dss::lsp::testing::lspExit;
using dss::lsp::testing::lspInitialize;
using dss::lsp::testing::lspShutdown;
using json = nlohmann::json;

namespace {

// A c-subset document with: a global function `add`, a `main` with a local
// `x` declared and used, and a call to `add`. Positions are chosen so the
// tests can point at exact tokens.
//
//   line 0: int add(int a, int b) { return a; }
//   line 1: int main() {
//   line 2:     int x = 0;
//   line 3:     x = add(x, x);
//   line 4: }
//
// Exact column anchors used by the strict tests below:
//   `x` decl       — line 2, col  8..9
//   `x` LHS use    — line 3, col  4..5
//   `x` arg1 use   — line 3, col 12..13
//   `x` arg2 use   — line 3, col 15..16
//   `add` decl     — line 0, col  4..7
//   `add` call use — line 3, col  8..11
constexpr std::string_view kSource =
    "int add(int a, int b) { return a; }\n"   // line 0
    "int main() {\n"                          // line 1
    "    int x = 0;\n"                         // line 2
    "    x = add(x, x);\n"                     // line 3
    "}\n";                                     // line 4

[[nodiscard]] std::string didOpen(std::string_view uri, std::string_view text,
                                  int version = 1,
                                  std::string_view languageId = "c") {
    std::string esc;
    for (char c : text) {
        if (c == '\n') esc += "\\n";
        else if (c == '"') esc += "\\\"";
        else esc += c;
    }
    std::string s =
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")";
    s += uri;
    s += R"(","languageId":")";
    s += languageId;
    s += R"(","version":)";
    s += std::to_string(version);
    s += R"(,"text":")";
    s += esc;
    s += R"("}}})";
    return s;
}

// A textDocument/didChange notification with a single FULL-content edit.
[[nodiscard]] std::string didChange(std::string_view uri, int version,
                                    std::string_view text) {
    std::string esc;
    for (char c : text) {
        if (c == '\n') esc += "\\n";
        else if (c == '"') esc += "\\\"";
        else esc += c;
    }
    std::string s = R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":")";
    s += uri;
    s += R"(","version":)";
    s += std::to_string(version);
    s += R"(},"contentChanges":[{"text":")";
    s += esc;
    s += R"("}]}})";
    return s;
}

[[nodiscard]] std::string posRequest(std::string_view method, int id,
                                      std::string_view uri,
                                      int line, int character,
                                      std::string extra = "") {
    std::string s = R"({"jsonrpc":"2.0","id":)";
    s += std::to_string(id);
    s += R"(,"method":")";
    s += method;
    s += R"(","params":{"textDocument":{"uri":")";
    s += uri;
    s += R"("},"position":{"line":)";
    s += std::to_string(line);
    s += R"(,"character":)";
    s += std::to_string(character);
    s += "}";
    s += extra;  // e.g. ,"newName":"y" or ,"context":{...}
    s += "}}";
    return s;
}

// Drive: initialize, didOpen, one request (id 7), shutdown, exit. Returns
// the parsed JSON of the request's reply (server message index 2: [0]=init
// response, [1]=publishDiagnostics, [2]=request reply).
[[nodiscard]] json driveOneRequest(std::string const& request) {
    LspTestHarness h;
    h.push(lspInitialize(1));
    h.push(didOpen("file:///x.c", kSource));
    h.push(request);
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    EXPECT_EQ(h.runUntilExit(), 0);

    auto msgs = h.takeServerMessages();
    // init-response, publishDiagnostics, request-reply, shutdown-ack.
    EXPECT_GE(msgs.size(), 3u);
    return json::parse(msgs[2]);
}

} // namespace

// hover over the local `x` use on line 3 col 4 → the EXACT markdown
// label produced by the server: a fenced block containing
// "variable x: i32" (declKindLabel + name + ": " + typeString).
TEST(LspSemantic, HoverReturnsExactSymbolLabel) {
    auto reply = driveOneRequest(
        posRequest("textDocument/hover", 7, "file:///x.c", 3, 4));
    EXPECT_EQ(reply.at("id"), 7);
    ASSERT_TRUE(reply.contains("result"));
    auto const& result = reply.at("result");
    ASSERT_FALSE(result.is_null()) << "hover must resolve the `x` use";
    EXPECT_EQ(result.at("contents").at("kind"), "markdown");
    auto value = result.at("contents").at("value").get<std::string>();
    EXPECT_EQ(value, "```\nvariable x: i32\n```")
        << "exact rendered hover label (declKindLabel + name + ': ' + type)";
}

// definition from the `x` use on line 3 col 4 → the decl on line 2,
// covering exactly the identifier `x` (cols 8..9 — the `x` token in
// `    int x = 0;`).
TEST(LspSemantic, DefinitionPointsAtExactDeclIdentifier) {
    auto reply = driveOneRequest(
        posRequest("textDocument/definition", 7, "file:///x.c", 3, 4));
    ASSERT_TRUE(reply.contains("result"));
    auto const& loc = reply.at("result");
    ASSERT_FALSE(loc.is_null());
    EXPECT_EQ(loc.at("uri"), "file:///x.c");
    auto const& r = loc.at("range");
    EXPECT_EQ(r.at("start").at("line"),      2);
    EXPECT_EQ(r.at("start").at("character"), 8);
    EXPECT_EQ(r.at("end").at("line"),        2);
    EXPECT_EQ(r.at("end").at("character"),   9);
}

// references for `x` (point at the use on line 3 col 4) — with
// includeDeclaration=true → EXACTLY 4 Locations: the decl + LHS use + 2
// arg uses. Spans are exact one-char ranges over each `x` token.
TEST(LspSemantic, ReferencesListsExactDeclAndUseSpans) {
    auto reply = driveOneRequest(
        posRequest("textDocument/references", 7, "file:///x.c", 3, 4,
                   R"(,"context":{"includeDeclaration":true})"));
    ASSERT_TRUE(reply.contains("result"));
    auto const& arr = reply.at("result");
    ASSERT_TRUE(arr.is_array());
    ASSERT_EQ(arr.size(), 4u)
        << "decl + LHS x + 2 arg xs = 4 locations";
    // Collect (line, startChar, endChar) tuples and compare as a set.
    using Triple = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;
    std::set<Triple> got;
    for (auto const& loc : arr) {
        EXPECT_EQ(loc.at("uri"), "file:///x.c");
        auto const& r = loc.at("range");
        // Each `x` token is one column wide; assert end.line == start.line.
        EXPECT_EQ(r.at("start").at("line"), r.at("end").at("line"));
        got.emplace(r.at("start").at("line").get<std::uint32_t>(),
                    r.at("start").at("character").get<std::uint32_t>(),
                    r.at("end").at("character").get<std::uint32_t>());
    }
    std::set<Triple> want{
        {2,  8,  9},   // `int x = 0;` decl
        {3,  4,  5},   // `x = add(...)` LHS
        {3, 12, 13},   // first arg `x`
        {3, 15, 16},   // second arg `x`
    };
    EXPECT_EQ(got, want);
}

// rename `x` (cursor on line 3 col 4) → EXACTLY 4 edits across the doc,
// each newText == "y", with exact ranges matching the references set.
TEST(LspSemantic, RenameProducesExactEditsForEveryOccurrence) {
    auto reply = driveOneRequest(
        posRequest("textDocument/rename", 7, "file:///x.c", 3, 4,
                   R"(,"newName":"y")"));
    ASSERT_TRUE(reply.contains("result"));
    auto const& result = reply.at("result");
    ASSERT_FALSE(result.is_null());
    // Single-file CU → exactly one URI key in `changes`. Pinning this
    // catches any future regression that fans rename edits out across
    // unrelated documents (or fails to group them under the single URI).
    EXPECT_EQ(result.at("changes").size(), 1u);
    auto const& edits = result.at("changes").at("file:///x.c");
    ASSERT_TRUE(edits.is_array());
    ASSERT_EQ(edits.size(), 4u);
    using Triple = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;
    std::set<Triple> got;
    for (auto const& e : edits) {
        EXPECT_EQ(e.at("newText"), "y");
        auto const& r = e.at("range");
        EXPECT_EQ(r.at("start").at("line"), r.at("end").at("line"));
        got.emplace(r.at("start").at("line").get<std::uint32_t>(),
                    r.at("start").at("character").get<std::uint32_t>(),
                    r.at("end").at("character").get<std::uint32_t>());
    }
    std::set<Triple> want{
        {2,  8,  9},
        {3,  4,  5},
        {3, 12, 13},
        {3, 15, 16},
    };
    EXPECT_EQ(got, want);
}

// completion inside main's body (line 3 col 4) — the visible bindings
// from that scope chain are: `x` (the local), `main`, `add` (top-level
// functions). The `add` function's parameters `a` / `b` live in
// funcDefTail's OWN scope — they must NOT be visible from `main`'s body.
// Also assert the `kind` field maps correctly (3 == Function, 6 ==
// Variable per LSP §10.18 — the server's completionItemKind mapping).
TEST(LspSemantic, CompletionListsInScopeSymbolsAndHidesNonVisibleParams) {
    auto reply = driveOneRequest(
        posRequest("textDocument/completion", 7, "file:///x.c", 3, 4));
    ASSERT_TRUE(reply.contains("result"));
    auto const& items = reply.at("result");
    ASSERT_TRUE(items.is_array());
    std::set<std::string> labels;
    int xKind = -1, addKind = -1;
    for (auto const& it : items) {
        auto label = it.at("label").get<std::string>();
        labels.insert(label);
        if (label == "x")   xKind   = it.at("kind").get<int>();
        if (label == "add") addKind = it.at("kind").get<int>();
    }
    EXPECT_TRUE(labels.contains("x"))    << "the local x is in-scope here";
    EXPECT_TRUE(labels.contains("add"))  << "the global add is in-scope here";
    EXPECT_TRUE(labels.contains("main")) << "the global main is in-scope here";
    EXPECT_FALSE(labels.contains("a"))
        << "`a` is a parameter of `add` — not visible from inside `main`";
    EXPECT_FALSE(labels.contains("b"))
        << "`b` is a parameter of `add` — not visible from inside `main`";
    // LSP CompletionItemKind: 6 == Variable, 3 == Function.
    EXPECT_EQ(xKind, 6);
    EXPECT_EQ(addKind, 3);
}

// signatureHelp inside a call's arg list → the callee's signature label.
// Driven on tsql, whose `callExpr` callRule + the COALESCE builtin FnSig
// give a callable with a real signature. Asserts the EXACT label
// (deterministic from the FnSig: name + "(" + param types + ") -> "
// + result) AND the parameters array length.
TEST(LspSemantic, SignatureHelpReturnsExactCalleeSignature) {
    // tsql doc: a CREATE TABLE + a SELECT calling the COALESCE builtin.
    //   line 0: CREATE TABLE T (a INT);
    //   line 1: SELECT COALESCE(a, a) FROM T;
    // col 18 on line 1 is inside the COALESCE(...) arg list.
    constexpr std::string_view tsqlSrc =
        "CREATE TABLE T (a INT);\n"
        "SELECT COALESCE(a, a) FROM T;\n";

    LspTestHarness h;
    h.push(lspInitialize(1));
    h.push(didOpen("file:///q.sql", tsqlSrc, /*version=*/1, /*lang=*/"sql"));
    h.push(posRequest("textDocument/signatureHelp", 7, "file:///q.sql", 1, 18));
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    EXPECT_EQ(h.runUntilExit(), 0);

    auto msgs = h.takeServerMessages();
    ASSERT_GE(msgs.size(), 3u);
    auto reply = json::parse(msgs[2]);
    ASSERT_TRUE(reply.contains("result"));
    auto const& result = reply.at("result");
    ASSERT_FALSE(result.is_null()) << "signatureHelp must resolve the COALESCE call";
    auto const& sigs = result.at("signatures");
    ASSERT_TRUE(sigs.is_array());
    ASSERT_EQ(sigs.size(), 1u);
    // COALESCE is a variadic builtin with 0 declared params and result I32.
    // typeString(fnSig) for "()" param list renders as `() -> i32` after
    // the name; full label = "COALESCE() -> i32".
    EXPECT_EQ(sigs[0].at("label").get<std::string>(), "COALESCE() -> i32");
    auto const& paramsJson = sigs[0].at("parameters");
    ASSERT_TRUE(paramsJson.is_array());
    EXPECT_EQ(paramsJson.size(), 0u)
        << "COALESCE declares no fixed params (variadic-only)";
    EXPECT_EQ(result.at("activeSignature"), 0);
}

// SE7 staleness — directly drive the DocumentStore. After a model is
// stored at gen N, an update() bumps the generation; a SUBSEQUENT
// setSemanticModel against the old gen must be rejected (return false)
// and must NOT overwrite the stored model.
//
// Uses REAL SemanticModel instances (built via buildShippedUnit +
// analyze, the same path the e2e fixture uses) — no reinterpret_cast.
// The two models intentionally come from independent analyze() calls on
// the same tiny source so they're distinct objects with distinct
// addresses, letting the test pin "stored model unchanged" by pointer
// identity.
TEST(LspSemantic, StaleSetSemanticModelIsDropped) {
    using dss::sem_test::buildShippedUnit;

    auto cuOld = buildShippedUnit("c-subset", {"int x;\n"});
    auto cuNew = buildShippedUnit("c-subset", {"int x;\n"});
    auto modelOld = std::make_shared<dss::SemanticModel const>(dss::analyze(cuOld));
    auto modelNew = std::make_shared<dss::SemanticModel const>(dss::analyze(cuNew));
    ASSERT_NE(modelOld.get(), modelNew.get());

    DocumentStore store;
    store.open("file:///s.c", 1, "int x;\n", nullptr);
    ASSERT_TRUE(store.setSemanticModel("file:///s.c", 0, modelOld));
    EXPECT_EQ(store.semanticModelFor("file:///s.c").get(), modelOld.get());

    // An update bumps the generation — anything keyed on gen 0 is now stale.
    auto newGen = store.update("file:///s.c", 2, "int y;\n");
    ASSERT_TRUE(newGen.has_value());
    EXPECT_GT(*newGen, 0u);

    // A delayed worker storing its (gen 0) model must be rejected AND
    // must not overwrite the stored one.
    EXPECT_FALSE(store.setSemanticModel("file:///s.c", 0, modelNew));
    EXPECT_EQ(store.semanticModelFor("file:///s.c").get(), modelOld.get())
        << "stale setSemanticModel must NOT overwrite the stored model";
}

// SE7 diagnostic union: didOpen of a c-subset doc containing a
// const-violation triggers the analyzer + publishes the resulting
// S_ConstViolation in textDocument/publishDiagnostics with the correct
// code string. Confirms the analyzer-side semantic diagnostics survive
// the union with parse diagnostics on the wire.
TEST(LspSemantic, PublishDiagnosticsIncludesSemanticConstViolation) {
    constexpr std::string_view src =
        "int main() { const int c = 1; c = 2; }\n";
    // Column layout (0-based, line 0):
    //   "int main() { const int c = 1; c = 2; }"
    //    0123456789012345678901234567890123456789
    // The OFFENDING `c` (LHS of `c = 2;`) sits at column 30; its single-
    // char span ends at column 31. The analyzer emits S_ConstViolation
    // on the token's span, which the wire translator must preserve.
    LspTestHarness h;
    h.push(lspInitialize(1));
    h.push(didOpen("file:///cv.c", src));
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    EXPECT_EQ(h.runUntilExit(), 0);

    auto msgs = h.takeServerMessages();
    // Walk every message looking for the publishDiagnostics for cv.c.
    bool sawConst = false;
    for (auto const& raw : msgs) {
        auto m = json::parse(raw);
        if (!m.is_object() || !m.contains("method")) continue;
        if (m.at("method") != "textDocument/publishDiagnostics") continue;
        if (m.at("params").at("uri") != "file:///cv.c") continue;
        for (auto const& d : m.at("params").at("diagnostics")) {
            if (d.at("code").get<std::string>() == "S_ConstViolation") {
                sawConst = true;
                EXPECT_EQ(d.at("severity").get<int>(), 1) << "Error severity (LSP=1)";
                auto const& r = d.at("range");
                EXPECT_EQ(r.at("start").at("line").get<int>(),      0);
                EXPECT_EQ(r.at("start").at("character").get<int>(), 30);
                EXPECT_EQ(r.at("end").at("line").get<int>(),        0);
                EXPECT_EQ(r.at("end").at("character").get<int>(),   31);
            }
        }
    }
    EXPECT_TRUE(sawConst)
        << "the analyzer's S_ConstViolation must reach publishDiagnostics";
}

// SE7 diagnostic union: an undeclared identifier reaches the wire too.
// The bare `bogus_id;` exprStmt produces an S_UndeclaredIdentifier.
TEST(LspSemantic, PublishDiagnosticsIncludesSemanticUndeclared) {
    constexpr std::string_view src =
        "int main() { bogus_id; }\n";
    // Column layout (0-based, line 0):
    //   "int main() { bogus_id; }"
    //    012345678901234567890123
    // The `bogus_id` token spans columns 13..21 (8 chars). The
    // analyzer emits S_UndeclaredIdentifier on that token's span.
    LspTestHarness h;
    h.push(lspInitialize(1));
    h.push(didOpen("file:///u.c", src));
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    EXPECT_EQ(h.runUntilExit(), 0);

    auto msgs = h.takeServerMessages();
    bool sawUndecl = false;
    for (auto const& raw : msgs) {
        auto m = json::parse(raw);
        if (!m.is_object() || !m.contains("method")) continue;
        if (m.at("method") != "textDocument/publishDiagnostics") continue;
        if (m.at("params").at("uri") != "file:///u.c") continue;
        for (auto const& d : m.at("params").at("diagnostics")) {
            if (d.at("code").get<std::string>() == "S_UndeclaredIdentifier") {
                sawUndecl = true;
                auto const& r = d.at("range");
                EXPECT_EQ(r.at("start").at("line").get<int>(),      0);
                EXPECT_EQ(r.at("start").at("character").get<int>(), 13);
                EXPECT_EQ(r.at("end").at("line").get<int>(),        0);
                EXPECT_EQ(r.at("end").at("character").get<int>(),   21);
            }
        }
    }
    EXPECT_TRUE(sawUndecl);
}
