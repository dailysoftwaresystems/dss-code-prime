// D-LINK-ELF-IMAGE-OVERALIGNED-DATA-PLACED-AT-ALIGNED-FILE-OFFSET
//
// An ELF image's allocated sections satisfy `va == imageBaseVa + fileOffset`.
// Both exec walkers used to choose ONE of the two and round IT to the section's
// alignment — the dynamic walker rounded the FILE OFFSET (`.rodata`, `.tdata`,
// `.data`) and the static walker rounded the DISTANCE FROM `.text`
// (`.rodata`). Either is `align`-aligned only when the quantity it was measured
// from — `imageBaseVa`, or `.text`'s VA — already is, and BOTH of those are
// DECLARED per format document.
//
// ⚠ SO THE LUCK RUNS IN OPPOSITE DIRECTIONS ON THE TWO SHIPPED PORTS, and that
// is the reason this file is a two-port matrix rather than one arm64 test:
//
//   `elf64-x86_64-linux-exec`   `.text` VA 0x401000 -> imageBaseVa 0x400000
//   `elf64-aarch64-linux-exec`  `.text` VA 0x400000 -> imageBaseVa 0x3FF000
//
// 0x400000 is a multiple of every power of two up to 4 MiB, so an over-aligned
// `.data` was placed CORRECTLY on x86_64 and 4096 bytes SHORT on arm64
// (✔MEASURED P64: `examples/c/alignment_overaligned_static_placed` returned 42
// and 54 respectively, from images whose `.data` header said
// `sh_addralign = 8192, sh_addr = 0x401000`). 0x401000 is NOT a multiple of
// 8192, so the static walker's `.rodata` was the mirror image: right on arm64,
// wrong on x86_64. A single-port pin would have called half of this fixed.
//
// ★ THE RULE ASSERTED HERE CANNOT BE SATISFIED BY LUCK, because it is checked
// rather than observed: for every allocated section the image emits,
// `sh_addr % sh_addralign == 0`, and `sh_addr - sh_offset` is the SAME constant
// for all of them (the one delta a PT_LOAD maps at). The alignment the request
// produced is asserted too — without that, a `maxAlign` that silently collapsed
// to the schema floor of 8 would make the modulus pass while the object moved.
//
// ⚠ THE FAILURE IS FAIL-QUIET, which is why the assertion is on the ADDRESS and
// not on a diagnostic: the misplaced image builds clean, loads, and runs. Its
// own section header is what contradicts it.

#include "asm/asm.hpp"
#include "core/types/alignment.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/elf.hpp"
#include "link/object_format_schema.hpp"
#include "link_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using dss::link_format::test::readU16LE;
using dss::link_format::test::readU32LE;
using dss::link_format::test::readU64LE;

namespace {

// The alignment every over-aligned item in this file asks for. 8192 is chosen
// because it is the largest value PE/COFF's four-bit IMAGE_SCN_ALIGN_* field can
// encode, so the same subject is expressible on every shipped format — and
// because it is exactly the request the shipped runnable example makes.
constexpr std::uint32_t kOverAlign = 8192u;

struct ElfSectionRow {
    std::string   name;
    std::uint32_t type = 0;
    std::uint64_t flags = 0;
    std::uint64_t addr = 0;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint64_t addrAlign = 0;
};

struct ElfProgramRow {
    std::uint32_t type = 0;
    std::uint64_t offset = 0;
    std::uint64_t vaddr = 0;
    std::uint64_t align = 0;
};

[[nodiscard]] std::string readCStr(std::vector<std::uint8_t> const& b,
                                   std::uint64_t off) {
    std::string s;
    for (std::uint64_t p = off; p < b.size() && b[p] != 0; ++p)
        s.push_back(static_cast<char>(b[p]));
    return s;
}

// Every section header, decoded. Elf64_Shdr is 64 bytes; the field offsets are
// the gABI's and are spelled out rather than named so this reader stays
// independent of the writer's own record struct.
[[nodiscard]] std::vector<ElfSectionRow> readSections(
        std::vector<std::uint8_t> const& b) {
    std::vector<ElfSectionRow> out;
    std::uint64_t const shoff    = readU64LE(b, 40);
    std::uint16_t const shnum    = readU16LE(b, 60);
    std::uint16_t const shstrndx = readU16LE(b, 62);
    std::uint64_t const shstrOff = readU64LE(b, shoff + shstrndx * 64 + 24);
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::uint64_t const r = shoff + i * 64;
        ElfSectionRow row;
        row.name      = readCStr(b, shstrOff + readU32LE(b, r + 0));
        row.type      = readU32LE(b, r + 4);
        row.flags     = readU64LE(b, r + 8);
        row.addr      = readU64LE(b, r + 16);
        row.offset    = readU64LE(b, r + 24);
        row.size      = readU64LE(b, r + 32);
        row.addrAlign = readU64LE(b, r + 48);
        out.push_back(std::move(row));
    }
    return out;
}

