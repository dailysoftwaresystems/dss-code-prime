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
#include "core/types/cfi.hpp"
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
    // Does this arch's stock object ALSO carry `__TEXT,__eh_frame`?
    // D-LK-MERGED-FOREIGN-FUNCTIONS-CARRY-NO-UNWIND-INFO-IN-THE-IMAGE.
    // ✔MEASURED 2026-08-25 on real Apple Silicon (macOS 26.5.2),
    // `/usr/bin/cc -arch <a> -c` with NO other flag -- and re-measured off
    // these very fixture bytes: x86_64 YES, arm64 NO. A PRODUCER fact, the
    // second one this struct carries, and the reason the two legs must assert
    // OPPOSITE outcomes rather than the same one.
    bool                             carriesDwarfUnwind;
    // How many `unwind` rows this arch's SHIPPED DOCUMENT declares. A DOCUMENT
    // fact rather than a producer one, and it must track `carriesDwarfUnwind`:
    // the document declares a row for each encoding the objects carry.
    std::size_t                      unwindRows;
};

[[nodiscard]] Leg arm64Leg() {
    return Leg{"arm64", "macho64-arm64-darwin",
               dss::test::appleClangMachoCompactUnwindArm64Object(), 1, false, 1};
}
[[nodiscard]] Leg x86Leg() {
    return Leg{"x86_64", "macho64-x86_64-darwin",
               dss::test::appleClangMachoCompactUnwindX86_64Object(), 0, true, 2};
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
    auto const root = dss::test::findConfigRoot();
    if (!root.has_value()) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    auto const leaf = *root / "object-formats"
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

    // ── THE UNWIND OUTCOME, AND THE TWO LEGS DISAGREE ON PURPOSE ──────
    //
    // D-LK-MERGED-FOREIGN-FUNCTIONS-CARRY-NO-UNWIND-INFO-IN-THE-IMAGE.
    // ✔MEASURED: the x86_64 object carries `__TEXT,__eh_frame` BESIDE its
    // compact section and the arm64 object does not, so on x86_64 every
    // function arrives DESCRIBED and there is nothing to warn about, while on
    // arm64 the loss is real and must still be loud.
    std::size_t warned = 0;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_UnwindRuleUnrepresentable
            && d.severity == DiagnosticSeverity::Warning) {
            ++warned;
        }
    }
    std::size_t described = 0;
    for (auto const& f : mod->functions) {
        if (f.cfi.has_value()) ++described;
    }

    if (leg.carriesDwarfUnwind) {
        EXPECT_EQ(described, mod->functions.size())
            << "every function this object contributes is described by an FDE "
               "in its `__TEXT,__eh_frame`, so every one must arrive carrying "
               "call-frame information"
            << rendered(rep);
        // ★★ AND THEN THE WARNING MUST BE GONE. A warning saying these
        //    functions reach the image undescribed would be FALSE -- and a
        //    false alarm on the common case is how a reader learns to ignore
        //    the one that is real. ⓘ This arm caught an ordering defect in the
        //    reader itself: `__LD,__compact_unwind` sits at a LOWER address
        //    than `__TEXT,__eh_frame`, so a single wire-order loop warned
        //    before the carry had happened.
        EXPECT_EQ(warned, 0u)
            << "nothing is lost on this leg, so nothing may be reported lost"
            << rendered(rep);
        EXPECT_EQ(rep.all().size(), 0u)
            << "a stock object whose unwind information is fully carried must "
               "read in silence"
            << rendered(rep);
    } else {
        EXPECT_EQ(described, 0u)
            << "compact unwind has no PC dimension and is deliberately NOT "
               "converted, so no function may arrive carrying call-frame "
               "information invented from it"
            << rendered(rep);
        // The loss is LOUD. A dropped unwind table produces a binary that
        // links, runs, and cannot be unwound -- invisible until a core dump.
        EXPECT_EQ(warned, 1u)
            << "exactly one warning per unwind section -- silence is the defect "
               "this row exists to prevent, and one per RECORD would bury it"
            << rendered(rep);
        EXPECT_TRUE(anyDiagnosticContains(rep, "__LD,__compact_unwind"));
        EXPECT_TRUE(anyDiagnosticContains(rep, "3 function unwind record(s)"))
            << "the count comes from the document's own `entrySize` (0x60 / 32), so "
               "a message without it means the schema field went unread";
        EXPECT_TRUE(anyDiagnosticContains(rep, "3 function(s) this object"))
            << "the warning counts FUNCTIONS THAT REACH THE IMAGE UNDESCRIBED, "
               "not records -- the record count alone was a false alarm on the "
               "leg whose sibling section carries";
        EXPECT_TRUE(anyDiagnosticContains(
            rep, "D-LK-MERGED-FOREIGN-FUNCTIONS-CARRY-NO-UNWIND-INFO-IN-THE-IMAGE"))
            << "the warning must name the row that owns the remaining gap, or the "
               "next reader has nowhere to go";
    }
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
    // ⚠ EVERY `unwind` ROW, NOT THE FIRST ONE. Since
    // D-LK-MERGED-FOREIGN-FUNCTIONS-CARRY-NO-UNWIND-INFO-IN-THE-IMAGE the
    // x86_64 document declares TWO (compact + dwarf-cfi), and a `break` after
    // the first would leave the mutant still holding the vocabulary it claims
    // to have removed -- a mutant that does not mutate, reading green.
    nlohmann::json kept = nlohmann::json::array();
    for (auto const& row : secs) {
        if (row.at("kind").get<std::string>() != "unwind") kept.push_back(row);
    }
    std::size_t const removed = before - kept.size();
    secs = kept;
    ASSERT_EQ(removed, leg.unwindRows)
        << leg.formatName << " must declare exactly " << leg.unwindRows
        << " `unwind` section row(s) -- if this fires, the document moved and "
           "this mutant no longer removes what it claims to";

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

