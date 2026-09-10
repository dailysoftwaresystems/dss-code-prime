// D-LINK-MACHO-IMAGE-OVERALIGNED-STATIC-IS-A-LOAD-TIME-COIN-FLIP
//
// The Mach-O half of the question
// [[D-LINK-ELF-IMAGE-OVERALIGNED-DATA-PLACED-AT-ALIGNED-FILE-OFFSET]] left
// open: the ELF row fixed an exec writer that rounded a FILE OFFSET where it
// should have rounded an ADDRESS, and said in its own Cross-refs that the PE
// and Mach-O writers *"build on the same `va == base + offset` identity and on
// the same `p_align`-equivalent segment promise, and neither was read for
// either half of this defect"*. This file is the reading, and it splits into
// two assertions because the two halves came out DIFFERENT here.
//
// ── HALF ONE: THE LOAD-TIME HALF, WHICH IS A REAL DEFECT AND IS WORSE ON
//    MACH-O THAN ON ELF, BECAUSE THE CONTAINER HAS NO FIELD TO FIX IT WITH ──
//
// An ELF PT_LOAD carries `p_align`, so the ELF writer could PROMISE a mapping
// granularity that covers its strongest member and the kernel would honour it.
// `segment_command_64` HAS NO SUCH FIELD. A Mach-O image is slid by dyld at a
// multiple of the target's VM page size and nothing in the image can ask for
// more — so an object requesting more than a page keeps the address the linker
// chose and LOSES its alignment at load, on roughly half of all runs, silently.
//
// ✔MEASURED ON THE REFERENCE, Apple Silicon, macOS 26.6.2, Apple clang 21.0.0,
// ld PROJECT:ld-1267, each port probed SEPARATELY on the same source, BUILD and
// RUN, 20 runs per cell because one run decides nothing about a coin flip:
//
//   arm64  (page 16384): align 16 / 4096 / 16384 -> 42, twenty times out of
//     twenty. align 32768 -> ld64 WARNS *"reducing alignment of section
//     __DATA,__data from 0x8000 to 0x4000 because it exceeds segment maximum
//     alignment"* and the program then returns 50 or 54 depending on the run —
//     NEVER 42, in twenty runs.
//   x86_64 (page 4096):  align 4096 -> 42 ten times out of ten. align 8192 ->
//     the SAME warning with the numbers 0x2000 -> 0x1000, and 50/52 by run.
//
// ★ SO THE CEILING IS THE TARGET'S PAGE SIZE, AND THE REFERENCE NAMES IT: the
// cap ld64 applies is 0x4000 on the 16 KiB port and 0x1000 on the 4 KiB one —
// it tracks the page, it is not a constant. `-Wl,-segalign,0x8000` does not
// raise it either; ld64 refuses the link outright with *"chained fixups,
// page_size not 4KB or 16KB in segment #2"*. So no reference makes an
// above-page static WORK on Mach-O, and under `DSS = (gcc ∪ clang ∪ MSVC)` —
// the union being over what WORKS, not what is ACCEPTED — a reference that
// accepts and then ships a wrong program casts no vote.
//
// ⇒ DSS REFUSES ABOVE `image.segmentPageSize`, read from the format document,
// with no format-identity branch. Before this it had NO ceiling at all on a
// STATIC (only the thread-local one), so it placed the object at a link-time
// correct address and shipped the coin flip with not even ld64's warning.
//
// ── HALF TWO: THE ADDRESS-VS-OFFSET HALF, WHICH IS **NOT** THE ELF DEFECT
//    HERE — AND SAYING SO PRECISELY IS THE DELIVERABLE ──────────────────────
//
// The mechanism IS present. `encodeExecDynamic` derives every allocated VA
// TWICE: an EARLY chain that rounds the ADDRESS (binding symbolVa before the
// relocation kernel runs) and a LATE chain that rounds the FILE OFFSET and
// derives the address from it. But the consequence is different in both
// directions, and both differences are measured:
//
//  (1) IT WAS NEVER SILENT. Four fail-loud congruence guards — `__const`,
//      `__eh_frame`, `__got`, `__DATA` — compare the two derivations and REFUSE
//      the image when they disagree. ELF had no such comparison, which is why
//      its divergence shipped as a misplaced object and this one cannot.
//      `AWalkerRefusesRatherThanShipAnImageWhoseTwoVaDerivationsDisagree` below
//      is the first pin any of those four guards has ever had.
//
//  (2) IT IS UNREACHABLE THROUGH ANY LOADABLE DOCUMENT, by arithmetic rather
//      than by luck. The delta IS `image.pageZeroSize`
//      (`textSegmentVaMatchesFileOff` requires
//      `__text.virtualAddress - pageZeroSize == textFileOff`); the kernel's mmap
//      rule `vmaddr % page == fileoff % page` applied to `__TEXT`, whose vmaddr
//      IS pageZeroSize and whose fileoff is 0, forces that delta to be a
//      multiple of the page; and the ceiling above caps every request AT the
//      page. Three powers of two, each dividing the next, so rounding the offset
//      and rounding the address are the same expression.
//
// ⇒ THE ELF ROW'S PRESCRIBED REMEDY WAS MEASURED AND REJECTED FOR THIS WRITER.
// Routing the four SECTION offsets through the shared `imageOffsetForAlignedVa`
// was written, built and then reverted: it moves no byte of any loadable image,
// and it would have been a PARTIAL adoption reading as a complete one — the four
// SEGMENT boundaries round file offsets too and CANNOT be converted, because an
// address-aligned segment start with a non-congruent file offset is exactly what
// EBADMACHO names. ✔MEASURED: with that change in place, the derived document
// below STILL trips the `__got` guard, which is the proof that the adoption was
// half of a thing rather than a thing.
//
// ⚠ AND THE ONE GAP IT LEAVES IS A CONFIG ONE, REPORTED NOT PAPERED OVER:
// `macho_backend`'s validate() requires `image.pageZeroSize` to be a POWER OF
// TWO "so that __TEXT.vmaddr (= pageZeroSize) preserves the kernel's mmap
// congruence" — but a power of two BELOW `segmentPageSize` passes that check and
// breaks the rule, and the neighbouring `__text.virtualAddress` check measures
// the offset WITHIN the segment rather than the segment's own address. The
// missing predicate is `pageZeroSize % segmentPageSize == 0`. Its owner is
// validate(), in a file this lane does not own; until it lands,
// `EveryShippedDarwinBaseIsAMultipleOfItsOwnPageSize` below asserts the premise
// the walker's correctness rests on, so it cannot rot unnoticed.

