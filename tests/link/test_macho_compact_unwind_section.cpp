// `__LD,__compact_unwind` -- the section Apple's clang emits BY DEFAULT, and
// the reason DSS refused EVERY stock macOS relocatable object until 2026-08-24.
// Anchor: D-LK-MACHO-COMPACT-UNWIND-SECTION-REFUSED-BLOCKS-EVERY-STOCK-MACOS-ARCHIVE.
//
// WHAT WAS WRONG, precisely. `macho::readRelocatableObject` resolves each
// section's `SectionKind` from the (segment, section) pair declared by the
// format document. No document named `__LD,__compact_unwind`, so the section
// resolved to NO kind -- and clang's local label at its offset 0 (`ltmp1`) then
// looked exactly like an atom boundary under MH_SUBSECTIONS_VIA_SYMBOLS, which
// drove the slicing loop into
//     defined symbol 'ltmp1' lives in section '__LD,__compact_unwind' which
//     resolves to no known code/data section kind
// ⓘ THAT REFUSAL WAS CORRECT and is NOT what these tests remove: a section whose
// kind is unknown must never be guessed at. What was missing was VOCABULARY.
//
// WHAT THE FIX IS. A THIRD species in the taxonomy -- `SectionKind::Unwind`,
// per-function unwind METADATA: linker-consumed, describing other sections'
// code, contributing no linkable body of its own -- declared by the two Mach-O
// RELOCATABLE documents (arm64 + x86_64) and their `-staticlib` siblings. The
// reader tests the universal KIND, never the section NAME.
//
// Coverage:
//   1. The STOCK arm64 object reads clean: 3 functions, no data items, and
//      `ltmp1` publishes NO `ModuleSymbol` (a label on bytes the image does not
//      carry must not name an address in it).
//   2. The loss is stated OUT LOUD -- one Warning per unwind section, carrying
//      the record COUNT derived from the document's own `entrySize`.
//   3. The STOCK x86_64 object reads clean too. It is not a duplicate: it has
//      NO local label at all, so its `__compact_unwind` reaches the reader as a
//      reloc-bearing section with no symbol in it -- the half its arm64 sibling
//      cannot reach.
//   4. CONFIG-LEVEL RED-ON-DISABLE, both arches: the shipped document MINUS
//      exactly the `unwind` row, and the original refusal comes straight back.
//      The unmodified document is the control in the same test.
//   5. The `entrySize` guard: a document whose declared record width does not
//      divide the section body REFUSES rather than reporting a count the reader
//      and the producer do not agree on.
//   6. An UNCLASSIFIED section still fails loud -- the taxonomy widened, the
//      guard did not weaken. Same bytes, a document that declares the row under
//      a DIFFERENT segment, so the pair no longer matches.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/macho_object_reader.hpp"
#include "link/object_format_schema.hpp"

#include "apple_clang_macho_compact_unwind_arm64_object.inc"
#include "apple_clang_macho_compact_unwind_x86_64_object.inc"
#include "repo_root.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace dss;

namespace {

// One arch's whole fixture set, so every test below states its arch ONCE and
// the two arches run the SAME assertions. A per-arch copy of each test is how
// one of the two silently stops being checked.
struct Leg {
    std::string_view                 targetName;
    std::string_view                 formatName;
    std::vector<std::uint8_t>        object;
    // How many `nlist_64` entries name a section this reader will NOT carry.
    // arm64's clang emitted `ltmp1`; x86_64's emitted no local label at all.
    // Stated per leg because it is a PRODUCER fact, measured off the bytes.
    std::size_t                      unwindLabels;
};

[[nodiscard]] Leg arm64Leg() {
    return Leg{"arm64", "macho64-arm64-darwin",
               dss::test::appleClangMachoCompactUnwindArm64Object(), 1};
}
[[nodiscard]] Leg x86Leg() {
    return Leg{"x86_64", "macho64-x86_64-darwin",
               dss::test::appleClangMachoCompactUnwindX86_64Object(), 0};
}

struct Loaded {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

[[nodiscard]] Loaded loadShipped(std::string_view targetName,
                                 std::string_view formatName) {
    Loaded out;
    auto t = TargetSchema::loadShipped(targetName);
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped target " << targetName << " failed";
        return out;
    }
    out.target = std::move(t).value();
    auto f = ObjectFormatSchema::loadShipped(formatName);
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped format " << formatName << " failed";
        return out;
    }
    out.format = std::move(f).value();
    return out;
}