[[nodiscard]] std::vector<ElfProgramRow> readProgramHeaders(
        std::vector<std::uint8_t> const& b) {
    std::vector<ElfProgramRow> out;
    std::uint64_t const phoff = readU64LE(b, 32);
    std::uint16_t const phnum = readU16LE(b, 56);
    for (std::uint16_t i = 0; i < phnum; ++i) {
        std::uint64_t const r = phoff + i * 56;
        ElfProgramRow row;
        row.type   = readU32LE(b, r + 0);
        row.offset = readU64LE(b, r + 8);
        row.vaddr  = readU64LE(b, r + 16);
        row.align  = readU64LE(b, r + 48);
        out.push_back(row);
    }
    return out;
}

struct PortSpec {
    char const*               label;
    char const*               targetName;
    char const*               formatName;
    std::vector<std::uint8_t> retBytes;
    std::vector<std::uint8_t> callBytes;
    std::uint32_t             callOffset;
};

// The ONLY per-port data in this file: a `ret`, and a call whose patch site the
// dynamic arm's extern relocation names. Every assertion below is port-blind,
// which is the point — the placement rule lives in format-neutral substrate and
// no target may reach into it.
[[nodiscard]] std::vector<PortSpec> ports() {
    return {
        {"x86_64", "x86_64", "elf64-x86_64-linux-exec",
         {0xC3}, {0xE8, 0, 0, 0, 0, 0xC3}, 1},
        {"arm64", "arm64", "elf64-aarch64-linux-exec",
         {0xC0, 0x03, 0x5F, 0xD6},
         {0x00, 0x00, 0x00, 0x94, 0xC0, 0x03, 0x5F, 0xD6}, 0},
    };
}

// One over-aligned item in each of the three allocated data sections, plus a
// 3-byte filler after the `.data` one.
//
// ★ THE FILLER IS LOAD-BEARING. Without it `.data`'s span is a whole number of
// 8192-byte units and the NEXT section's base is already aligned, so `.bss`'s
// own placement would be free — the arm that has always been right would then
// be right for a reason the test never exercised. The odd size forces a real
// round-up. (This is the same reason the shipped runnable example interleaves
// odd-sized fillers; a one-object subject is right by luck.)
[[nodiscard]] AssembledModule makeOverAlignedModule(PortSpec const& port,
                                                    bool dynamicArm) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;

    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = dynamicArm ? port.callBytes : port.retBytes;
    if (dynamicArm) {
        Relocation rel;
        rel.offset = port.callOffset;
        rel.target = SymbolId{99};
        rel.kind   = RelocationKind{1};   // call rel32 / call26
        fn.relocations.push_back(rel);
    }
    mod.functions.push_back(std::move(fn));

    auto item = [](std::uint32_t id, DataSectionKind kind,
                   std::uint32_t align, std::size_t size) {
        AssembledData d;
        d.symbol    = SymbolId{id};
        d.section   = kind;
        d.alignment = Alignment::ofRuntimePow2(align);
        if (isZeroFill(kind)) d.reservedSize = size;
        else                  d.bytes.assign(size, 0x5Au);
        return d;
    };

    mod.dataItems.push_back(item(10, DataSectionKind::Rodata, kOverAlign, 3));
    mod.dataItems.push_back(item(11, DataSectionKind::Data,   kOverAlign, 5));
    mod.dataItems.push_back(item(12, DataSectionKind::Data,   1u,         3));
    mod.dataItems.push_back(item(13, DataSectionKind::Bss,    kOverAlign, 7));

    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "entry_fn",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    if (dynamicArm) {
        mod.externImports.push_back(
            ExternImport{SymbolId{99}, "printf", "libc.so.6"});
    }
    // These writer tests drive the walker directly rather than through
    // `linker::link`, so they must SAY they want an untrampolined image — the
    // entry gate rejects a module that reached a walker with no override.
    mod.imageEntryOverride = 0u;
    return mod;
}

}  // namespace

