// Substrate-level tests for `relocation_table.hpp` — plan 13 §2.6
// reloc-taxonomy unifier substrate.
//
// Pins behavior the consumer-level schema-loader tests don't
// directly exercise: the `extendRow == false` skip contract, the
// loader-side dup-kind defense-in-depth, the no-extension overload,
// and the `relocation_row` concept boundary.

#include "core/substrate/diagnostic_collector.hpp"
#include "core/substrate/relocation_table.hpp"
#include "core/substrate/transparent_string_hash.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

using namespace dss;
using namespace dss::substrate;

namespace {

// Minimal RowT satisfying `relocation_row`. Carries one optional
// extension field (`note`) the lambda can populate.
struct TestRow {
    std::string    name;
    RelocationKind kind{};
    std::string    note;
};

struct LoaderState {
    std::vector<TestRow>                                 rows;
    TransparentStringMap<std::uint16_t>                  nameIdx;
    std::unordered_map<RelocationKind, std::uint16_t>    kindIdx;
    DiagnosticCollector                                  coll;
};

[[nodiscard]] std::size_t countCode(DiagnosticCollector const& c,
                                     DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : c.view()) {
        if (d.code == code) ++n;
    }
    return n;
}

} // namespace

// ── concept gate ─────────────────────────────────────────────────

static_assert(relocation_row<TestRow>,
              "TestRow{name:string, kind:RelocationKind} must satisfy "
              "the substrate's relocation_row concept");

// ── happy path ───────────────────────────────────────────────────

TEST(RelocationTableSubstrate, NoRelocationsArrayIsLegal) {
    LoaderState st;
    auto doc = nlohmann::json::parse(R"({})");
    loadRelocationsTable<TestRow>(doc, st.rows, st.nameIdx, st.kindIdx, st.coll);
    EXPECT_TRUE(st.rows.empty());
    EXPECT_FALSE(st.coll.hasErrors());
}

TEST(RelocationTableSubstrate, NoExtensionOverloadPopulatesIndices) {
    LoaderState st;
    auto doc = nlohmann::json::parse(R"({
      "relocations": [
        {"name":"R_TEST_A","kind":1},
        {"name":"R_TEST_B","kind":2}
      ]
    })");
    loadRelocationsTable<TestRow>(doc, st.rows, st.nameIdx, st.kindIdx, st.coll);
    EXPECT_FALSE(st.coll.hasErrors());
    ASSERT_EQ(st.rows.size(), 2u);
    EXPECT_EQ(st.nameIdx.size(), 2u);
    EXPECT_EQ(st.kindIdx.size(), 2u);
    EXPECT_EQ(st.kindIdx[RelocationKind{1}], 0u);
    EXPECT_EQ(st.kindIdx[RelocationKind{2}], 1u);
}

// ── extendRow contract ───────────────────────────────────────────

TEST(RelocationTableSubstrate, ExtendRowReturningFalseSkipsRow) {
    LoaderState st;
    auto doc = nlohmann::json::parse(R"({
      "relocations": [
        {"name":"R_KEEP","kind":1},
        {"name":"R_SKIP","kind":2,"note":"bad"},
        {"name":"R_KEEP_TOO","kind":3}
      ]
    })");
    // Extension lambda skips any row whose `note` field is "bad".
    loadRelocationsTable<TestRow>(
        doc, st.rows, st.nameIdx, st.kindIdx, st.coll,
        [](nlohmann::json const& r, TestRow& info,
           DiagnosticCollector&, std::size_t) -> bool {
            if (r.contains("note") && r.at("note").get<std::string>() == "bad") {
                return false;
            }
            if (r.contains("note")) info.note = r.at("note").get<std::string>();
            return true;
        });
    ASSERT_EQ(st.rows.size(), 2u);
    EXPECT_EQ(st.rows[0].name, "R_KEEP");
    EXPECT_EQ(st.rows[1].name, "R_KEEP_TOO");
    // The skipped row neither entered the name index nor the kind
    // index — the substrate's "skip means skip everywhere" contract.
    EXPECT_EQ(st.nameIdx.find("R_SKIP"), st.nameIdx.end());
    EXPECT_EQ(st.kindIdx.find(RelocationKind{2}), st.kindIdx.end());
}