// The shipped document's own TEXT -- the "shipped file MINUS exactly one row"
// discipline this suite already uses for the `isCall` pin, so a mutant cannot
// drift away from the document it stands in for.
[[nodiscard]] std::string shippedFormatText(std::string_view formatName) {
    auto const root = dss::test::findRepoRoot();
    if (!root.has_value()) {
        ADD_FAILURE() << dss::test::repoRootDiagnostic();
        return {};
    }
    auto const leaf = *root / "src" / "dss-config" / "object-formats"
                    / (std::string{formatName} + ".format.json");
    std::ifstream in{leaf, std::ios::binary};
    if (!in.good()) {
        ADD_FAILURE() << "cannot open " << leaf.string();
        return {};
    }
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
}

// Every diagnostic the reporter saw, rendered. Appended to EVERY assertion
// below that depends on one.
//
// ★★ THIS IS NOT DECORATION -- IT IS WHAT MAKES A RED-ON-DISABLE EVIDENCE.
// ✔MEASURED 2026-08-24: the first mutation round disabled the symbol gate and
// this suite went red with `Value of: mod.has_value() / Actual: false` and
// nothing else. That proves A guard fired; it does not prove WHICH, and this
// file pins TWO refusals that both end in a refused read. A failure output that
// cannot name the refusal it is standing in for is exactly the "green-looking
// result" the mutation discipline exists to catch, one level up.
[[nodiscard]] std::string rendered(DiagnosticReporter const& rep) {
    std::string out = "\n  diagnostics seen ("
                    + std::to_string(rep.all().size()) + "):";
    if (rep.all().empty()) out += "\n    <none>";
    for (auto const& d : rep.all()) {
        out += "\n    [" + std::string{diagnosticCodeName(d.code)} + "] "
             + d.actual;
    }
    return out;
}

[[nodiscard]] ::testing::AssertionResult
anyDiagnosticContains(DiagnosticReporter const& rep, std::string_view needle) {
    if (std::any_of(rep.all().begin(), rep.all().end(),
                    [&](ParseDiagnostic const& d) {
                        return d.actual.find(needle) != std::string::npos;
                    })) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
        << "no diagnostic contains \"" << needle << "\"" << rendered(rep);
}

[[nodiscard]] std::size_t countFunctionsNamed(AssembledModule const& mod,
                                              std::string_view name) {
    return static_cast<std::size_t>(
        std::count_if(mod.symbols.begin(), mod.symbols.end(),
                      [&](ModuleSymbol const& s) { return s.name == name; }));
}

