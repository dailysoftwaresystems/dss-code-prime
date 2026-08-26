// D-LK-OBJECT-CARRIES-NO-SUMMARY-OR-MIR-SECTION — the two LINK-TIME-OPTIMIZATION
// section rows every LINK-INPUT format declares, and the properties that make
// the pair worth declaring at all.
//
// ★★★ WHY TWO SECTIONS AND NOT ONE. `.dss.summary` is a small per-module DIGEST
// and `.dss.mir` is the module's whole body. A global pass reads EVERY summary
// to decide what to import and must do that WITHOUT PAGING IN ONE BYTE OF MIR,
// then reads the MIR of only the modules it chose. That is the entire ThinLTO
// economy, and one combined section forecloses it. So the split is not a
// stylistic partition of one payload — it is the feature — and
// `TheSummaryAndTheIrAreSEPARATESECTIONS` below is the pin that says so.
//
// ★★ WHAT THIS FILE PINS, AND WHAT IT DELIBERATELY DOES NOT. It pins the
// VOCABULARY: that the rows are declared, on the right population, spelled
// per-format, and NON-ALLOC in each format's own idiom. It pins nothing about
// CONTENTS, because nothing writes them yet — the summary-index lane populates
// them. That ordering is deliberate: the reader/writer arrives against a
// declared vocabulary rather than inventing one, which is what keeps the
// section NAME out of the engine (a walker resolves `SectionKind::LtoSummary`
// through `sectionByKind`, never a string).
//
// ★ THE POPULATION IS DERIVED, NEVER LISTED. "Which formats must carry the
// pair" is answered from the documents themselves — a format that is NOT an
// image flavour and that declares a `text` row is a LINK INPUT (a relocatable
// object, or an archive of them), and a link-time pass reads exactly those.
// Naming the ten documents here instead would make a new relocatable format
// silently exempt on the day it lands, which is the failure mode every
// corpus-derived test in this directory exists to avoid. spirv/wasm fall out
// by their OWN documents (they declare no sections at all), not by name.
//
// ⚠ NON-ALLOC IS SPELLED THREE DIFFERENT WAYS AND THE TEST SAYS ALL THREE.
// ELF states it by the ABSENCE of SHF_ALLOC; PE by IMAGE_SCN_MEM_DISCARDABLE
// with no MEM_READ; Mach-O by S_ATTR_DEBUG, the same "do not load this"
// instruction its `__compact_unwind` row already carries. One fact, three
// idioms — so the assertion dispatches on the format's declared KIND, which a
// TEST may do and the ENGINE may not. The engine never asks: it resolves the
// row through `sectionByKind` and writes what the document declared.

#include "core/types/section_kind.hpp"
#include "link/object_format_schema.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

using dss::ObjectFormatKind;
using dss::ObjectFormatSchema;
using dss::ObjectFormatSectionInfo;
using dss::SectionKind;

namespace {

// ── The shipped format NAMES, enumerated from disk ──────────────────────────
//
// From the directory rather than from a list, for the reason the file header
// gives: a format added tomorrow is measured the day it lands.
[[nodiscard]] std::vector<std::string> shippedFormatNames() {
    std::vector<std::string> out;
    auto const root = dss::test::findConfigRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return out;
    }
    auto const     dir = *root / "object-formats";
    std::error_code ec;
    for (auto const& entry : fs::directory_iterator{dir, ec}) {
        auto const filename = entry.path().filename().string();
        constexpr std::string_view kSuffix{".format.json"};
        if (filename.size() <= kSuffix.size()) continue;
        if (filename.compare(filename.size() - kSuffix.size(), kSuffix.size(),
                             kSuffix) != 0) {
            continue;
        }
        out.push_back(filename.substr(0, filename.size() - kSuffix.size()));
    }
    std::sort(out.begin(), out.end());
    EXPECT_FALSE(out.empty())
        << "no shipped .format.json documents found under " << dir.string()
        << " — every assertion in this file would then be vacuous";
    return out;
}

struct LoadedFormat {
    std::string                        name;
    std::shared_ptr<ObjectFormatSchema const> schema;
};

[[nodiscard]] std::vector<LoadedFormat> shippedFormats() {
    std::vector<LoadedFormat> out;
    for (auto const& name : shippedFormatNames()) {
        auto r = ObjectFormatSchema::loadShipped(name);
        if (!r.has_value()) {
            ADD_FAILURE() << "shipped format '" << name
                          << "' failed to load — the LTO rows this file pins "
                             "are declared in that document, so a load failure "
                             "would silently make every case below vacuous";
            continue;
        }
        out.push_back(LoadedFormat{name, *r});
    }
    return out;
}