// ── The rule, on both ports and both image arms ───────────────────────────
//
// SHF_ALLOC = 0x2. A section that is not mapped has no address to get wrong.
TEST(ElfImageOverAlignedSectionPlacement,
     EveryAllocatedSectionSitsOnItsOwnDeclaredAlignmentOnBothPortsAndBothArms) {
    auto runCell = [](PortSpec const& port, bool dynamicArm) {
        std::string const label =
            std::string{port.label}
            + (dynamicArm ? " [dynamic image arm]" : " [static ET_EXEC arm]");

        auto target = TargetSchema::loadShipped(port.targetName);
        ASSERT_TRUE(target.has_value()) << label;
        auto fmt = ObjectFormatSchema::loadShipped(port.formatName);
        ASSERT_TRUE(fmt.has_value()) << label;

        AssembledModule mod = makeOverAlignedModule(port, dynamicArm);
        DiagnosticReporter rep;
        auto bytes = elf::encode(mod, **target, **fmt, rep);
        std::string diags;
        for (auto const& d : rep.all()) diags += d.actual + "\n";
        ASSERT_EQ(rep.errorCount(), 0u) << label << "\n" << diags;
        ASSERT_FALSE(bytes.empty()) << label << "\n" << diags;

        auto const sections = readSections(bytes);

        // The cell must really reach the arm it names. Without this the whole
        // matrix could be exercising ONE walker while claiming to cover two.
        bool sawDynsym = false;
        for (auto const& s : sections) if (s.name == ".dynsym") sawDynsym = true;
        ASSERT_EQ(sawDynsym, dynamicArm)
            << label << ": this cell did not reach the image arm it names, so "
                        "its result says nothing about that arm";

        // The alignment REQUEST reached the section header. Asserted first and
        // separately: if `maxAlign` ever collapsed to the schema floor, every
        // modulus below would pass while the object moved, and the pin would
        // read green over the exact defect it exists for.
        bool sawRodata = false, sawData = false, sawBss = false;
        for (auto const& s : sections) {
            if (s.name == ".rodata") { sawRodata = true; EXPECT_EQ(s.addrAlign, kOverAlign) << label; }
            if (s.name == ".data")   { sawData   = true; EXPECT_EQ(s.addrAlign, kOverAlign) << label; }
            if (s.name == ".bss")    { sawBss    = true; EXPECT_EQ(s.addrAlign, kOverAlign) << label; }
        }
        EXPECT_TRUE(sawRodata) << label << ": no `.rodata` emitted";
        EXPECT_TRUE(sawData)   << label << ": no `.data` emitted";
        EXPECT_TRUE(sawBss)    << label << ": no `.bss` emitted";

        // THE RULE. `sh_addr` is where the loader puts the section and where
        // every relocation against a member was resolved to.
        for (auto const& s : sections) {
            if ((s.flags & 0x2u) == 0u) continue;      // not SHF_ALLOC
            if (s.addrAlign <= 1u) continue;
            EXPECT_EQ(s.addr % s.addrAlign, 0u)
                << label << ": section `" << s.name << "` declares "
                << "sh_addralign=" << s.addrAlign << " and sits at sh_addr=0x"
                << std::hex << s.addr << std::dec
                << " — the image contradicts its own header, and the program "
                   "runs on an object placed somewhere it did not ask to be";
        }

        // ONE mapping delta for the whole image: `va == imageBaseVa + offset`.
        // Rounding the offset and rounding the address agree only while the
        // base divides the alignment — this is the assertion that catches the
        // two drifting apart, whichever of them was rounded.
        // NOBITS (`.bss`, type 8) is excluded: it stores no file bytes and its
        // sh_offset is conventional, so it has no delta to keep.
        bool haveDelta = false;
        std::uint64_t delta = 0;
        std::string   deltaOwner;
        for (auto const& s : sections) {
            if ((s.flags & 0x2u) == 0u) continue;      // not SHF_ALLOC
            if (s.type == 8u) continue;                // SHT_NOBITS
            if (!haveDelta) {
                haveDelta = true;
                delta = s.addr - s.offset;
                deltaOwner = s.name;
                continue;
            }
            EXPECT_EQ(s.addr - s.offset, delta)
                << label << ": `" << s.name << "` maps at a different "
                << "VA/file delta than `" << deltaOwner << "` — one PT_LOAD "
                   "cannot map both, so the loader would present the wrong "
                   "bytes at one of the two addresses";
        }

        // The kernel's own PT_LOAD rule, which an image that got the delta
        // right cannot fail — and an image that "fixed" alignment by moving a
        // segment's VA alone would.
        //
        // ★ AND THE SEGMENTS CARRY THE SAME ONE DELTA THE SECTIONS DO. The
        // loader maps `file[p_offset …]` at `p_vaddr`, so a segment whose delta
        // differs from the sections it contains puts their bytes at addresses
        // no symbol names — silently, and only for the over-aligned case, since
        // the two agree whenever nothing asks for more than a page.
        bool          haveSegDelta = false;
        std::uint64_t segDelta = 0;
        for (auto const& p : readProgramHeaders(bytes)) {
            if (p.type != 1u) continue;                // PT_LOAD
            ASSERT_GT(p.align, 0u) << label << ": PT_LOAD with p_align 0";
            EXPECT_EQ(p.vaddr % p.align, p.offset % p.align)
                << label << ": PT_LOAD p_vaddr 0x" << std::hex << p.vaddr
                << " and p_offset 0x" << p.offset << std::dec
                << " are not congruent mod p_align — execve() refuses this "
                   "image with ENOEXEC and says nothing about why";
            if (!haveSegDelta) { haveSegDelta = true; segDelta = p.vaddr - p.offset; }
            EXPECT_EQ(p.vaddr - p.offset, segDelta)
                << label << ": a PT_LOAD maps the file at a different delta "
                   "than its siblings";
            if (haveDelta) {
                EXPECT_EQ(segDelta, delta)
                    << label << ": the PT_LOADs map the file at a delta the "
                       "SECTION headers do not agree with — every global would "
                       "read the bytes of whatever lies at that distance";
            }
        }
    };

    for (auto const& port : ports()) {
        runCell(port, /*dynamicArm=*/false);
        runCell(port, /*dynamicArm=*/true);
    }
}