#include "core/types/alignment.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/macho.hpp"
#include "link/object_format_schema.hpp"
#include "link_test_support.hpp"
#include "repo_root.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using dss::link_format::test::readU32LE;
using dss::link_format::test::readU64LE;

namespace {

// Every image this file builds names the same artifact. The shipped darwin
// documents put `${artifactFileName}` in the ad-hoc signature's identifier, so
// a walker call that omits it is REFUSED rather than given a fabricated one.
constexpr std::string_view kArtifactName = "overalign_placement_pin";

// ── The two shipped exec ports. They are BOTH here because the ceiling this
//    file pins is READ FROM THE DOCUMENT and differs between them (4096 vs
//    16384) — a single-port pin would leave "the ceiling is a config value"
//    indistinguishable from "the ceiling is a constant that happens to match".
struct PortSpec {
    std::string_view label;
    std::string_view targetName;
    std::string_view formatName;
    std::vector<std::uint8_t> callBytes;   // call/BL <extern>; ret
    std::uint64_t callOffset;
};

[[nodiscard]] std::vector<PortSpec> ports() {
    return {
        {"x86_64-darwin", "x86_64", "macho64-x86_64-darwin-exec",
         {0xE8, 0, 0, 0, 0, 0xC3}, 1},
        {"arm64-darwin", "arm64", "macho64-arm64-darwin-exec",
         {0x00, 0x00, 0x00, 0x94, 0xC0, 0x03, 0x5F, 0xD6}, 0},
    };
}

// ── Mach-O readers. Field offsets are <mach-o/loader.h>'s and are spelled out
//    rather than named, so this decoder stays independent of the writer's own
//    record structs — a shared struct would let a writer bug and its pin move
//    together.
constexpr std::uint32_t kLcSegment64 = 0x19u;
constexpr std::size_t   kMachHeader64Size = 32;
constexpr std::size_t   kSegmentCommand64Size = 72;
constexpr std::size_t   kSection64Size = 80;

struct MachoSectionRow {
    std::string   segName;
    std::string   secName;
    std::uint64_t addr = 0;
    std::uint64_t size = 0;
    std::uint32_t offset = 0;
    std::uint32_t alignLog2 = 0;
    std::uint32_t flags = 0;