// ★ THE DERIVED POPULATION. Not an image, and it can hold compiled code ⇒ its
// artifacts are LINK INPUTS, and a link-time pass reads link inputs. Both
// halves are read off the document.
[[nodiscard]] bool isLinkInputFormat(ObjectFormatSchema const& f) {
    return !f.isImageFlavor()
        && f.sectionByKind(SectionKind::Text) != nullptr;
}

// Is this row declared NON-ALLOC in its own format's idiom? Returns a REASON on
// failure rather than a bare bool, so the assertion names the bit that is wrong
// instead of only the row.
[[nodiscard]] std::string nonAllocViolation(ObjectFormatKind          kind,
                                            ObjectFormatSectionInfo const& row) {
    switch (kind) {
    case ObjectFormatKind::Elf: {
        // SHF_ALLOC = 0x2. Its ABSENCE is what keeps the section out of every
        // PT_LOAD, so a summary-carrying object still links with a foreign `ld`
        // into a byte-identical program.
        constexpr std::uint64_t kShfAlloc = 0x2u;
        if ((row.flags & kShfAlloc) != 0u) {
            return "ELF row declares SHF_ALLOC (0x2) — the section would be "
                   "loaded into the image";
        }
        return {};
    }
    case ObjectFormatKind::Pe: {
        constexpr std::uint32_t kCntInitializedData = 0x00000040u;
        constexpr std::uint32_t kMemDiscardable     = 0x02000000u;
        constexpr std::uint32_t kMemRead            = 0x40000000u;
        constexpr std::uint32_t kMemWrite           = 0x80000000u;
        constexpr std::uint32_t kMemExecute         = 0x20000000u;
        if ((row.type & kMemDiscardable) == 0u) {
            return "PE row does not declare IMAGE_SCN_MEM_DISCARDABLE "
                   "(0x02000000) — the linker would map it";
        }
        if ((row.type & (kMemRead | kMemWrite | kMemExecute)) != 0u) {
            return "PE row declares a memory-access characteristic "
                   "(MEM_READ/WRITE/EXECUTE) — a discarded section must claim "
                   "none";
        }
        if ((row.type & kCntInitializedData) == 0u) {
            return "PE row does not declare IMAGE_SCN_CNT_INITIALIZED_DATA "
                   "(0x40) — the section does carry file bytes";
        }
        return {};
    }
    case ObjectFormatKind::MachO: {
        // S_ATTR_DEBUG — a LINKER INSTRUCTION ("do not load this"), the same
        // value the `__LD,__compact_unwind` row carries.
        constexpr std::uint32_t kSAttrDebug = 0x02000000u;
        if ((row.type & kSAttrDebug) == 0u) {
            return "Mach-O row does not declare S_ATTR_DEBUG (0x02000000) — "
                   "ld64 would lay the section out in the image";
        }
        if (row.segment.empty()) {
            return "Mach-O row declares no segment — a two-level format names "
                   "both halves";
        }
        return {};
    }
    default:
        return "format kind has no declared non-ALLOC idiom for an LTO row";
    }
}

}  // namespace

// ── (1) THE VOCABULARY EXISTS AND THE TWO ROLES ARE DISTINCT ────────────────
//
// The enum half, asserted here as well as in `section_kind.hpp`'s own
// `static_assert`s, because a test that only reads documents cannot tell a
// document declaring the right spelling from a table that resolves both
// spellings to one kind.
TEST(LtoSummaryIrSections, TheTwoRolesAreDistinctVocabulary) {
    EXPECT_EQ(dss::sectionKindFromName("lto-summary"), SectionKind::LtoSummary);
    EXPECT_EQ(dss::sectionKindFromName("lto-ir"), SectionKind::LtoIr);
    EXPECT_NE(SectionKind::LtoSummary, SectionKind::LtoIr);
    // Neither holds body bytes a defined symbol can name: a symbol inside one
    // must never start an atom, or the reader mints a body the image then has
    // to place.
    EXPECT_FALSE(dss::sectionKindCarriesLinkableBody(SectionKind::LtoSummary));
    EXPECT_FALSE(dss::sectionKindCarriesLinkableBody(SectionKind::LtoIr));
    EXPECT_FALSE(dss::isDataSectionKind(SectionKind::LtoSummary));
    EXPECT_FALSE(dss::isDataSectionKind(SectionKind::LtoIr));
}