// ── The LOAD-TIME half: a base-relative image promises its own slide ──────
//
// An ET_DYN image (a PIE, or a `.so`) has no fixed address: the loader picks
// one, and the granularity it picks on is the LARGEST `p_align` among the
// image's PT_LOADs. So link-time addresses that are perfectly aligned prove
// NOTHING about where the member ends up — with `p_align` left at the page size
// a 8192-aligned object lands on an odd page half the time, and the image
// contains no record that it happened.
//
// ✔MEASURED P64, and the flicker is the whole point: twenty runs per port of a
// DSS PIE carrying `aligned(8192)` statics returned 42 or 50 depending on the
// run — 10 of 20 wrong on arm64, 12 of 20 on x86_64 — while gcc's PIE of the
// same source returned 42 twenty times out of twenty. A test that ran the
// binary ONCE would have called this green with even odds.
//
// ⇒ The property asserted here is the one that cannot flicker: every allocated
// section's alignment DIVIDES the largest `p_align` the image declares, so every
// slide the loader may choose preserves it.
TEST(ElfImageOverAlignedSectionPlacement,
     BaseRelativeImageDeclaresASlideGranularityEveryMemberSurvives) {
    struct DynFlavour { char const* label; char const* targetName;
                        char const* formatName; };
    std::vector<DynFlavour> const flavours{
        {"x86_64 PIE", "x86_64", "elf64-x86_64-linux-pie"},
        {"arm64 PIE",  "arm64",  "elf64-aarch64-linux-pie"},
        {"x86_64 .so", "x86_64", "elf64-x86_64-linux-dyn"},
        {"arm64 .so",  "arm64",  "elf64-aarch64-linux-dyn"},
    };
    auto const portList = ports();

    for (auto const& fl : flavours) {
        std::string const label = fl.label;
        auto target = TargetSchema::loadShipped(fl.targetName);
        ASSERT_TRUE(target.has_value()) << label;
        auto fmt = ObjectFormatSchema::loadShipped(fl.formatName);
        ASSERT_TRUE(fmt.has_value()) << label;
        PortSpec const& port =
            portList[std::string_view{fl.targetName} == "x86_64" ? 0u : 1u];

        AssembledModule mod = makeOverAlignedModule(port, /*dynamicArm=*/false);
        DiagnosticReporter rep;
        auto bytes = elf::encode(mod, **target, **fmt, rep);
        std::string diags;
        for (auto const& d : rep.all()) diags += d.actual + "\n";
        ASSERT_EQ(rep.errorCount(), 0u) << label << "\n" << diags;
        ASSERT_FALSE(bytes.empty()) << label << "\n" << diags;

        // e_type must really be ET_DYN (3) — otherwise this cell is asserting a
        // slide rule about an image that is never slid.
        ASSERT_EQ(readU16LE(bytes, 16), 3u)
            << label << ": not ET_DYN, so this cell says nothing about the "
                        "loader's slide";

        std::uint64_t maxPAlign = 0;
        for (auto const& p : readProgramHeaders(bytes)) {
            if (p.type == 1u) maxPAlign = std::max(maxPAlign, p.align);
        }
        ASSERT_GT(maxPAlign, 0u) << label << ": no PT_LOAD";

        for (auto const& s : readSections(bytes)) {
            if ((s.flags & 0x2u) == 0u) continue;      // not SHF_ALLOC
            if (s.addrAlign <= 1u) continue;
            EXPECT_EQ(maxPAlign % s.addrAlign, 0u)
                << label << ": section `" << s.name << "` wants "
                << s.addrAlign << "-byte alignment but the image's largest "
                << "PT_LOAD p_align is " << maxPAlign
                << " — the loader slides on that granularity, so the member "
                   "keeps its alignment only when the slide happens to be "
                   "lucky, and the run that fails looks exactly like the run "
                   "that does not";
        }
    }
}

