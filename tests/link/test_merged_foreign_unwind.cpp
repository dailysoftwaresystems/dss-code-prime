// D-LK-MERGED-FOREIGN-FUNCTIONS-CARRY-NO-UNWIND-INFO-IN-THE-IMAGE
//
// A function merged from a foreign object used to arrive in a DSS image with
// NO unwind description at all: a backtrace stopped at it, a profiler stack
// ended there, and an exception thrown through it terminated. The information
// WAS present in the object and was dropped -- in SILENCE on ELF.
//
// The fix is a READ-side tier, not a per-format carry: `dwarf_cfi_decode.hpp`
// inverts a foreign `.eh_frame` back into the neutral, PC-keyed `CfiFunction`
// vocabulary that every format writer in this tree already encodes its own
// unwind table from, so the merged function's table is RE-EMITTED and no
// format gains a new emit path.
//
// ★ THE SUBJECTS ARE REAL gcc BYTES, NOT A HAND-WRITTEN SHAPE. Both fixtures
//   in `gcc_lib_c164_object.inc` are `gcc -c` with DEFAULT flags -- which is
//   precisely what makes them carry `.eh_frame` -- and their aarch64 half
//   reaches a case the x86_64 half cannot (see `Aarch64CieSaysNothingAbout...`
//   below). A hand-built `.eh_frame` would agree with this decoder by
//   construction and witness nothing.
//
// ★ THE STRONGEST ARM HERE IS THE ROUND TRIP. Decoding gcc's rules and
//   re-encoding them through the SHIPPED `buildEhFrame`, then decoding THAT,
//   must reproduce the rule stream exactly. It is the property the whole fix
//   rests on -- that the carry is lossless -- and it is checkable with no
//   golden bytes to rot, because the two halves are independent code.

#include "asm/asm.hpp"
#include "core/types/cfi.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/dwarf_cfi.hpp"
#include "link/format/dwarf_cfi_decode.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/object_format_schema.hpp"

#include "gcc_lib_c164_object.inc"
#include "repo_root.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace dss;

