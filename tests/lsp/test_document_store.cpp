// DocumentStore: open/update/close lifecycle, generation bumping,
// stale-parse suppression (worker writes diagnostics tagged with
// the generation it parsed; if the doc was updated meanwhile the
// write is silently dropped).

#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_span.hpp"
#include "lsp/document_store.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using dss::ByteOffset;
using dss::DiagnosticCode;
using dss::DiagnosticSeverity;
using dss::ParseDiagnostic;
using dss::SourceSpan;
using dss::lsp::DocumentStore;

namespace {

[[nodiscard]] ParseDiagnostic makeDiag(std::string actual) {
    ParseDiagnostic d;
    d.code     = DiagnosticCode::P_UnexpectedToken;
    d.severity = DiagnosticSeverity::Error;
    d.span     = SourceSpan::of(ByteOffset{0}, ByteOffset{1});
    d.actual   = std::move(actual);
    return d;
}

} // namespace

TEST(DocumentStore, OpenSnapshotRoundTrip) {
    DocumentStore s;
    s.open("file:///a.toy", 1, "x", nullptr);
    auto snap = s.snapshot("file:///a.toy");
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->uri, "file:///a.toy");
    EXPECT_EQ(snap->clientVersion, 1);
    EXPECT_EQ(snap->parseGeneration, 0u);
    EXPECT_EQ(snap->text, "x");
    EXPECT_EQ(snap->schema, nullptr);
}

// ★★ EXERCISE THE FAILURE ARM RATHER THAN READ IT
// (D-LSP-ASSEMBLY-DIALECT-UNSERVABLE). A document with no schema and no reason
// publishes an EMPTY diagnostics array, which an editor renders exactly like a
// clean file — so the two states a user most needs to tell apart become
// identical on the wire. `open` therefore refuses to store that pair: it
// substitutes a reason that names the omission as a server defect. The
// production caller always supplies one, so this arm is only ever reached by a
// call site that forgot — which is precisely who it is for.
TEST(DocumentStore, SchemalessOpenWithoutAReasonSubstitutesALoudOne) {
    DocumentStore s;
    s.open("file:///mystery", 1, "x", nullptr);
    auto snap = s.snapshot("file:///mystery");
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->schema, nullptr);
    EXPECT_FALSE(snap->schemaError.empty())
        << "a null schema with no reason is the silent state; the store must "
           "not be able to hold it";
    EXPECT_NE(snap->schemaError.find("did not record why"), std::string::npos)
        << "the substitute must say the SERVER failed to explain itself, not "
           "invent a property of the file; got: " << snap->schemaError;
}

// The caller's own reason is kept verbatim — the substitution above is a
// backstop, never an overwrite.
TEST(DocumentStore, SchemalessOpenKeepsTheCallersReason) {
    DocumentStore s;
    s.open("file:///a.s", 1, "nop", nullptr, "two dialects claim .s");
    auto snap = s.snapshot("file:///a.s");
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->schemaError, "two dialects claim .s");
}

TEST(DocumentStore, SnapshotMissingUriReturnsNullopt) {
    DocumentStore s;
    EXPECT_FALSE(s.snapshot("file:///nope").has_value());
}

TEST(DocumentStore, UpdateBumpsGenerationAndReturnsIt) {
    DocumentStore s;
    s.open("u", 1, "a", nullptr);
    auto g1 = s.update("u", 2, "ab");
    auto g2 = s.update("u", 3, "abc");
    ASSERT_TRUE(g1.has_value());
    ASSERT_TRUE(g2.has_value());
    EXPECT_EQ(*g1, 1u);
    EXPECT_EQ(*g2, 2u);
    auto snap = s.snapshot("u");
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->parseGeneration, 2u);
    EXPECT_EQ(snap->clientVersion, 3);
    EXPECT_EQ(snap->text, "abc");
}

TEST(DocumentStore, UpdateOnUnknownUriReturnsNullopt) {
    DocumentStore s;
    EXPECT_FALSE(s.update("ghost", 1, "x").has_value());
}

TEST(DocumentStore, SetDiagnosticsAppliesWhenGenerationMatches) {
    DocumentStore s;
    s.open("u", 1, "x", nullptr);
    std::vector<ParseDiagnostic> diags;
    diags.push_back(makeDiag("foo"));
    EXPECT_TRUE(s.setDiagnostics("u", 0u, std::move(diags)));
    auto got = s.diagnosticsFor("u");
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].actual, "foo");
}

TEST(DocumentStore, SetDiagnosticsDroppedWhenStale) {
    DocumentStore s;
    s.open("u", 1, "x", nullptr);
    (void)s.update("u", 2, "xy"); // bumps gen to 1
    std::vector<ParseDiagnostic> staleDiags;
    staleDiags.push_back(makeDiag("STALE"));
    // Worker started at gen 0 — must be dropped.
    EXPECT_FALSE(s.setDiagnostics("u", 0u, std::move(staleDiags)));
    EXPECT_TRUE(s.diagnosticsFor("u").empty());
}

TEST(DocumentStore, CloseRemovesDocument) {
    DocumentStore s;
    s.open("u", 1, "x", nullptr);
    s.close("u");
    EXPECT_FALSE(s.snapshot("u").has_value());
    EXPECT_TRUE(s.diagnosticsFor("u").empty());
}

TEST(DocumentStore, ConcurrentUpdatesAndStaleWritebackPreserveLatest) {
    // Worker thread storms `setDiagnostics(staleGen, …)` while the
    // main thread storms `update(...)`. The mutex + generation
    // token must guarantee: (a) the stale writeback is dropped or
    // applied only against its matching generation, and (b) the
    // latest update's text + version wins.
    DocumentStore s;
    s.open("u", 0, "v0", nullptr);

    constexpr int kUpdates = 200;
    std::atomic<bool> stop{false};
    std::thread worker([&] {
        while (!stop.load(std::memory_order_acquire)) {
            std::vector<ParseDiagnostic> diags;
            diags.push_back(makeDiag("worker"));
            // Always target gen 0 — every update bumps past it, so
            // these writes should be dropped after the first update.
            (void)s.setDiagnostics("u", 0u, std::move(diags));
        }
    });

    for (int i = 1; i <= kUpdates; ++i) {
        auto gen = s.update("u", i, "v" + std::to_string(i));
        ASSERT_TRUE(gen.has_value());
    }
    stop.store(true, std::memory_order_release);
    worker.join();

    auto snap = s.snapshot("u");
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->clientVersion, kUpdates);
    EXPECT_EQ(snap->text, "v" + std::to_string(kUpdates));
    EXPECT_EQ(snap->parseGeneration, static_cast<std::uint32_t>(kUpdates));
}

TEST(DocumentStore, ReopenResetsState) {
    DocumentStore s;
    s.open("u", 1, "old", nullptr);
    (void)s.update("u", 2, "older"); // gen 1
    std::vector<ParseDiagnostic> oldDiags;
    oldDiags.push_back(makeDiag("OLD"));
    (void)s.setDiagnostics("u", 1u, std::move(oldDiags));

    s.open("u", 10, "new", nullptr);
    auto snap = s.snapshot("u");
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->parseGeneration, 0u);
    EXPECT_EQ(snap->clientVersion, 10);
    EXPECT_EQ(snap->text, "new");
    EXPECT_TRUE(s.diagnosticsFor("u").empty());
}
