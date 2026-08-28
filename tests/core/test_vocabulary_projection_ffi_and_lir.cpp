// ── D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET ──────────
//    The two sites of the class that live OUTSIDE a config loader: the
//    shipped-lib descriptor reader, and the LIR register-class vocabulary.
// ─────────────────────────────────────────────────────────────────────────
//
// THE CLASS. A closed vocabulary is decided by a table (or an if-chain), and
// the message beside the check states the accepted set as a STRING LITERAL.
// Two owners of one fact; the rows keep working while the sentence becomes a
// lie, and the sentence is the half a human reads. A test asserting the
// sentence CONTAINS a name stays green the whole way down.
//
// ★★★ BOTH SITES HERE HAD ALREADY DRIFTED OR WERE ONE RENAME FROM IT, and the
// two are different species — which is why they share a file:
//
//   (1) `src/ffi/shipped_lib_descriptor.cpp` — RETYPED AND NARROWER.
//       ✔MEASURED 2026-08-20: the reader accepts an object-format spelling via
//       `objectFormatKindFromName` + `isSelectableObjectFormatKind`, i.e. FIVE
//       spellings (`kSelectableObjectFormatKindNames`). FOUR refusals in that
//       file advertised THREE — `"pe"/"elf"/"macho"` — so a descriptor author
//       writing `wasm` was told by name it was not allowed while the reader
//       took it.
//
//   (2) `src/lir/lir_reg.hpp` — A SECOND OWNER WITH A PIN THAT COULD NOT SEE
//       IT. `lirRegClassName`/`lirRegClassFromName` were a hand-written switch
//       and a hand-written if-chain over exactly the five spellings
//       `kTargetRegClassTable` owns. The `static_assert`s in that header have
//       pinned the two enums since ML5 and they pin VALUES ONLY: rename `"vr"`
//       in the target table and every one of them still passes, while
//       `.target.json` starts declaring a class the `.dsslir` text parser
//       refuses and the `.dsslir` writer emits a name the target loader
//       refuses. Two file formats disagreeing in silence, suite green.
//
// ⚠ MUST run through ctest, never a bare `.exe`: the shipped-config resolver
// walks the cwd unless `DSS_CONFIG_ROOT` is set, and only `dss_add_test` sets
// it.

#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/enum_name_table.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "lir/lir_reg.hpp"

#include "scratch_dir.hpp"
#include "vocabulary_message_probe.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

using ::dss::test_support::quotedTokens;

// ★ `quotedTokens` used to be a file-local FOURTH copy here — byte-identical to
// the one three sibling files had already merged into
// `vocabulary_projection_probe.hpp`, and therefore invisible to the mutant that
// closed D-TEST-VOCABULARY-PROJECTION-PROBE-HELPERS-ARE-COPIED-PER-FILE: that
// mutant reddened 3 of 4 and this file stayed GREEN over a helper that no longer
// worked. It has ONE owner now, so a change to what counts as a QUOTED TOKEN
// reaches every pin that reads a refusal back — see
// D-TEST-VOCABULARY-PROBE-HELPER-FOURTH-COPY-OUTSIDE-THE-EXTRACTED-HEADER.

// A spelling no vocabulary in this tree claims.
constexpr char const* kBadSpelling = "zzNotAnyVocabularySpelling";

[[nodiscard]] std::string renderDiagnostics(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) {
        out += "\n  ";
        out += d.actual;
    }
    return out.empty() ? std::string{"\n  <no diagnostics>"} : out;
}

// Read one descriptor written to a private scratch directory, and return the
// diagnostic whose text carries `needle`.
//
// ⚠ THE NEEDLE IS THE ASSERTION. A malformed descriptor raises
// `F_ShippedLibDescriptorMalformed` for a missing file, a wrong-shaped field
// and an unknown enum key alike, so `errorCount() > 0` — and even a code match
// — is satisfied by rejections that never look at the vocabulary.
struct DescriptorRejection {
    bool        read      = false;   // did the read fail as intended?
    std::string message;             // the vocabulary refusal, if found
    std::string all;                 // every diagnostic, for the << payload
    // The descriptor's own path. Every message in this reader opens
    // `shipped-lib descriptor '<path>': ...`, so the path is a quoted token
    // that is NOT vocabulary. It is carried EXACTLY rather than filtered by a
    // "looks like a path" heuristic: a heuristic that swallowed a real
    // spelling would make the honesty half silently weaker, in the one
    // direction it exists to check.
    std::string path;         // generic (forward-slash) spelling
    std::string nativePath;   // host-native spelling
};