    // S_ZEROFILL (1), S_GB_ZEROFILL (0xC) and S_THREAD_LOCAL_ZEROFILL (0x12)
    // store no file bytes, so their `offset` is conventional and carries no
    // VA/file delta to keep.
    [[nodiscard]] bool zeroFill() const noexcept {
        std::uint32_t const type = flags & 0xFFu;
        return type == 0x1u || type == 0xCu || type == 0x12u;
    }
    [[nodiscard]] std::uint64_t alignBytes() const noexcept {
        return std::uint64_t{1} << alignLog2;
    }
};

struct MachoSegmentRow {
    std::string   name;
    std::uint64_t vmaddr = 0;
    std::uint64_t vmsize = 0;
    std::uint64_t fileoff = 0;
    std::uint64_t filesize = 0;
};

[[nodiscard]] std::string readName16(std::vector<std::uint8_t> const& b,
                                     std::size_t off) {
    std::string s;
    for (std::size_t i = 0; i < 16 && off + i < b.size(); ++i) {
        if (b[off + i] == 0) break;
        s.push_back(static_cast<char>(b[off + i]));
    }
    return s;
}

struct MachoImageView {
    std::vector<MachoSegmentRow> segments;
    std::vector<MachoSectionRow> sections;
};

[[nodiscard]] MachoImageView readImage(std::vector<std::uint8_t> const& b) {
    MachoImageView v;
    if (b.size() < kMachHeader64Size) return v;
    std::uint32_t const ncmds = readU32LE(b, 16);
    std::size_t cursor = kMachHeader64Size;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        if (cursor + 8 > b.size()) break;
        std::uint32_t const cmd     = readU32LE(b, cursor);
        std::uint32_t const cmdsize = readU32LE(b, cursor + 4);
        if (cmdsize == 0 || cursor + cmdsize > b.size()) break;
        if (cmd == kLcSegment64 && cmdsize >= kSegmentCommand64Size) {
            MachoSegmentRow seg;
            seg.name     = readName16(b, cursor + 8);
            seg.vmaddr   = readU64LE(b, cursor + 24);
            seg.vmsize   = readU64LE(b, cursor + 32);
            seg.fileoff  = readU64LE(b, cursor + 40);
            seg.filesize = readU64LE(b, cursor + 48);
            std::uint32_t const nsects = readU32LE(b, cursor + 64);
            for (std::uint32_t s = 0; s < nsects; ++s) {
                std::size_t const r =
                    cursor + kSegmentCommand64Size + s * kSection64Size;
                if (r + kSection64Size > b.size()) break;
                MachoSectionRow row;
                row.secName   = readName16(b, r);
                row.segName   = readName16(b, r + 16);
                row.addr      = readU64LE(b, r + 32);
                row.size      = readU64LE(b, r + 40);
                row.offset    = readU32LE(b, r + 48);
                row.alignLog2 = readU32LE(b, r + 52);
                row.flags     = readU32LE(b, r + 64);
                v.sections.push_back(std::move(row));
            }
            v.segments.push_back(std::move(seg));
        }
        cursor += cmdsize;
    }
    return v;
}