namespace {

struct Port {
    char const*                         label;
    char const*                         targetName;
    char const*                         formatName;
    std::vector<std::uint8_t>           (*object)();
};

// ★★ EVERY PORT ALWAYS RUNS, and the shape is deliberate rather than
//    stylistic. A `for (p : ports) { ASSERT_... }` aborts the WHOLE loop at
//    the first port that fails, so a defect that is loud on x86_64 and silent
//    on aarch64 never reaches the aarch64 half -- the safe port masking the
//    dangerous one. That exact vacuity was measured in this project once
//    already (D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED, mutant M4), and it bit
//    THIS row's own author during development: the first cut of the DWARF
//    reverse map refused any duplicated DWARF number, which is green on
//    x86_64 (33 numbers, 33 rows) and RED on aarch64 (`dN` and `vN` share
//    DWARF 64..95 by correct psABI). Running the body through a void callable
//    lets an ASSERT return from the body and still run every other port.
template <typename Fn>
void forEachPort(Fn const& body) {
    Port const ports[] = {
        {"x86_64", "x86_64", "elf64-x86_64-linux", &dss::test::gccLibC164Object},
        {"aarch64", "arm64", "elf64-aarch64-linux",
         &dss::test::gccLibC164ObjectAarch64},
    };
    for (Port const& p : ports) body(p);
}

struct Loaded {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

[[nodiscard]] std::optional<Loaded> load(Port const& p) {
    auto t = TargetSchema::loadShipped(p.targetName);
    if (!t.has_value()) {
        ADD_FAILURE() << p.label << ": TargetSchema::loadShipped('"
                      << p.targetName << "') failed";
        return std::nullopt;
    }
    auto f = ObjectFormatSchema::loadShipped(p.formatName);
    if (!f.has_value()) {
        ADD_FAILURE() << p.label << ": ObjectFormatSchema::loadShipped('"
                      << p.formatName << "') failed";
        return std::nullopt;
    }
    return Loaded{std::move(t).value(), std::move(f).value()};
}

// Render every diagnostic a reporter collected. A red printing only
// "Actual: false" proves A refusal fired but not WHICH, which is the defect
// `D-LK-MACHO-COMPACT-UNWIND-SECTION-REFUSED-BLOCKS-EVERY-STOCK-MACOS-ARCHIVE`
// had to repair in its own test mid-run.
[[nodiscard]] std::string diagText(DiagnosticReporter const& r) {
    std::string out = "\n  diagnostics seen (" + std::to_string(r.all().size())
                    + "):";
    if (r.all().empty()) out += "\n    <none>";
    for (auto const& d : r.all()) {
        out += "\n    [" + std::string{diagnosticCodeName(d.code)} + "] "
             + d.actual;
    }
    return out;
}

// The shipped document's own TEXT -- the "shipped file MINUS exactly one row"
// discipline this suite already uses, so a mutant cannot drift away from the
// document it stands in for.
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

// ── FIND THE UNWIND SECTION BY ITS UNIVERSAL KIND, NEVER BY NAME ────────
// Mirrors the reader. Returns {offset, size} of the section this format
// declares as `unwind`, or nullopt.
struct SecSpan { std::size_t off; std::size_t size; };

[[nodiscard]] std::optional<SecSpan>
unwindSectionOf(std::span<std::uint8_t const> obj, ObjectFormatSchema const& fmt) {
    auto rdU16 = [&](std::size_t o) {
        return static_cast<std::uint16_t>(obj[o] | (obj[o + 1] << 8));
    };
    auto rdU32 = [&](std::size_t o) {
        std::uint32_t v = 0;
        for (unsigned i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(obj[o + i]) << (8 * i);
        return v;
    };
    auto rdU64 = [&](std::size_t o) {
        std::uint64_t v = 0;
        for (unsigned i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(obj[o + i]) << (8 * i);
        return v;
    };
    std::string unwindName;
    for (auto const& row : fmt.sections()) {
        if (row.kind == SectionKind::Unwind) unwindName = row.name;
    }
    if (unwindName.empty()) return std::nullopt;
    std::uint64_t const shoff = rdU64(0x28);
    std::uint16_t const shnum = rdU16(0x3C);
    std::uint16_t const shstr = rdU16(0x3E);
    std::uint64_t const strOff =
        rdU64(static_cast<std::size_t>(shoff) + shstr * 64u + 0x18);
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::size_t const sh = static_cast<std::size_t>(shoff) + i * 64u;
        std::uint32_t const nameIdx = rdU32(sh + 0);
        std::string name;
        for (std::size_t k = static_cast<std::size_t>(strOff) + nameIdx;
             k < obj.size() && obj[k] != 0; ++k) {
            name.push_back(static_cast<char>(obj[k]));
        }
        if (name != unwindName) continue;
        return SecSpan{static_cast<std::size_t>(rdU64(sh + 0x18)),
                       static_cast<std::size_t>(rdU64(sh + 0x20))};
    }
    return std::nullopt;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// The carry itself.
// ═══════════════════════════════════════════════════════════════════════

// Every function gcc described must arrive with a description. Before the
// fix EVERY one of these was `nullopt`, on both ports, with no diagnostic.
TEST(MergedForeignUnwind, EveryGccDescribedFunctionArrivesWithCallFrameInfo) {
    forEachPort([](Port const& p) {
        auto loaded = load(p);
        if (!loaded.has_value()) return;
        DiagnosticReporter rep;
        auto const bytes = p.object();
        auto mod = elf::readRelocatableObject(bytes, *loaded->target,
                                              *loaded->format, rep);
        ASSERT_TRUE(mod.has_value())
            << p.label << ": a real gcc object must reconstruct" << diagText(rep);
        ASSERT_FALSE(mod->functions.empty()) << p.label;
        for (std::size_t i = 0; i < mod->functions.size(); ++i) {
            auto const& fn = mod->functions[i];
            EXPECT_TRUE(fn.cfi.has_value())
                << p.label << ": function #" << i
                << " came out of a `gcc -c` object with no unwind description; "
                   "gcc emits one FDE per function with a frame and dropping it "
                   "is the whole subject of "
                   "D-LK-MERGED-FOREIGN-FUNCTIONS-CARRY-NO-UNWIND-INFO-IN-THE-IMAGE";
            if (!fn.cfi.has_value()) continue;
            // The extent must cover the whole function or an unwinder stops
            // mid-walk with no error.
            EXPECT_EQ(fn.cfi->codeLength,
                      static_cast<std::uint32_t>(fn.bytes.size()))
                << p.label << ": function #" << i;
            EXPECT_EQ(validateCfiFunction(*fn.cfi), std::string{})
                << p.label << ": function #" << i;
            EXPECT_FALSE(fn.cfi->ops.empty())
                << p.label << ": function #" << i
                << " decoded to an EMPTY rule stream, which would encode as an "
                   "FDE describing a frame that never changes";
        }
    });
}

// ★ THE ROUND TRIP. gcc's rules -> `buildEhFrame` (the SHIPPED encoder) ->
//   this decoder again. Any loss anywhere in the carry shows up here as a
//   difference, and neither half can hide it by agreeing with itself: the
//   encoder was written for a different row, by a different cycle, and has
//   its own data-alignment factor (-1) and its own advance_loc choices.
TEST(MergedForeignUnwind, GccRulesSurviveReEncodingThroughTheShippedWriter) {
    forEachPort([](Port const& p) {
        auto loaded = load(p);
        if (!loaded.has_value()) return;
        DiagnosticReporter rep;
        auto const bytes = p.object();
        auto mod = elf::readRelocatableObject(bytes, *loaded->target,
                                              *loaded->format, rep);
        ASSERT_TRUE(mod.has_value()) << p.label << diagText(rep);

        std::vector<std::optional<CfiFunction>> perFunc;
        for (auto const& fn : mod->functions) perFunc.push_back(fn.cfi);
        ASSERT_FALSE(perFunc.empty()) << p.label;

        auto const regs = link::format::dwarfRegisterMappingOf(*loaded->target);
        auto sec = link::format::buildEhFrame(perFunc, regs, 8, rep);
        ASSERT_TRUE(sec.has_value())
            << p.label << ": the shipped writer refused rules it had just been "
                          "handed back" << diagText(rep);
        ASSERT_FALSE(sec->bytes.empty()) << p.label;

        auto again = link::format::decodeEhFrame(sec->bytes, regs,
                                                 "test::roundTrip", rep);
        ASSERT_TRUE(again.has_value()) << p.label << diagText(rep);

        std::size_t described = 0;
        for (auto const& f : perFunc) if (f.has_value()) ++described;
        ASSERT_EQ(again->fdes.size(), described) << p.label;

        std::size_t k = 0;
        for (auto const& f : perFunc) {
            if (!f.has_value()) continue;
            CfiFunction const& orig = *f;
            CfiFunction const& back = again->fdes[k++].cfi;
            EXPECT_EQ(back.codeLength, orig.codeLength) << p.label;
            EXPECT_TRUE(back.initial == orig.initial)
                << p.label << ": the entry state did not survive the writer";
            ASSERT_EQ(back.ops.size(), orig.ops.size())
                << p.label << ": the rule stream changed length across the "
                              "round trip, so the carry is lossy";
            for (std::size_t oi = 0; oi < orig.ops.size(); ++oi) {
                EXPECT_EQ(back.ops[oi].pcOffset, orig.ops[oi].pcOffset)
                    << p.label << " op#" << oi;
                EXPECT_EQ(static_cast<int>(back.ops[oi].kind),
                          static_cast<int>(orig.ops[oi].kind))
                    << p.label << " op#" << oi << " ("
                    << cfiOpKindName(orig.ops[oi].kind) << ")";
                EXPECT_TRUE(back.ops[oi].reg == orig.ops[oi].reg)
                    << p.label << " op#" << oi;
                EXPECT_EQ(back.ops[oi].offset, orig.ops[oi].offset)
                    << p.label << " op#" << oi;
            }
        }
    });
}

// ★★ THE HALF THE x86_64 PORT CANNOT REACH, AND IT COST A WHOLE LEG.
//    `aarch64-linux-gnu-gcc`'s CIE says `DW_CFA_def_cfa r31 (sp) ofs 0` and
//    NOTHING about the return address, leaning on DWARF's `same_value`
//    default for the RA column; x86_64 gcc spells `DW_CFA_offset r16 at
//    cfa-8` out. Reading the silence as "no information" produced an entry
//    state that disagreed with the one DSS's own producer derives from
//    `cc.linkRegister`, and the shared-CIE check refused the whole image --
//    on the aarch64 leg only, after the x86_64 leg had linked and RUN green.
TEST(MergedForeignUnwind, ASilentCieStillStatesWhereTheReturnAddressIs) {
    forEachPort([](Port const& p) {
        auto loaded = load(p);
        if (!loaded.has_value()) return;
        DiagnosticReporter rep;
        auto const bytes = p.object();
        auto mod = elf::readRelocatableObject(bytes, *loaded->target,
                                              *loaded->format, rep);
        ASSERT_TRUE(mod.has_value()) << p.label << diagText(rep);
        CfiInitialState const* first = nullptr;
        for (auto const& fn : mod->functions) {
            if (fn.cfi.has_value()) { first = &fn.cfi->initial; break; }
        }
        ASSERT_NE(first, nullptr) << p.label;
        // EXACTLY ONE of the two forms must be stated. Both empty means the
        // decoded frame does not say where the return address went, which is
        // the shape that silently disagrees with DSS's own producer.
        EXPECT_NE(first->returnAddressAtCfaOffset.has_value(),
                  first->returnAddressRegister.has_value())
            << p.label << ": the entry state names the return address "
            << (first->returnAddressAtCfaOffset.has_value() ? "twice" : "not at all");
        // And it must be the form the target's own calling convention uses,
        // or `buildEhFrame`'s shared-CIE check refuses the image.
        auto const regs = link::format::dwarfRegisterMappingOf(*loaded->target);
        ASSERT_TRUE(regs.returnAddressColumn.has_value()) << p.label;
        bool columnIsARegister = false;
        for (auto const& r : regs.registers) {
            if (r.dwarfNumber.has_value()
                && *r.dwarfNumber == *regs.returnAddressColumn) {
                columnIsARegister = true;
            }
        }
        EXPECT_EQ(first->returnAddressRegister.has_value(), columnIsARegister)
            << p.label
            << ": a link-register ABI (the RA column IS a register) must decode "
               "to `returnAddressRegister`, and a synthetic column to a CFA "
               "offset -- getting this backwards makes the merged functions "
               "disagree with DSS's own about their entry state";
    });
}

// ★★★ THE ARM THE ROUND TRIP CANNOT REPLACE, AND THE REASON IT EXISTS.
//     A round trip compares this decoder against the shipped encoder, so a
//     SYMMETRIC error — mis-reading the CIE's data-alignment factor, say —
//     round-trips perfectly and ships a table every rule of which is scaled
//     wrong. What pins the absolute values is that the FOREIGN entry state
//     must equal the one DSS's own producer derives from the calling
//     convention: `cfaRegister` = `cc.stackPointer`, `cfaOffset` =
//     `cc.callPushBytes`, and the return address either at `-callPushBytes`
//     or in `cc.linkRegister`. That is not a coincidence to be pinned — it is
//     a REQUIREMENT, because `buildEhFrame` emits ONE shared CIE per module
//     and refuses two disagreeing entry states outright. Deriving the
//     expectation from the target document rather than typing -8 and 0 keeps
//     it true for a third target that ships tomorrow.
TEST(MergedForeignUnwind, ForeignEntryStateEqualsWhatDssOwnProducerDerives) {
    forEachPort([](Port const& p) {
        auto loaded = load(p);
        if (!loaded.has_value()) return;
        auto const ccs = loaded->target->callingConventions();
        ASSERT_FALSE(ccs.empty()) << p.label;
        auto const& cc = ccs[0];
        ASSERT_TRUE(cc.stackPointer.has_value()) << p.label;

        CfiInitialState expected;
        expected.cfaRegister = cc.stackPointer->ordinal;
        expected.cfaOffset   = static_cast<std::int64_t>(cc.callPushBytes);
        if (cc.callPushBytes > 0) {
            expected.returnAddressAtCfaOffset =
                -static_cast<std::int64_t>(cc.callPushBytes);
        } else if (cc.linkRegister.has_value()) {
            expected.returnAddressRegister = cc.linkRegister->ordinal;
        }

        DiagnosticReporter rep;
        auto const bytes = p.object();
        auto mod = elf::readRelocatableObject(bytes, *loaded->target,
                                              *loaded->format, rep);
        ASSERT_TRUE(mod.has_value()) << p.label << diagText(rep);
        std::size_t checked = 0;
        for (std::size_t i = 0; i < mod->functions.size(); ++i) {
            auto const& fn = mod->functions[i];
            if (!fn.cfi.has_value()) continue;
            ++checked;
            EXPECT_EQ(fn.cfi->initial.cfaRegister, expected.cfaRegister)
                << p.label << " fn#" << i << ": the foreign CIE's CFA base "
                   "register is not the one this target calls its stack pointer";
            EXPECT_EQ(fn.cfi->initial.cfaOffset, expected.cfaOffset)
                << p.label << " fn#" << i
                << ": the foreign CIE's entry CFA offset disagrees with "
                   "`cc.callPushBytes` (" << cc.callPushBytes << ")";
            EXPECT_EQ(fn.cfi->initial.returnAddressAtCfaOffset,
                      expected.returnAddressAtCfaOffset)
                << p.label << " fn#" << i
                << ": where the foreign object says the return address is "
                   "saved does not match what a CALL on this target pushes — a "
                   "mis-scaled decode looks exactly like this and round-trips "
                   "perfectly";
            EXPECT_EQ(fn.cfi->initial.returnAddressRegister,
                      expected.returnAddressRegister)
                << p.label << " fn#" << i;
        }
        EXPECT_GT(checked, 0u)
            << p.label << ": no function carried an entry state to check, so "
                          "this arm asserted nothing";
    });
}

// ═══════════════════════════════════════════════════════════════════════
// The refusals. Each mutates the REAL object's bytes so the construct
// under test is reached through the ordinary path, and each asserts the
// MESSAGE, not merely that something failed.
// ═══════════════════════════════════════════════════════════════════════

namespace {

// Overwrite the CIE's augmentation character (the byte after the 'z') in a
// copy of the object. gcc's `.eh_frame` always begins with a CIE whose
// augmentation is "zR", so byte (unwindOff + 9) is the 'R'.
[[nodiscard]] std::vector<std::uint8_t>
withAugmentationChar(std::span<std::uint8_t const> obj, std::size_t unwindOff,
                     char c) {
    std::vector<std::uint8_t> copy(obj.begin(), obj.end());
    copy[unwindOff + 10] = static_cast<std::uint8_t>(c);
    return copy;
}

} // namespace

// A personality routine that this project's neutral representation cannot
// carry must REFUSE, never zero the pointer: a dropped personality turns a
// caught exception into a terminate, and the image would look complete.
TEST(MergedForeignUnwind, PersonalityAugmentationIsRefusedByName) {
    forEachPort([](Port const& p) {
        auto loaded = load(p);
        if (!loaded.has_value()) return;
        auto const bytes = p.object();
        auto const sec = unwindSectionOf(bytes, *loaded->format);
        ASSERT_TRUE(sec.has_value())
            << p.label << ": format '" << p.formatName
            << "' declares no `unwind` section row, so this object's "
               "`.eh_frame` is not even classified";
        // "zR" -> "zP": the 'R' at offset 9 of the CIE body becomes 'P'.
        auto mutated = withAugmentationChar(bytes, sec->off, 'P');
        DiagnosticReporter rep;
        auto mod = elf::readRelocatableObject(mutated, *loaded->target,
                                              *loaded->format, rep);
        EXPECT_FALSE(mod.has_value())
            << p.label << ": a CIE naming a personality routine was ACCEPTED";
        EXPECT_NE(diagText(rep).find("personality"), std::string::npos)
            << p.label << ": the refusal did not name the personality routine."
            << diagText(rep);
    });
}

// An unknown augmentation character changes the layout of every record after
// it, so it is refused rather than skipped.
TEST(MergedForeignUnwind, UnknownAugmentationCharacterIsRefused) {
    forEachPort([](Port const& p) {
        auto loaded = load(p);
        if (!loaded.has_value()) return;
        auto const bytes = p.object();
        auto const sec = unwindSectionOf(bytes, *loaded->format);
        ASSERT_TRUE(sec.has_value()) << p.label;
        auto mutated = withAugmentationChar(bytes, sec->off, 'Q');
        DiagnosticReporter rep;
        auto mod = elf::readRelocatableObject(mutated, *loaded->target,
                                              *loaded->format, rep);
        EXPECT_FALSE(mod.has_value())
            << p.label << ": an unknown CIE augmentation was ACCEPTED";
        EXPECT_NE(diagText(rep).find("augmentation"), std::string::npos)
            << p.label << diagText(rep);
    });
}

// ★ THE DECODER MUST NOT BE ABLE TO BIND AN FDE TO THE WRONG FUNCTION.
//   Corrupting the FDE's `address_range` makes it describe a different
//   extent than the function symbol declares; accepting that ships a table
//   whose tail is undescribed and whose walk stops with no diagnostic.
TEST(MergedForeignUnwind, AnFdeThatDoesNotCoverItsFunctionIsRefused) {
    forEachPort([](Port const& p) {
        auto loaded = load(p);
        if (!loaded.has_value()) return;
        auto const bytes = p.object();
        auto const sec = unwindSectionOf(bytes, *loaded->format);
        ASSERT_TRUE(sec.has_value()) << p.label;
        DiagnosticReporter probe;
        auto const decoded = link::format::decodeEhFrame(
            std::span<std::uint8_t const>(bytes).subspan(sec->off, sec->size),
            link::format::dwarfRegisterMappingOf(*loaded->target),
            "test::extent", probe);
        ASSERT_TRUE(decoded.has_value()) << p.label << diagText(probe);
        ASSERT_FALSE(decoded->fdes.empty()) << p.label;
        // `address_range` sits immediately after the 4-byte
        // `initial_location` field this decoder reports the offset of.
        std::size_t const rangeAt =
            sec->off + decoded->fdes[0].initialLocationFieldOffset + 4u;
        std::vector<std::uint8_t> mutated(bytes.begin(), bytes.end());
        mutated[rangeAt] = static_cast<std::uint8_t>(mutated[rangeAt] + 1u);
        DiagnosticReporter rep;
        auto mod = elf::readRelocatableObject(mutated, *loaded->target,
                                              *loaded->format, rep);
        EXPECT_FALSE(mod.has_value())
            << p.label << ": an FDE whose extent disagrees with its function's "
                          "symbol was ACCEPTED";
        EXPECT_NE(diagText(rep).find("describes"), std::string::npos)
            << p.label << diagText(rep);
    });
}

// ★★ THE ROW'S OWN VOCABULARY CLAIM, EXERCISED: the reader finds the section
//    by its universal `kind` and never by the string `.eh_frame`. Remove the
//    row and the very same bytes carry nothing -- which is exactly the
//    pre-fix behaviour, so this arm is the in-suite twin of the end-to-end
//    config mutant.
TEST(MergedForeignUnwind, WithNoUnwindRowDeclaredTheSameBytesCarryNothing) {
    forEachPort([](Port const& p) {
        auto loaded = load(p);
        if (!loaded.has_value()) return;
        // A format whose `unwind` row is absent: the SHIPPED document minus
        // exactly that row, re-loaded through the schema's own loader so the
        // mutant cannot drift away from the file it stands in for.
        std::string const text = shippedFormatText(p.formatName);
        if (text.empty()) return;
        nlohmann::json doc = nlohmann::json::parse(text);
        auto& secs = doc.at("sections");
        for (auto it = secs.begin(); it != secs.end(); ++it) {
            if (it->value("kind", std::string{}) == "unwind") {
                secs.erase(it);
                break;
            }
        }
        auto stripped = ObjectFormatSchema::loadFromText(
            doc.dump(), std::string{p.formatName} + " (unwind row removed)");
        ASSERT_TRUE(stripped.has_value())
            << p.label << ": the document minus its unwind row must still load";
        bool hasUnwindRow = false;
        for (auto const& row : (*stripped)->sections()) {
            if (row.kind == SectionKind::Unwind) hasUnwindRow = true;
        }
        ASSERT_FALSE(hasUnwindRow) << p.label;

        DiagnosticReporter rep;
        auto const bytes = p.object();
        auto mod = elf::readRelocatableObject(bytes, *loaded->target,
                                              **stripped, rep);
        ASSERT_TRUE(mod.has_value())
            << p.label << ": an undeclared unwind section must leave the object "
                          "readable, exactly as before the row existed"
            << diagText(rep);
        for (std::size_t i = 0; i < mod->functions.size(); ++i) {
            EXPECT_FALSE(mod->functions[i].cfi.has_value())
                << p.label << ": function #" << i
                << " carried unwind information with NO `unwind` row declared, "
                   "so the reader is finding the section some other way than "
                   "through the format vocabulary";
        }
    });
}