// ── 1 + 2. THE STOCK OBJECT READS, AND SAYS WHAT IT LEAVES BEHIND ─────
void stockObjectReadsCleanAndSaysWhatItDrops(Leg const& leg) {
    auto loaded = loadShipped(leg.targetName, leg.formatName);
    ASSERT_TRUE(loaded.target && loaded.format);

    DiagnosticReporter rep;
    auto mod = macho::readRelocatableObject(leg.object, *loaded.target,
                                            *loaded.format, rep);
    ASSERT_TRUE(mod.has_value())
        << "the ordinary output of the platform's own compiler must READ. This "
           "object was produced by `/usr/bin/clang -c` with no flags at all."
        << rendered(rep);
    EXPECT_EQ(rep.errorCount(), 0u);

    // The three functions the source declares, each a real body.
    EXPECT_EQ(mod->functions.size(), 3u);
    for (auto const* nm : {"_dss_stock_leaf", "_dss_stock_add",
                           "_dss_stock_answer"}) {
        EXPECT_EQ(countFunctionsNamed(*mod, nm), 1u)
            << "missing or duplicated: " << nm;
    }
    // The unwind section contributes NO body. If it ever did, its bytes would
    // be laid out in the image as if they were program data.
    EXPECT_TRUE(mod->dataItems.empty())
        << "a compact-unwind body must never be reconstructed as a data item";

    // ★ AND IT CONTRIBUTES NO NAME EITHER. ✔MEASURED 2026-08-24: recording
    //   clang's `ltmp1` put `T ltmp1` into the linked arm64 image AT THE
    //   ADDRESS OF DSS'S OWN ENTRY TRAMPOLINE (`nm -n` showed `ltmp1` where the
    //   baseline build shows `_sym_*`). The program still ran, so this was a
    //   green build shipping a symbol table that states something false.
    EXPECT_EQ(countFunctionsNamed(*mod, "ltmp1"), 0u)
        << "a label on bytes the image does not carry must not name an address "
           "in it";

    // The loss is LOUD. A dropped unwind table produces a binary that links,
    // runs, and cannot be unwound -- invisible until a core dump.
    std::size_t warned = 0;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_UnwindRuleUnrepresentable
            && d.severity == DiagnosticSeverity::Warning) {
            ++warned;
        }
    }
    EXPECT_EQ(warned, 1u)
        << "exactly one warning per unwind section -- silence is the defect "
           "this row exists to prevent, and one per RECORD would bury it"
        << rendered(rep);
    EXPECT_TRUE(anyDiagnosticContains(rep, "__LD,__compact_unwind"));
    EXPECT_TRUE(anyDiagnosticContains(rep, "3 function unwind record(s)"))
        << "the count comes from the document's own `entrySize` (0x60 / 32), so "
           "a message without it means the schema field went unread";
    EXPECT_TRUE(anyDiagnosticContains(
        rep, "D-LK-MERGED-FOREIGN-FUNCTIONS-CARRY-NO-UNWIND-INFO-IN-THE-IMAGE"))
        << "the warning must name the row that owns the remaining gap, or the "
           "next reader has nowhere to go";
}

TEST(MachoCompactUnwind, Arm64StockObjectReadsCleanAndSaysWhatItDrops) {
    stockObjectReadsCleanAndSaysWhatItDrops(arm64Leg());
}
TEST(MachoCompactUnwind, X86_64StockObjectReadsCleanAndSaysWhatItDrops) {
    stockObjectReadsCleanAndSaysWhatItDrops(x86Leg());
}

// ── 3. THE TWO FIXTURES ARE NOT THE SAME TEST TWICE ───────────────────
//
// Both came out of one `clang -c`, and they disagree in the way that separates
// the two halves of the fix: arm64 carries the `ltmp1` label (the SYMBOL half),
// x86_64 carries none (so its `__compact_unwind` is a reloc-bearing section
// with no symbol -- the RELOCATION half). Pinned so a future fixture refresh
// cannot quietly collapse the pair into one shape.
TEST(MachoCompactUnwind, TheTwoStockFixturesDisagreeAboutTheirLocalLabels) {
    EXPECT_EQ(arm64Leg().unwindLabels, 1u);
    EXPECT_EQ(x86Leg().unwindLabels, 0u);

    auto const arm = arm64Leg().object;
    auto const x86 = x86Leg().object;
    // The claim above is about BYTES, so read the bytes: the arm64 object's
    // string table contains the label name and the x86_64 object's does not.
    auto contains = [](std::vector<std::uint8_t> const& b, std::string_view s) {
        return std::search(b.begin(), b.end(), s.begin(), s.end()) != b.end();
    };
    EXPECT_TRUE(contains(arm, "ltmp1"));
    EXPECT_FALSE(contains(x86, "ltmp1"));
}