// ── The CONTROL ───────────────────────────────────────────────────────────
//
// The same matrix with NO over-aligned member: every item asks for 8, which
// both shipped bases already satisfy. It must stay GREEN throughout every
// mutation of the placement code, because a mutant that reddens this one broke
// the WALKER rather than the over-alignment rule — "everything went red" is
// otherwise equally consistent with "the build broke".
TEST(ElfImageOverAlignedSectionPlacement,
     NaturallyAlignedMembersPlaceIdenticallyOnBothPortsAndBothArms) {
    auto runCell = [](PortSpec const& port, bool dynamicArm) {
        std::string const label =
            std::string{port.label}
            + (dynamicArm ? " [dynamic image arm]" : " [static ET_EXEC arm]");

        auto target = TargetSchema::loadShipped(port.targetName);
        ASSERT_TRUE(target.has_value()) << label;
        auto fmt = ObjectFormatSchema::loadShipped(port.formatName);
        ASSERT_TRUE(fmt.has_value()) << label;

        AssembledModule mod = makeOverAlignedModule(port, dynamicArm);
        for (auto& d : mod.dataItems) d.alignment = Alignment::of<8>();

        DiagnosticReporter rep;
        auto bytes = elf::encode(mod, **target, **fmt, rep);
        std::string diags;
        for (auto const& d : rep.all()) diags += d.actual + "\n";
        ASSERT_EQ(rep.errorCount(), 0u) << label << "\n" << diags;
        ASSERT_FALSE(bytes.empty()) << label << "\n" << diags;

        for (auto const& s : readSections(bytes)) {
            if ((s.flags & 0x2u) == 0u) continue;
            if (s.addrAlign <= 1u) continue;
            EXPECT_EQ(s.addr % s.addrAlign, 0u)
                << label << ": control section `" << s.name << "` misplaced";
        }
    };

    for (auto const& port : ports()) {
        runCell(port, /*dynamicArm=*/false);
        runCell(port, /*dynamicArm=*/true);
    }
}