// ── The subject. THREE over-aligned items in THREE different sections, each
//    followed by an odd-sized filler.
//
// ★ THE FILLERS ARE LOAD-BEARING and it is the same reason the shipped runnable
// example carries eight objects: an over-aligned item that lands at its
// SECTION HEAD is right BY LUCK, because the section head is already aligned
// for an unrelated reason. The filler forces a real round-up inside the
// section AND makes the next section's base land off a multiple of the request,
// so the section after it must round too.
[[nodiscard]] AssembledModule makeModule(PortSpec const& port,
                                         std::uint32_t align) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;

    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = port.callBytes;
    Relocation rel;
    rel.offset = port.callOffset;
    rel.target = SymbolId{99};
    rel.kind   = RelocationKind{1};   // call rel32 / BL imm26
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));

    auto item = [](std::uint32_t id, DataSectionKind kind,
                   std::uint32_t a, std::size_t size) {
        AssembledData d;
        d.symbol    = SymbolId{id};
        d.section   = kind;
        d.alignment = Alignment::ofRuntimePow2(a);
        if (isZeroFill(kind)) d.reservedSize = size;
        else                  d.bytes.assign(size, 0x5Au);
        return d;
    };

    mod.dataItems.push_back(item(10, DataSectionKind::Rodata, align, 3));
    mod.dataItems.push_back(item(11, DataSectionKind::Rodata, 1u,    5));
    mod.dataItems.push_back(item(12, DataSectionKind::Data,   align, 5));
    mod.dataItems.push_back(item(13, DataSectionKind::Data,   1u,    3));
    mod.dataItems.push_back(item(14, DataSectionKind::Bss,    align, 7));

    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "_main",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    // An extern import is what routes `macho::encode` to `encodeExecDynamic`;
    // the static arm emits no data sections at all and would answer a different
    // question than the one this file asks.
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_abs", "/usr/lib/libSystem.B.dylib"});
    // These writer tests call the walker directly rather than through
    // `linker::link`, so they must SAY they want an untrampolined image — the
    // entry gate refuses a module that reached a walker with no override.
    mod.imageEntryOverride = 0u;
    return mod;
}

[[nodiscard]] std::vector<std::uint8_t> encodeImage(
        AssembledModule const& mod, TargetSchema const& target,
        ObjectFormatSchema const& fmt, DiagnosticReporter& rep) {
    return macho::encode(
        mod, target, fmt, rep,
        dss::ImageRequest{.artifactFileName = std::string{kArtifactName}});
}

[[nodiscard]] std::string diagText(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += d.actual + "\n";
    return s;
}

}  // namespace