[[nodiscard]] DescriptorRejection
rejectDescriptor(std::string_view group, std::string_view text,
                 std::string_view needle) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          std::string{group}};
    auto const path = scratch.path() / "desc.json";
    {
        std::ofstream out{path, std::ios::binary};
        out << text;
    }
    // Writing is only half the premise — prove the bytes are there before
    // reading anything into a "read failed" result.
    DescriptorRejection out;
    {
        std::ifstream in{path, std::ios::binary};
        std::string   back{std::istreambuf_iterator<char>{in},
                           std::istreambuf_iterator<char>{}};
        if (back != text) {
            out.all = "FIXTURE FAULT: the descriptor on disk does not match "
                      "the bytes this test wrote";
            return out;
        }
    }
    TypeInterner       interner{CompilationUnitId{1}};
    TypeRegistry       registry;
    DiagnosticReporter rep;
    auto const d = ffi::readShippedLibDescriptor(path, interner, registry, rep,
                                                 DataModel::Lp64);
    out.read = !d.has_value();
    out.all  = renderDiagnostics(rep);
    // ⚠ `generic_string()`, not `string()`. On Windows the two differ by the
    // separator and the reader renders the path with forward slashes, so a
    // native-separator comparison never matches and the honesty check reports
    // the path as an advertised spelling. Both spellings are carried so this
    // holds on either host.
    out.path       = path.generic_string();
    out.nativePath = path.string();
    for (auto const& diag : rep.all()) {
        if (diag.actual.find(needle) == std::string::npos) continue;
        out.message = diag.actual;
        break;
    }
    return out;
}

void expectNamesExactly(DescriptorRejection const&        r,
                        std::span<std::string_view const> names,
                        std::span<char const* const>      ignore,
                        std::string_view                  label) {
    ASSERT_TRUE(r.read) << label
                        << ": the descriptor must be REFUSED — a typo that "
                           "reads clean binds the wrong library or the wrong "
                           "signature with no diagnostic."
                        << r.all;
    ASSERT_FALSE(r.message.empty())
        << label
        << ": the vocabulary refusal never ran (the read failed for some other "
           "reason, which proves nothing about the set the message names)."
        << r.all;
    // (B) COMPLETENESS — every spelling the check takes is advertised.
    auto const quoted = quotedTokens(r.message);
    for (auto const n : names) {
        EXPECT_NE(std::find(quoted.begin(), quoted.end(), std::string{n}),
                  quoted.end())
            << label << ": the refusal does not name '" << n
            << "', a spelling the reader ACCEPTS. A message narrower than its "
               "check tells a descriptor author, by name, that a spelling the "
               "reader takes is not allowed — measured live on FOUR refusals "
               "in this file (3 of 5 object formats).\nmessage was:\n"
            << r.message;
    }
    // (C) HONESTY — it advertises nothing the check refuses.
    for (auto const& q : quoted) {
        if (std::find(names.begin(), names.end(), std::string_view{q})
            != names.end()) {
            continue;
        }
        if (q == kBadSpelling) continue;
        if (q == r.path || q == r.nativePath) continue;  // its own path
        bool ignored = false;
        for (char const* g : ignore) {
            if (q == g) { ignored = true; break; }
        }
        EXPECT_TRUE(ignored)
            << label << ": the refusal quotes '" << q
            << "', which is neither a spelling the reader accepts nor a "
               "declared non-vocabulary quote for this site.\nmessage was:\n"
            << r.message;
    }
}

constexpr auto kDataModelNames = allNames(kDataModelTable);
constexpr char const* kIgnoreDataModelKey[] = {"signatureByDataModel",
                                               "dataModel"};
constexpr char const* kIgnoreFormatKey[]    = {"availableObjectFormats",
                                               "library", "format",
                                               "sourceRealization", "when"};

}  // namespace

// ── (1) THE DESCRIPTOR READER ───────────────────────────────────────────

// The site the P23 row named. ⚠ THE ROW'S PREMISE WAS WRONG and it is recorded
// here rather than silently corrected: the row said this refusal "names no
// accepted set". It named one — `(expected one of "LP64"/"LLP64"/"ILP32")` —
// as a RETYPED literal. The defect was the retyping, not an absence.
//
// `tests/analysis/semantic/test_fc3_width_semantics.cpp` pins this message's
// PREFIX (`'signatureByDataModel' has unknown data-model key`), which the
// projection preserves — verified by that test still passing.
TEST(FfiDescriptorVocabularyProjection, SignatureByDataModelNamesEveryDataModel) {
    auto const r = rejectDescriptor(
        "vocab-sigbydm",
        R"({"header":"x.h","symbols":[
          {"name":"f","signature":"fn(i32) -> i32",
           "signatureByDataModel":{"zzNotAnyVocabularySpelling":"fn(i32) -> i32"}}]})",
        "'signatureByDataModel' has unknown data-model key");
    expectNamesExactly(r, kDataModelNames, kIgnoreDataModelKey,
                       "signatureByDataModel key");
}