// ── (2) EVERY LINK-INPUT FORMAT DECLARES BOTH ROWS ──────────────────────────
//
// ★ The RED-ON-DISABLE lever for the whole config change: delete either row
// from any one of the ten documents and this goes red naming that document.
TEST(LtoSummaryIrSections, EveryLinkInputFormatDeclaresBothRows) {
    std::size_t population = 0;
    for (auto const& f : shippedFormats()) {
        if (!isLinkInputFormat(*f.schema)) continue;
        ++population;
        auto const* summary = f.schema->sectionByKind(SectionKind::LtoSummary);
        auto const* ir      = f.schema->sectionByKind(SectionKind::LtoIr);
        EXPECT_NE(summary, nullptr)
            << f.name
            << " declares no 'lto-summary' section row — a link input whose "
               "summary cannot be found is invisible to the whole-program pass";
        EXPECT_NE(ir, nullptr)
            << f.name
            << " declares no 'lto-ir' section row — its body cannot be "
               "imported by a whole-program pass";
    }
    // The population itself is asserted: a predicate that quietly selected
    // NOTHING would leave every expectation above unexecuted and the test
    // green. Ten documents ship today (elf x2 arch x {reloc,staticlib},
    // macho x2 arch x {reloc,staticlib}, pe x {reloc,staticlib}); the bound is
    // stated as a MINIMUM so a new relocatable format is welcome without an
    // edit here, and as a non-zero one so the vacuous case is a failure.
    EXPECT_GE(population, 10u)
        << "fewer link-input formats than the shipped corpus has — the "
           "selection predicate stopped matching, so the assertions above "
           "measured almost nothing";
}

// ── (3) THE SPLIT IS REAL: TWO SECTIONS, NOT ONE NAMED TWICE ────────────────
TEST(LtoSummaryIrSections, TheSummaryAndTheIrAreSeparateSections) {
    for (auto const& f : shippedFormats()) {
        if (!isLinkInputFormat(*f.schema)) continue;
        auto const* summary = f.schema->sectionByKind(SectionKind::LtoSummary);
        auto const* ir      = f.schema->sectionByKind(SectionKind::LtoIr);
        if (summary == nullptr || ir == nullptr) continue;  // (2) reports it
        EXPECT_NE(summary->name, ir->name)
            << f.name
            << " spells the summary and the IR with ONE section name — the "
               "global pass could then not read every summary without paging "
               "in the bodies, which is the entire reason the pair is a pair";
        EXPECT_FALSE(summary->name.empty()) << f.name;
        EXPECT_FALSE(ir->name.empty()) << f.name;
        // On a two-level format the (segment, name) PAIR is the identity, so
        // the segment is asserted alongside rather than instead.
        if (f.schema->kind() == ObjectFormatKind::MachO) {
            EXPECT_EQ(summary->segment, ir->segment)
                << f.name
                << " splits the pair across two segments — they are one "
                   "producer's metadata and belong together";
        }
    }
}

// ── (4) BOTH ROWS ARE NON-ALLOC, IN EACH FORMAT'S OWN IDIOM ─────────────────
//
// ✔MEASURED alongside this pin: with the rows declared, `--compile u2.c
// --target x86_64:pe64-x86_64-windows` emits a BYTE-IDENTICAL object (md5
// 274764fa952eed3b6e5d487797996d10 before and after). Writers resolve sections
// by KIND and none asks for these, so declaring them adds vocabulary and zero
// bytes — which is precisely why the non-ALLOC declaration has to be pinned
// HERE rather than discovered later by a program that fails to load.
TEST(LtoSummaryIrSections, BothRowsAreDeclaredNonAlloc) {
    for (auto const& f : shippedFormats()) {
        if (!isLinkInputFormat(*f.schema)) continue;
        for (auto const kind : {SectionKind::LtoSummary, SectionKind::LtoIr}) {
            auto const* row = f.schema->sectionByKind(kind);
            if (row == nullptr) continue;  // (2) reports it
            EXPECT_EQ(nonAllocViolation(f.schema->kind(), *row), std::string{})
                << f.name << " row '" << row->name << "' ("
                << dss::sectionKindName(kind) << ')';
        }
    }
}

// ── (5) AN IMAGE DECLARES NEITHER, AND THAT IS A STATEMENT ──────────────────
//
// An executable / shared library is the END of the pipeline: nothing reads its
// summary, and carrying the module bodies into a shipped binary would be a
// silent size regression nobody asked for. Declaring the rows there would also
// teach the image readers a classification they must never need. The absence
// is therefore intent, and intent is worth a pin.
TEST(LtoSummaryIrSections, ImageFormatsDeclareNeitherRow) {
    std::size_t images = 0;
    for (auto const& f : shippedFormats()) {
        if (!f.schema->isImageFlavor()) continue;
        ++images;
        EXPECT_EQ(f.schema->sectionByKind(SectionKind::LtoSummary), nullptr)
            << f.name
            << " declares an 'lto-summary' row on an IMAGE format — nothing "
               "reads a shipped binary's summary";
        EXPECT_EQ(f.schema->sectionByKind(SectionKind::LtoIr), nullptr)
            << f.name
            << " declares an 'lto-ir' row on an IMAGE format — that would ship "
               "every module body inside the program";
    }
    EXPECT_GT(images, 0u)
        << "no image-flavour format was examined — the absence claim above is "
           "vacuous";
}