// ═══════════════════════════════════════════════════════════════════════
// D-LK-MERGED-FOREIGN-FUNCTIONS-CARRY-NO-UNWIND-INFO-IN-THE-IMAGE — the
// MACH-O half: `__TEXT,__eh_frame` is CARRIED into `CfiFunction`.
// ═══════════════════════════════════════════════════════════════════════
//
// ✔MEASURED 2026-08-25 on real Apple Silicon (macOS 26.5.2), `/usr/bin/cc
// -arch <a> -c foreign.c` with NO other flag: an x86_64 object carries BOTH
// `__LD,__compact_unwind` AND `__TEXT,__eh_frame`; its arm64 sibling from the
// SAME compiler and SAME source carries compact only. That is the whole reason
// the section-row identity had to become the (kind, encoding) PAIR, and it is
// why the two legs below assert OPPOSITE outcomes.

// ── 7. THE CARRY IS REAL, AND ITS CONTENT IS PINNED ───────────────────
//
// ★ "every function has a `cfi`" would pass on a decoder that attached an
//   EMPTY description to each. So this asserts what clang actually SAID:
//   the x86_64 CIE's entry state (CFA = rsp+8, return address at CFA-8 -- the
//   psABI's at-entry frame) and the three-op `push rbp; mov rsp,rbp` prologue
//   every one of these functions has. ✔MEASURED by hand-decoding the CIE and
//   FDEs of a fresh `cc -arch x86_64 -c` object: CIE `0c 07 08` = def_cfa r7
//   +8, `90 01` = offset r16 at CFA-8; each FDE `41 0e 10 86 02 43 0d 06` =
//   advance 1 / def_cfa_offset 16 / offset r6 at CFA-16 / advance 3 /
//   def_cfa_register r6.
TEST(MachoCompactUnwind, X86_64StockObjectCarriesItsDwarfUnwindIntoCfi) {
    Leg const leg = x86Leg();
    ASSERT_TRUE(leg.carriesDwarfUnwind);
    auto loaded = loadShipped(leg.targetName, leg.formatName);
    ASSERT_TRUE(loaded.target && loaded.format);

    DiagnosticReporter rep;
    auto mod = macho::readRelocatableObject(leg.object, *loaded.target,
                                            *loaded.format, rep);
    ASSERT_TRUE(mod.has_value()) << rendered(rep);
    ASSERT_EQ(mod->functions.size(), 3u);

    for (std::size_t i = 0; i < mod->functions.size(); ++i) {
        auto const& fn = mod->functions[i];
        SCOPED_TRACE("function #" + std::to_string(i));
        ASSERT_TRUE(fn.cfi.has_value())
            << "an FDE describes this function in the object; it must arrive "
               "carrying that description"
            << rendered(rep);
        auto const& cfi = *fn.cfi;

        // The ENTRY state, from the CIE. A decoder that lost it would still
        // produce a plausible op stream, and every unwind would start from the
        // wrong CFA.
        EXPECT_EQ(cfi.initial.cfaOffset, 8)
            << "x86_64 at function entry: CFA = rsp + 8 (the pushed return "
               "address)";
        ASSERT_TRUE(cfi.initial.returnAddressAtCfaOffset.has_value())
            << "the CIE states where the return address is; losing it is the "
               "one thing that stops a walk dead";
        EXPECT_EQ(*cfi.initial.returnAddressAtCfaOffset, -8);

        // The PROLOGUE, from the FDE. Three ops, in DWARF order.
        ASSERT_EQ(cfi.ops.size(), 3u)
            << "clang's `push rbp; mov rsp,rbp` prologue is exactly three rule "
               "changes; a different count means the op stream was not decoded "
               "but invented";
        EXPECT_EQ(cfi.ops[0].kind, CfiOpKind::DefCfaOffset);
        EXPECT_EQ(cfi.ops[0].pcOffset, 1u) << "after `push %rbp`";
        EXPECT_EQ(cfi.ops[0].offset, 16);
        EXPECT_EQ(cfi.ops[1].kind, CfiOpKind::RegAtCfaOffset);
        EXPECT_EQ(cfi.ops[1].offset, -16) << "the saved rbp is at CFA-16";
        EXPECT_EQ(cfi.ops[2].kind, CfiOpKind::DefCfaRegister);
        EXPECT_EQ(cfi.ops[2].pcOffset, 4u) << "after `mov %rsp,%rbp`";

        // ★ AND THE DECODED DESCRIPTION MUST BE ONE THE REST OF THE PIPELINE
        //   ACCEPTS. `validateCfiFunction` is the shared invariant every
        //   format writer's encoder spends; a decode that satisfies this test's
        //   field-by-field reading and not that predicate would be caught
        //   later, in a writer, with the object's identity already gone.
        EXPECT_EQ(validateCfiFunction(cfi), std::string{})
            << "the decoded description must satisfy the same invariant a "
               "DSS-produced one does";
    }
}