// ── HALF ONE, POSITIVE ARM: at the strongest alignment the format admits,
//    every allocated section sits where its own header says it does ─────────
TEST(MachOImageOverAlignedSectionPlacement,
     EveryAllocatedSectionSitsOnItsOwnDeclaredAlignmentOnBothPorts) {
    for (auto const& port : ports()) {
        std::string const label{port.label};

        auto target = TargetSchema::loadShipped(port.targetName);
        ASSERT_TRUE(target.has_value()) << label;
        auto fmt = ObjectFormatSchema::loadShipped(port.formatName);
        ASSERT_TRUE(fmt.has_value()) << label;

        // The request is DERIVED from the document, never transcribed: it is
        // exactly the ceiling the walker enforces, so this cell is the last
        // value that must be PLACED and its sibling test's first value that
        // must be REFUSED.
        std::uint64_t const page = (**fmt).machoImage().segmentPageSize;
        ASSERT_GT(page, 1u) << label;

        DiagnosticReporter rep;
        auto bytes = encodeImage(
            makeModule(port, static_cast<std::uint32_t>(page)),
            **target, **fmt, rep);
        ASSERT_EQ(rep.errorCount(), 0u) << label << "\n" << diagText(rep);
        ASSERT_FALSE(bytes.empty()) << label << "\n" << diagText(rep);

        auto const image = readImage(bytes);
        ASSERT_FALSE(image.sections.empty()) << label;

        // The cell really reached the DYNAMIC exec arm. Without this the whole
        // matrix could be exercising the static walker — which emits no data
        // sections — while claiming to cover the one that places them.
        bool sawDataConst = false;
        for (auto const& s : image.segments)
            if (s.name == "__DATA_CONST") sawDataConst = true;
        ASSERT_TRUE(sawDataConst)
            << label << ": no __DATA_CONST segment, so this cell did not reach "
                        "the dynamic exec arm and says nothing about it";

        // The alignment REQUEST reached the section record. Asserted first and
        // separately: if `maxAlign` ever collapsed to the schema floor of 8,
        // every modulus below would pass while the object moved, and the pin
        // would read green over the exact defect it exists for.
        int sawRequested = 0;
        for (auto const& s : image.sections) {
            if (s.secName == "__const" || s.secName == "__data"
                || s.secName == "__bss") {
                ++sawRequested;
                EXPECT_EQ(s.alignBytes(), page)
                    << label << ": `" << s.secName << "` declares align 2^"
                    << s.alignLog2 << " but the module asked for " << page;
            }
        }
        EXPECT_EQ(sawRequested, 3)
            << label << ": expected __const, __data and __bss to be emitted";

        // THE RULE. `addr` is where dyld puts the section and where every
        // relocation against a member was resolved to.
        for (auto const& s : image.sections) {
            if (s.alignBytes() <= 1u) continue;
            EXPECT_EQ(s.addr % s.alignBytes(), 0u)
                << label << ": section `" << s.segName << "," << s.secName
                << "` declares align 2^" << s.alignLog2 << " and sits at addr 0x"
                << std::hex << s.addr << std::dec
                << " — the image contradicts its own record, and the program "
                   "runs on an object placed somewhere it did not ask to be";
        }

        // ONE mapping delta for the whole image, and it is `pageZeroSize`.
        // Rounding the file offset and rounding the address agree only while
        // that delta divides the alignment — this is the assertion that catches
        // the two derivations drifting apart, whichever of them was rounded.
        std::uint64_t const delta = (**fmt).machoImage().pageZeroSize;
        // ⚠ SCOPED TO SEGMENTS THAT MAP SECTIONS, and the scope is DERIVED
        // (nsects > 0) rather than a name list. `__PAGEZERO` maps nothing, and
        // `__LINKEDIT` legitimately carries its OWN delta — everything in it is
        // addressed by ABSOLUTE FILE OFFSET (LC_SYMTAB.symoff, the dyld-info
        // offsets), never by VA, so it owes the image base nothing.
        //
        // ✔MEASURED, AND THE FIRST VERSION OF THIS COMMENT WAS WRONG ABOUT WHY,
        // which is worth recording because the wrong reason was the plausible
        // one. It said ld64 gives __LINKEDIT its own delta too, citing numbers
        // read out of a link ld64 had REFUSED (a `-segalign` probe) — a layout
        // dump from a failed build is not a measurement of a built image. On a
        // CLEAN Apple clang 21.0.0 / ld-1267 arm64 exec every segment,
        // __LINKEDIT INCLUDED, sits at delta 0x100000000. ★ THE REAL REASON is
        // the ZERO-FILL TAIL: `__DATA` here holds `__bss`, so its `vmsize`
        // (0xc000) exceeds its page-rounded `filesize` (0x4000), and the VM and
        // file cursors part company at that segment — everything placed AFTER
        // it inherits the difference. DSS's own image shows __LINKEDIT at vmaddr
        // 0x100018000 / fileoff 0x14000, delta 0x100004000. ld64's probe agrees
        // by ACCIDENT of its subject's sizes: that program's `__DATA` VM and
        // file extents happen to round to the same page count, so nothing
        // diverges. Neither linker promises this segment a delta, and an
        // assertion over ALL segments would pin an accident.
        int mappedSegs = 0;
        for (auto const& s : image.segments) {
            bool carriesSections = false;
            for (auto const& sec : image.sections)
                if (sec.segName == s.name) carriesSections = true;
            if (!carriesSections) continue;
            ++mappedSegs;
            EXPECT_EQ(s.vmaddr - s.fileoff, delta)
                << label << ": segment `" << s.name << "` maps the file at a "
                   "different VA/file delta than `image.pageZeroSize` — dyld "
                   "maps file[fileoff …] at vmaddr, so its sections' bytes "
                   "would appear at addresses no symbol names";
        }
        EXPECT_GE(mappedSegs, 3)
            << label << ": expected at least __TEXT, __DATA_CONST and __DATA to "
                        "carry sections; fewer means this arm checked less than "
                        "it claims";
        for (auto const& s : image.sections) {
            if (s.zeroFill()) continue;             // stores no file bytes
            EXPECT_EQ(s.addr - s.offset, delta)
                << label << ": section `" << s.segName << "," << s.secName
                << "` maps at a different VA/file delta than the image base";
        }
    }
}