// ── 4. CONFIG-LEVEL RED-ON-DISABLE: THE SHIPPED DOCUMENT MINUS THE ROW ─
//
// The mutant is the shipped file with exactly the `unwind` row erased, in
// memory; the file on disk is never rewritten. The CONTROL in the same test
// proves the unmodified document reads THESE EXACT BYTES clean -- without it a
// refusal would prove nothing about the erased row.
void documentMinusItsUnwindRowRefusesAgain(Leg const& leg,
                                           std::string_view expectedFragment) {
    std::string const text = shippedFormatText(leg.formatName);
    ASSERT_FALSE(text.empty());

    auto loaded = loadShipped(leg.targetName, leg.formatName);
    ASSERT_TRUE(loaded.target && loaded.format);

    {   // CONTROL
        DiagnosticReporter rep;
        auto ok = macho::readRelocatableObject(leg.object, *loaded.target,
                                               *loaded.format, rep);
        ASSERT_TRUE(ok.has_value())
            << "control: WITH the row, these exact bytes read clean"
            << rendered(rep);
        EXPECT_EQ(rep.errorCount(), 0u);
    }

    nlohmann::json doc = nlohmann::json::parse(text);
    auto& secs = doc.at("sections");
    std::size_t const before = secs.size();
    for (auto it = secs.begin(); it != secs.end(); ++it) {
        if (it->at("kind").get<std::string>() == "unwind") { secs.erase(it); break; }
    }
    ASSERT_EQ(secs.size(), before - 1u)
        << leg.formatName << " must declare exactly one `unwind` section row -- "
           "if this fires, the row was dropped and the whole suite is vacuous";

    auto stripped = ObjectFormatSchema::loadFromText(
        doc.dump(), std::string{leg.formatName} + "-no-unwind-row");
    ASSERT_TRUE(stripped.has_value())
        << "the row is OPTIONAL at load -- its absence is a fact about the "
           "format, not a malformed document";

    DiagnosticReporter rep;
    auto refused = macho::readRelocatableObject(leg.object, *loaded.target,
                                                **stripped, rep);
    EXPECT_FALSE(refused.has_value())
        << "without the vocabulary the reader must REFUSE, not guess -- the "
           "original fail-loud behaviour is what this row keeps"
        << rendered(rep);
    EXPECT_GT(rep.errorCount(), 0u);
    EXPECT_TRUE(anyDiagnosticContains(rep, expectedFragment))
        << "the refusal must be the ORIGINAL one, naming the section it could "
           "not classify -- a different message means this pin is watching the "
           "wrong failure";
}

TEST(MachoCompactUnwind, Arm64DocumentMinusItsUnwindRowRefusesAgain) {
    // arm64's `ltmp1` reaches the SLICING loop's refusal first.
    documentMinusItsUnwindRowRefusesAgain(
        arm64Leg(),
        "lives in section '__LD,__compact_unwind' which resolves to no known "
        "code/data section kind");
}
TEST(MachoCompactUnwind, X86_64DocumentMinusItsUnwindRowRefusesAgain) {
    // x86_64 has no label to trip that arm, so it reaches the RELOCATION
    // refusal instead -- which is exactly why both fixtures are here.
    documentMinusItsUnwindRowRefusesAgain(
        x86Leg(),
        "carries 3 relocation(s) but reconstructed no atom to attach them to");
}