// ── 8. CONFIG-LEVEL RED-ON-DISABLE FOR THE CARRY ITSELF ───────────────
//
// The shipped x86_64 document MINUS exactly its `dwarf-cfi` row, in memory. The
// same bytes, the same binary, ONE row of vocabulary removed:
//   * every function arrives with NO call-frame information, and
//   * the compact section's warning -- silent in the control -- comes back.
// ★ THE OBJECT STILL READS. That is the point of removing only ONE of the two
//   rows: `__compact_unwind` is still classified, so this is not the old
//   "unclassified section" refusal wearing a new hat, it is the CARRY being
//   switched off and nothing else.
TEST(MachoCompactUnwind, X86_64DocumentMinusItsDwarfRowStopsCarrying) {
    Leg const leg = x86Leg();
    std::string const text = shippedFormatText(leg.formatName);
    ASSERT_FALSE(text.empty());
    auto loaded = loadShipped(leg.targetName, leg.formatName);
    ASSERT_TRUE(loaded.target && loaded.format);

    {   // CONTROL: with the row, these exact bytes carry, in silence.
        DiagnosticReporter rep;
        auto ok = macho::readRelocatableObject(leg.object, *loaded.target,
                                               *loaded.format, rep);
        ASSERT_TRUE(ok.has_value()) << rendered(rep);
        for (auto const& fn : ok->functions) {
            EXPECT_TRUE(fn.cfi.has_value()) << "control" << rendered(rep);
        }
        EXPECT_EQ(rep.all().size(), 0u) << "control" << rendered(rep);
    }

    nlohmann::json doc = nlohmann::json::parse(text);
    auto& secs = doc.at("sections");
    nlohmann::json kept = nlohmann::json::array();
    std::size_t removed = 0;
    for (auto const& row : secs) {
        bool const isDwarfUnwind =
            row.at("kind").get<std::string>() == "unwind"
            && row.contains("encoding")
            && row.at("encoding").get<std::string>() == "dwarf-cfi";
        if (isDwarfUnwind) { ++removed; continue; }
        kept.push_back(row);
    }
    ASSERT_EQ(removed, 1u)
        << "the shipped x86_64 relocatable document must declare exactly one "
           "`dwarf-cfi` unwind row -- if this fires the mutant removes nothing";
    secs = kept;

    auto stripped = ObjectFormatSchema::loadFromText(
        doc.dump(), std::string{leg.formatName} + "-no-dwarf-unwind-row");
    ASSERT_TRUE(stripped.has_value())
        << "one unwind row is a legal document -- that is what the arm64 "
           "sibling ships";

    DiagnosticReporter rep;
    auto mod = macho::readRelocatableObject(leg.object, *loaded.target,
                                            **stripped, rep);
    ASSERT_TRUE(mod.has_value())
        << "the compact row still classifies `__LD,__compact_unwind`, so the "
           "object must still READ -- this mutant switches off the CARRY, not "
           "the classification"
        << rendered(rep);
    for (auto const& fn : mod->functions) {
        EXPECT_FALSE(fn.cfi.has_value())
            << "with no `dwarf-cfi` row the section is not classified as "
               "unwind metadata at all, so nothing may be carried from it";
    }
    EXPECT_TRUE(anyDiagnosticContains(rep, "3 function(s) this object"))
        << "and the loss the control had nothing to report is loud again"
        << rendered(rep);
}