// ── HALF ONE, NEGATIVE ARM: above the declared page size the request cannot be
//    honoured by ANY Mach-O image, so it is refused rather than shipped ──────
//
// ⚠ THE CONTROL IS IN THE SAME TEST AND IS NAMED, because "refuses" is only
// meaningful beside a value that is ACCEPTED: a walker that refused everything
// would pass the refusal arm alone. The control is the page size itself — the
// last admitted value — so the two arms are one step apart and the boundary is
// pinned rather than merely straddled.
TEST(MachOImageOverAlignedSectionPlacement,
     AnAlignmentAboveTheDeclaredSegmentPageSizeIsRefusedByNameNotShippedAsACoinFlip) {
    for (auto const& port : ports()) {
        std::string const label{port.label};

        auto target = TargetSchema::loadShipped(port.targetName);
        ASSERT_TRUE(target.has_value()) << label;
        auto fmt = ObjectFormatSchema::loadShipped(port.formatName);
        ASSERT_TRUE(fmt.has_value()) << label;
        std::uint64_t const page = (**fmt).machoImage().segmentPageSize;
        ASSERT_GT(page, 1u) << label;

        // CONTROL: exactly at the ceiling, and it must BUILD.
        {
            DiagnosticReporter rep;
            auto bytes = encodeImage(
                makeModule(port, static_cast<std::uint32_t>(page)),
                **target, **fmt, rep);
            EXPECT_EQ(rep.errorCount(), 0u)
                << label << " [control @ page size " << page << "]\n"
                << diagText(rep);
            EXPECT_FALSE(bytes.empty()) << label << " [control]";
        }

        // ONE STEP ABOVE: refused, by code and by name.
        {
            DiagnosticReporter rep;
            auto bytes = encodeImage(
                makeModule(port, static_cast<std::uint32_t>(page * 2u)),
                **target, **fmt, rep);
            EXPECT_TRUE(bytes.empty())
                << label << ": an image was emitted for a static asking "
                << (page * 2u) << " bytes of alignment, which dyld's slide "
                   "cannot preserve — it would be correct on some runs and "
                   "wrong on others";
            bool sawCode = false;
            for (auto const& d : rep.all()) {
                if (d.code == DiagnosticCode::K_StaticObjectOveralignedForFormat)
                    sawCode = true;
            }
            EXPECT_TRUE(sawCode)
                << label << ": expected K_StaticObjectOveralignedForFormat, got:\n"
                << diagText(rep);
            // The message must carry the DECLARED page size, not a constant —
            // that number is what tells a reader the ceiling moved with the
            // document rather than with the architecture.
            EXPECT_NE(diagText(rep).find(std::to_string(page)), std::string::npos)
                << label << ": the refusal does not name the declared "
                            "segmentPageSize " << page << ":\n"
                << diagText(rep);
        }
    }
}