// The FOUR object-format refusals, and the direction they were wrong on.
// Driven from `kSelectableObjectFormatKindNames` — the enum table minus its
// `unknown` sentinel, which is EXACTLY what the reader's
// `objectFormatKindFromName` + `isSelectableObjectFormatKind` pair accepts.
TEST(FfiDescriptorVocabularyProjection, ObjectFormatRefusalsNameEverySelectableKind) {
    {
        auto const r = rejectDescriptor(
            "vocab-availfmt",
            R"({"header":"x.h","availableObjectFormats":["zzNotAnyVocabularySpelling"],
                "symbols":[]})",
            "'availableObjectFormats' has unknown object-format");
        expectNamesExactly(r, kSelectableObjectFormatKindNames,
                           kIgnoreFormatKey, "availableObjectFormats entry");
    }
    {
        auto const r = rejectDescriptor(
            "vocab-libmap",
            R"({"header":"x.h",
                "library":{"zzNotAnyVocabularySpelling":"libc.so.6"},
                "symbols":[]})",
            "has unknown object-format key");
        expectNamesExactly(r, kSelectableObjectFormatKindNames,
                           kIgnoreFormatKey, "library map key");
    }
}

// ★ AND THE SENTINEL STILL EARNS ITS OWN REFUSAL. `objectFormatKindFromName`
// RESOLVES `"unknown"` — the one spelling that passes the lookup and then
// matches no real format — so the reader rejects it separately. This pin exists
// because the projection above must NOT start advertising it: a message that
// listed `unknown` would send an author to write the sentinel.
TEST(FfiDescriptorVocabularyProjection, TheSentinelIsNeitherAdvertisedNorAccepted) {
    EXPECT_EQ(std::find(kSelectableObjectFormatKindNames.begin(),
                        kSelectableObjectFormatKindNames.end(),
                        kObjectFormatKindSentinelName),
              kSelectableObjectFormatKindNames.end())
        << "kSelectableObjectFormatKindNames must not carry the reserved "
           "sentinel spelling '" << kObjectFormatKindSentinelName
        << "' — every object-format refusal in the descriptor reader renders "
           "from it, and a message that listed the sentinel would send an "
           "author to write the one spelling that passes the name lookup and "
           "then matches nothing";
    auto const r = rejectDescriptor(
        "vocab-sentinel",
        R"({"header":"x.h","availableObjectFormats":["unknown"],"symbols":[]})",
        "sentinel");
    EXPECT_TRUE(r.read)
        << "the reserved `unknown` sentinel must be REFUSED in "
           "availableObjectFormats — it spells correctly, survives the name "
           "lookup, and then narrows availability to a format no image can "
           "have."
        << r.all;
}

// ── (2) THE LIR REGISTER-CLASS VOCABULARY ───────────────────────────────
//
// The header's own `static_assert`s already make this property a BUILD
// failure — including the totality walk over `kTargetRegClassTable.rows` added
// with this change. This test is the runtime witness of the same claim, and it
// exists for one reason a `static_assert` cannot serve: a compile-time pin
// reports as "the tree does not build", which is indistinguishable from any
// other breakage. Here the failure NAMES the class that lost its spelling.
TEST(LirRegClassVocabulary, EveryTargetRegClassRowRoundTripsThroughTheLirHelpers) {
    ASSERT_EQ(kTargetRegClassTable.rows.size(), 5u)
        << "the class set changed — this pin walks the rows, so it needs no "
           "edit, but the count is stated so a SILENT shrink is visible";
    for (auto const& row : kTargetRegClassTable.rows) {
        SCOPED_TRACE(std::string{row.second});
        auto const lir = lirRegClassFromName(row.second);
        ASSERT_TRUE(lir.has_value())
            << "the target schema accepts register class '" << row.second
            << "' and the LIR text parser does not — a `.target.json` declaring "
               "it produces a `.dsslir` the parser refuses";
        EXPECT_EQ(static_cast<int>(*lir), static_cast<int>(row.first))
            << "'" << row.second
            << "' resolves to different numeric tags on the two tiers; "
               "`LirReg::classKind` would carry a value the other side reads as "
               "a different class";
        EXPECT_EQ(lirRegClassName(*lir), row.second)
            << "'" << row.second
            << "' does not round-trip — the LIR writer would emit a spelling "
               "the target loader refuses";
    }
}

// The mirror direction: no LIR class exists that the target tier cannot name.
// `lirRegClassName`'s `-Werror=switch` backstop makes a NEW enumerator a build
// failure; this asserts the five that exist all resolve.
TEST(LirRegClassVocabulary, EveryLirRegClassHasATargetSpelling) {
    for (auto const c : {LirRegClass::None, LirRegClass::GPR, LirRegClass::FPR,
                         LirRegClass::VR, LirRegClass::Flags}) {
        auto const name = lirRegClassName(c);
        EXPECT_FALSE(name.empty())
            << "a LIR register class with no spelling cannot be written to or "
               "read back from `.dsslir` text";
        auto const back = lirRegClassFromName(name);
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(static_cast<int>(*back), static_cast<int>(c));
        EXPECT_TRUE(targetRegClassFromName(name).has_value())
            << "the LIR tier names register class '" << name
            << "' and the target schema refuses it — the spellings have ONE "
               "owner and this is what a split looks like";
    }
}