// ── 5. `entrySize` IS READ, AND A DISAGREEMENT IS LOUD ────────────────
//
// The record count in the warning is the DOCUMENT's arithmetic, not a literal
// typed into the reader. A width that does not divide the body means the reader
// and the producer disagree about the section, and a warning naming a wrong
// count is worse than no count.
TEST(MachoCompactUnwind, EntrySizeThatDoesNotDivideTheBodyRefuses) {
    Leg const leg = arm64Leg();
    std::string const text = shippedFormatText(leg.formatName);
    ASSERT_FALSE(text.empty());
    auto loaded = loadShipped(leg.targetName, leg.formatName);
    ASSERT_TRUE(loaded.target && loaded.format);

    nlohmann::json doc = nlohmann::json::parse(text);
    bool retyped = false;
    for (auto& row : doc.at("sections")) {
        if (row.at("kind").get<std::string>() == "unwind") {
            ASSERT_EQ(row.at("entrySize").get<std::uint64_t>(), 32u)
                << "the shipped width is MEASURED off Apple clang; if it moved, "
                   "this mutant is no longer the one it claims to be";
            row["entrySize"] = 7;   // 0x60 is not a multiple of 7
            retyped = true;
        }
    }
    ASSERT_TRUE(retyped);

    auto mutated = ObjectFormatSchema::loadFromText(doc.dump(),
                                                    "macho64-arm64-bad-entrysize");
    ASSERT_TRUE(mutated.has_value());

    DiagnosticReporter rep;
    auto refused = macho::readRelocatableObject(leg.object, *loaded.target,
                                                **mutated, rep);
    EXPECT_FALSE(refused.has_value()) << rendered(rep);
    EXPECT_TRUE(anyDiagnosticContains(rep, "is not a whole number of 7-byte "
                                           "records"))
        << "the refusal must name BOTH numbers, or it cannot be told from a "
           "corrupt object";
}

// ── 6. THE TAXONOMY WIDENED; THE GUARD DID NOT WEAKEN ─────────────────
//
// The same stock bytes, against a document that declares the row under a
// DIFFERENT SEGMENT. The (segment, section) PAIR is the Mach-O identity, so the
// pair no longer matches, the section resolves to no kind, and the original
// refusal fires. This is the pin that the fix is VOCABULARY and not a relaxed
// guard: an unknown section is still refused, by the same message, at the same
// place.
TEST(MachoCompactUnwind, ARowUnderTheWrongSegmentDoesNotClassifyTheSection) {
    Leg const leg = arm64Leg();
    std::string const text = shippedFormatText(leg.formatName);
    ASSERT_FALSE(text.empty());
    auto loaded = loadShipped(leg.targetName, leg.formatName);
    ASSERT_TRUE(loaded.target && loaded.format);

    nlohmann::json doc = nlohmann::json::parse(text);
    bool moved = false;
    for (auto& row : doc.at("sections")) {
        if (row.at("kind").get<std::string>() == "unwind") {
            ASSERT_EQ(row.at("segment").get<std::string>(), "__LD");
            row["segment"] = "__NOTLD";
            moved = true;
        }
    }
    ASSERT_TRUE(moved);

    auto mutated = ObjectFormatSchema::loadFromText(doc.dump(),
                                                    "macho64-arm64-wrong-segment");
    ASSERT_TRUE(mutated.has_value());

    DiagnosticReporter rep;
    auto refused = macho::readRelocatableObject(leg.object, *loaded.target,
                                                **mutated, rep);
    EXPECT_FALSE(refused.has_value())
        << "the SECTION NAME alone must never classify a Mach-O section -- the "
           "two `__const` rows already differ only by segment"
        << rendered(rep);
    EXPECT_TRUE(anyDiagnosticContains(
        rep, "resolves to no known code/data section kind"));
}

// ── The taxonomy's own claim, at compile time ─────────────────────────
//
// `Unwind` must never become producer-emittable by accident: `AssembledData`
// carrying it would mean some pass in this codebase spelling a format's unwind
// table by hand instead of stating `CfiFunction`.
static_assert(!isDataSectionKind(SectionKind::Unwind));
static_assert(!sectionKindCarriesLinkableBody(SectionKind::Unwind));
static_assert(sectionKindFromName("unwind") == SectionKind::Unwind);

} // namespace