// ── loader-side duplicate-kind defense-in-depth ──────────────────

TEST(RelocationTableSubstrate, DuplicateKindFailsLoudInLoader) {
    LoaderState st;
    auto doc = nlohmann::json::parse(R"({
      "relocations": [
        {"name":"R_A","kind":7},
        {"name":"R_B","kind":7}
      ]
    })");
    loadRelocationsTable<TestRow>(doc, st.rows, st.nameIdx, st.kindIdx, st.coll);
    EXPECT_TRUE(st.coll.hasErrors());
    EXPECT_EQ(countCode(st.coll, DiagnosticCode::C_MalformedJson), 1u);
    // First row entered indices; second was rejected at loader
    // time so the dual indices STILL point at the FIRST occurrence
    // — but a downstream call would correctly find the first row.
    ASSERT_EQ(st.rows.size(), 1u);
    EXPECT_EQ(st.rows[0].name, "R_A");
}

// ── per-row malformed paths ──────────────────────────────────────

TEST(RelocationTableSubstrate, NonArrayRelocationsEmitsMalformedJson) {
    LoaderState st;
    auto doc = nlohmann::json::parse(R"({"relocations":"oops"})");
    loadRelocationsTable<TestRow>(doc, st.rows, st.nameIdx, st.kindIdx, st.coll);
    EXPECT_TRUE(st.coll.hasErrors());
    EXPECT_GE(countCode(st.coll, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_TRUE(st.rows.empty());
}

TEST(RelocationTableSubstrate, ZeroKindAcceptedByLoaderCaughtByValidator) {
    // The loader range-checks kind to fit in uint32 but DOESN'T
    // reject zero (that's the validator's job — slot 0 is the
    // invalid sentinel). Pin the cross-pass split so the
    // belt-and-suspenders relationship between loader and validator
    // doesn't drift silently.
    LoaderState st;
    auto doc = nlohmann::json::parse(R"({
      "relocations":[{"name":"R_BAD","kind":0}]
    })");
    loadRelocationsTable<TestRow>(doc, st.rows, st.nameIdx, st.kindIdx, st.coll);
    EXPECT_FALSE(st.coll.hasErrors());
    ASSERT_EQ(st.rows.size(), 1u);

    std::vector<std::string> problems;
    validateRelocationsTable<TestRow>(
        std::span<TestRow const>{st.rows},
        [&](std::string path, std::string msg) {
            problems.push_back(path + ": " + msg);
        });
    EXPECT_FALSE(problems.empty())
        << "validateRelocationsTable must catch kind=0 sentinel";
}

// ── DiagnosticCollector encapsulation ────────────────────────────

TEST(DiagnosticCollector, EmitAndReleaseRoundTrip) {
    DiagnosticCollector c;
    c.emit(DiagnosticCode::C_MalformedJson, "/x", "boom");
    EXPECT_TRUE(c.hasErrors());
    EXPECT_EQ(c.size(), 1u);
    auto out = std::move(c).release();
    EXPECT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].code, DiagnosticCode::C_MalformedJson);
}

TEST(DiagnosticCollector, EmitRawForwardsExternalDiagnostic) {
    DiagnosticCollector c;
    ConfigDiagnostic d{DiagnosticCode::C_MissingField,
                       DiagnosticSeverity::Error, "/y", "missing"};
    c.emitRaw(std::move(d));
    EXPECT_TRUE(c.hasErrors());
    EXPECT_EQ(c.view()[0].code, DiagnosticCode::C_MissingField);
}

TEST(DiagnosticCollector, WarningSeverityDoesNotTripHasErrors) {
    DiagnosticCollector c;
    c.emit(DiagnosticCode::C_MalformedJson, "/z", "soft", DiagnosticSeverity::Warning);
    EXPECT_FALSE(c.hasErrors());
    EXPECT_FALSE(c.empty());
}