// ── HALF TWO, PREMISE ARM: the offset-rounded LATE chain is correct because of
//    a relationship between two document keys — so assert the relationship ────
//
// The walker's LATE chain rounds FILE OFFSETS where `elf.cpp` was forced to
// round addresses, and that is sound only while
// `image.pageZeroSize % image.segmentPageSize == 0` — the delta then divides
// every alignment the ceiling admits, and the two roundings coincide. That is a
// KERNEL rule (`__TEXT.vmaddr` IS pageZeroSize, `__TEXT.fileoff` is 0, and
// `vmaddr % page == fileoff % page`), but validate() checks only that
// pageZeroSize is a POWER OF TWO — which a value BELOW the page size satisfies
// while breaking the rule.
//
// ⚠ SO THIS IS NOT A TEST OF THE FORMAT DOCUMENTS FOR THEIR OWN SAKE. It is the
// premise the writer's arithmetic rests on, held still. If a future darwin
// document lowered its base, the walker would stop being right for a reason
// nothing else in the suite would name.
TEST(MachOImageOverAlignedSectionPlacement,
     EveryShippedDarwinBaseIsAMultipleOfItsOwnSegmentPageSize) {
    // Derived from the shipped population rather than a typed list, so a new
    // darwin document joins this rule by existing.
    for (std::string_view name : {"macho64-x86_64-darwin-exec",
                                  "macho64-arm64-darwin-exec",
                                  "macho64-x86_64-darwin-dylib",
                                  "macho64-arm64-darwin-dylib"}) {
        auto fmt = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(fmt.has_value()) << name;
        auto const& im = (**fmt).machoImage();
        ASSERT_GT(im.segmentPageSize, 0u) << name;
        EXPECT_EQ(im.pageZeroSize % im.segmentPageSize, 0u)
            << name << ": image.pageZeroSize 0x" << std::hex << im.pageZeroSize
            << " is not a multiple of image.segmentPageSize 0x"
            << im.segmentPageSize << std::dec
            << " — __TEXT.vmaddr IS pageZeroSize and __TEXT.fileoff is 0, so "
               "the kernel's `vmaddr % page == fileoff % page` rule is broken "
               "and the image will not load; and the walker's LATE layout, "
               "which rounds FILE OFFSETS, stops agreeing with the EARLY "
               "address chain it binds symbolVa from "
               "(D-LINK-MACHO-IMAGE-OVERALIGNED-STATIC-IS-A-LOAD-TIME-COIN-FLIP)";
    }
}