// ── 9. AN UNWIND ROW THAT DOES NOT SAY WHICH ENCODING IS REFUSED ──────
//
// ★ THE ONE PLACE THIS COULD HAVE BECOME A GUESS. Reading a compact body as
//   DWARF resynchronizes onto record bytes and produces a confident table of
//   noise, which is strictly WORSE than the silence this row exists to end --
//   the unwinder trusts a table that is present. So an unwind row with no
//   `encoding` is refused BY NAME, and the arm64 document (whose single row
//   makes the key optional as far as `validate()` is concerned) is the one
//   that proves the READER, not the loader, holds that line.
TEST(MachoCompactUnwind, AnUnwindRowWithNoDeclaredEncodingIsRefused) {
    Leg const leg = arm64Leg();
    std::string const text = shippedFormatText(leg.formatName);
    ASSERT_FALSE(text.empty());
    auto loaded = loadShipped(leg.targetName, leg.formatName);
    ASSERT_TRUE(loaded.target && loaded.format);

    nlohmann::json doc = nlohmann::json::parse(text);
    bool erased = false;
    for (auto& row : doc.at("sections")) {
        if (row.at("kind").get<std::string>() != "unwind") continue;
        ASSERT_EQ(row.at("encoding").get<std::string>(), "compact-unwind")
            << "if the shipped encoding moved, this mutant no longer removes "
               "what it claims to";
        row.erase("encoding");
        erased = true;
    }
    ASSERT_TRUE(erased);

    auto mutated = ObjectFormatSchema::loadFromText(
        doc.dump(), "macho64-arm64-unwind-row-without-encoding");
    ASSERT_TRUE(mutated.has_value())
        << "a SINGLE unwind row may legally omit the key -- `validate()` only "
           "requires it where a kind repeats. The refusal below is therefore "
           "the READER declining to guess, which is the claim under test";

    DiagnosticReporter rep;
    auto refused = macho::readRelocatableObject(leg.object, *loaded.target,
                                                **mutated, rep);
    EXPECT_FALSE(refused.has_value()) << rendered(rep);
    EXPECT_TRUE(anyDiagnosticContains(rep, "does not "
                                           "say which WIRE ENCODING it is in"))
        << "the refusal must name the missing key, because adding it is the "
           "whole fix"
        << rendered(rep);
}

// ── 10. THE SCHEMA RULES THAT KEEP `sectionByKind` HONEST ─────────────
//
// The kind-only lookup is what every WRITER uses and it cannot name an
// encoding, so a second row of a kind it can be asked about would make it
// answer "no such section" for a section the document plainly declares. Four
// rules hold that line, and each is exercised against the SHIPPED document
// rather than a hand-built one, so none of them can pass over a fixture the
// loader would never see.
[[nodiscard]] std::vector<std::string>
loadErrorsFor(nlohmann::json const& doc, std::string_view label) {
    auto r = ObjectFormatSchema::loadFromText(doc.dump(), std::string{label});
    std::vector<std::string> out;
    if (r.has_value()) return out;
    for (auto const& d : r.error()) out.push_back(d.message);
    return out;
}

[[nodiscard]] bool anyErrorContains(std::vector<std::string> const& errs,
                                    std::string_view needle) {
    return std::any_of(errs.begin(), errs.end(), [&](std::string const& e) {
        return e.find(needle) != std::string::npos;
    });
}

TEST(MachoSectionEncoding, TheSamePairTwiceIsRefused) {
    nlohmann::json doc = nlohmann::json::parse(
        shippedFormatText("macho64-x86_64-darwin"));
    // Duplicate the dwarf row VERBATIM: same kind, same encoding.
    nlohmann::json dup;
    for (auto const& row : doc.at("sections")) {
        if (row.at("kind").get<std::string>() == "unwind"
            && row.value("encoding", "") == "dwarf-cfi") {
            dup = row;
        }
    }
    ASSERT_FALSE(dup.is_null());
    dup["name"] = "__eh_frame_again";
    doc.at("sections").push_back(dup);

    auto const errs = loadErrorsFor(doc, "macho-dup-pair");
    ASSERT_FALSE(errs.empty()) << "two rows with the SAME (kind, encoding) are "
                                 "indistinguishable and must be refused";
    EXPECT_TRUE(anyErrorContains(errs, "duplicate section kind 'unwind' with "
                                       "encoding 'dwarf-cfi'"))
        << "the message must name BOTH halves -- 'duplicate kind' alone would "
           "send the author to remove the row that is legitimately there";
}