// ── HALF TWO, BEHAVIOUR ARM: when the premise IS broken, the walker refuses ──
//
// ★ THIS IS THE FIRST PIN ANY OF THE FOUR EARLY/LATE CONGRUENCE GUARDS HAS EVER
// HAD, and it is the assertion that separates Mach-O from the ELF row: there the
// two derivations drifted apart and the image SHIPPED with an object 4096 bytes
// from where it asked to be; here they are compared, and the image is withheld.
// A guard nothing exercises is a guard that can be deleted in a refactor with
// every suite still green — and these four are the only reason this writer's
// two independent VA chains are safe to keep.
//
// ⚠⚠ THE DOCUMENT IS SYNTHETIC AND UNLOADABLE, DELIBERATELY, AND THAT IS NOT A
// WEAKNESS OF THE PIN — IT IS THE FINDING. There is NO loadable Mach-O document
// that reaches this divergence (see the premise arm above), so a refusal is the
// only correct behaviour available, and "refuses loud" is the whole contract.
// A pin built on a shipped document would be green against an implementation
// that had no guards at all.
//
// ★ DERIVED, NEVER TRANSCRIBED, AND IN THE HOSTILE DIRECTION: the shipped
// document with two keys LOWERED. A fixture that built a fresh document would
// stop tracking the shipped one; one that RAISED the base would be green under
// the very implementation it exists to catch.
TEST(MachOImageOverAlignedSectionPlacement,
     AWalkerRefusesRatherThanShipAnImageWhoseTwoVaDerivationsDisagree) {
    for (auto const& port : ports()) {
        std::string const label{port.label};

        auto target = TargetSchema::loadShipped(port.targetName);
        ASSERT_TRUE(target.has_value()) << label;
        auto shipped = ObjectFormatSchema::loadShipped(port.formatName);
        ASSERT_TRUE(shipped.has_value()) << label;
        std::uint64_t const page = (**shipped).machoImage().segmentPageSize;
        ASSERT_GT(page, 1u) << label;

        auto const path = dss::test::configRoot()
                        / "object-formats"
                        / (std::string{port.formatName} + ".format.json");
        std::ifstream in(path, std::ios::binary);
        ASSERT_TRUE(in.good()) << label << ": cannot read " << path.string();
        std::ostringstream buf;
        buf << in.rdbuf();
        auto doc = nlohmann::json::parse(buf.str(), nullptr, false);
        ASSERT_FALSE(doc.is_discarded())
            << label << ": the shipped document did not parse as JSON";

        // A base BELOW the page size — still a power of two, which is all
        // validate() requires today, and still leaving
        // `virtualAddress - pageZeroSize` a whole page so the walker's OWN
        // `textSegmentVaMatchesFileOff` belt is satisfied and the divergence
        // this test is about is the one that fires.
        std::uint64_t const hostileBase = page / 2u;
        ASSERT_NE(hostileBase % page, 0u) << label;
        doc["image"]["pageZeroSize"] = hostileBase;
        bool movedText = false;
        for (auto& row : doc.at("sections")) {
            if (row.contains("kind") && row.at("kind") == "text") {
                row["virtualAddress"] = hostileBase + page;
                movedText = true;
            }
        }
        ASSERT_TRUE(movedText)
            << label << ": premise broken — the shipped document declares no "
                        "`text` section row to move";

        auto derived = ObjectFormatSchema::loadFromText(
            doc.dump(), "<darwin exec, base below the page>");
        // The document LOADING is itself part of the finding: validate() lets a
        // sub-page power-of-two base through, which is the gap this file's
        // header reports. If a future validate() closes it, this assertion is
        // where a reader is told the pin has moved to the boundary.
        if (!derived.has_value()) {
            std::string why;
            for (auto const& d : derived.error()) why += d.message + "\n";
            FAIL() << label
                   << ": validate() now REFUSES a sub-page pageZeroSize, which "
                      "is the better home for this rule — move this pin to the "
                      "schema boundary and delete this arm:\n" << why;
        }

        DiagnosticReporter rep;
        auto bytes = encodeImage(
            makeModule(port, static_cast<std::uint32_t>(page)),
            **target, **derived, rep);
        EXPECT_TRUE(bytes.empty())
            << label << ": the walker EMITTED an image whose EARLY (address) "
                        "and LATE (file-offset) VA chains cannot agree — every "
                        "relocation was resolved against one of them and every "
                        "section record written from the other";
        EXPECT_GT(rep.errorCount(), 0u)
            << label << ": no diagnostic at all — this is the ELF failure mode "
                        "exactly, a clean build over a misplaced object";
        EXPECT_NE(diagText(rep).find("congruence"), std::string::npos)
            << label << ": the refusal does not name the congruence that broke:\n"
            << diagText(rep);
    }
}