TEST(MachoSectionEncoding, ASecondRowOfANonDiscriminatedKindIsRefused) {
    nlohmann::json doc = nlohmann::json::parse(
        shippedFormatText("macho64-x86_64-darwin"));
    nlohmann::json extraText;
    for (auto const& row : doc.at("sections")) {
        if (row.at("kind").get<std::string>() == "text") extraText = row;
    }
    ASSERT_FALSE(extraText.is_null());
    extraText["name"]     = "__text2";
    extraText["encoding"] = "dwarf-cfi";   // pair-unique, and still illegal
    doc.at("sections").push_back(extraText);

    auto const errs = loadErrorsFor(doc, "macho-two-text-rows");
    ASSERT_FALSE(errs.empty())
        << "`sectionByKind(Text)` cannot name an encoding, so a second `text` "
           "row would make it answer nothing for a section that exists";
    EXPECT_TRUE(anyErrorContains(errs, "is declared by 2 rows"))
        << "the refusal must be the MULTIPLICITY rule and not the inert-key "
           "one -- both fire here and only one of them is the point";
}

TEST(MachoSectionEncoding, AnEncodingOnAKindThatHasNoneIsRefused) {
    nlohmann::json doc = nlohmann::json::parse(
        shippedFormatText("macho64-x86_64-darwin"));
    for (auto& row : doc.at("sections")) {
        if (row.at("kind").get<std::string>() == "text") {
            row["encoding"] = "dwarf-cfi";
        }
    }
    auto const errs = loadErrorsFor(doc, "macho-text-with-encoding");
    ASSERT_FALSE(errs.empty())
        << "inert config is rejected BY NAME here, exactly as a second-owner "
           "key is elsewhere in this schema -- a discriminator nothing "
           "dispatches on reads as meaningful and is not";
    EXPECT_TRUE(anyErrorContains(errs, "states a discriminator no reader "
                                       "dispatches on"));
}

TEST(MachoSectionEncoding, OneOfTwoRowsOfAKindMayNotStaySilent) {
    nlohmann::json doc = nlohmann::json::parse(
        shippedFormatText("macho64-x86_64-darwin"));
    for (auto& row : doc.at("sections")) {
        if (row.at("kind").get<std::string>() == "unwind"
            && row.value("encoding", "") == "compact-unwind") {
            row.erase("encoding");
        }
    }
    // The PAIR is still unique -- (unwind, unspecified) and (unwind, dwarf-cfi)
    // are different keys -- which is exactly why the pair rule alone is not
    // enough: the silent row is the one no reader can identify.
    auto const errs = loadErrorsFor(doc, "macho-unwind-one-row-silent");
    ASSERT_FALSE(errs.empty())
        << "pair-uniqueness admits this document; the rule that refuses it is "
           "the one that says every row of a REPEATED kind must speak";
    EXPECT_TRUE(anyErrorContains(errs, "so each must say which "
                                       "wire encoding it carries"));
}

// ── 11. A SPELLED `unspecified` IS NOT A SECOND WAY TO SAY NOTHING ────
//
// The sentinel is the ABSENCE of the key. Accepting the spelling would give
// one state two spellings, which is the shape this schema rejects by name
// wherever a second owner appears.
TEST(MachoSectionEncoding, TheSentinelSpellingIsNotDeclarable) {
    nlohmann::json doc = nlohmann::json::parse(
        shippedFormatText("macho64-arm64-darwin"));
    for (auto& row : doc.at("sections")) {
        if (row.at("kind").get<std::string>() == "unwind") {
            row["encoding"] = "unspecified";
        }
    }
    auto const errs = loadErrorsFor(doc, "macho-encoding-unspecified-spelled");
    ASSERT_FALSE(errs.empty());
    EXPECT_TRUE(anyErrorContains(errs, "unknown SectionEncoding name"));
    EXPECT_TRUE(anyErrorContains(errs, "omit the key"))
        << "the message must say what to do instead, because 'unspecified' is "
           "a spelling a reader of the enum would reasonably try";
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
