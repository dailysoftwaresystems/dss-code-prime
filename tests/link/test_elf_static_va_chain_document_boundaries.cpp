// D-LINK-ELF-STATIC-EXEC-VA-CHAIN-ROUNDS-TO-AN-UNDECLARED-PAGE
// D-LINK-ELF-STATIC-IMAGE-BASE-UNDERFLOWS-BELOW-ITS-OWN-HEADERS
//
// TWO WAYS `elf::encode`'s STATIC layout can read a format document and get a
// plausible answer out of arithmetic that had no business running. They share a
// file because they are one question asked twice — *what does the VA chain do
// with a document value it was never promised?* — and because a reader who saw
// only one of them would take the wrong lesson from it.
//
// ── (1) A DOCUMENT THAT DESCRIBES NO IMAGE ────────────────────────────────
//
// The writable-segment VA chain used to be computed on EVERY path, including
// the ET_REL one. Its first step is `alignUp(roSpanEndVa, fmt.elf().pageAlign)`,
// and the RELOCATABLE and STATICLIB documents declare NO `elf.pageAlign` at
// all — so the alignment arrived as 0, and `alignUp(v, 0)` returns 0 for every
// `v` (the mask form computes `~(0 - 1)` = 0 and erases the value).
//
// ★ IT WAS RIGHT ANYWAY, WHICH IS THE ONLY REASON IT SURVIVED: every consumer
// of those addresses sits inside the walker's `if (isExec)` block, and every
// `sh_addr` they reach is written `isExec ? va : 0`. Nothing observable moved.
// ⚠ SO NO BYTE-LEVEL ASSERTION CAN SEE THE DEFECT, and this file does not
// pretend otherwise: the instrument is the precondition `assert` in
// `detail::alignUp` (`src/link/format/byte_emit.hpp`), which aborts the process
// the moment a walker rounds to an alignment that is not a positive power of
// two. Running the relocatable and staticlib documents through the walker AT
// ALL is therefore the probe; the `sh_addr == 0` assertions below are the
// separate, always-on statement of what an ET_REL object's addresses must be —
// the property the gated chain must not disturb.
//
// ── (2) A DOCUMENT THAT DESCRIBES AN IMAGE STANDING BELOW ITS OWN HEADERS ──
//
// `.text`'s virtual address is DECLARED per document; its file offset is what
// the layout pass emits. The image's one mapping delta is the difference, and a
// document that puts `.text` below the Ehdr + program headers makes that
// difference WRAP. ⚠ AND THE WRAP CANCELS: every allocated section is then
// written at `va - imageBaseVa`, which is `va - textVa + textOff` — a perfectly
// ordinary small offset. `.rodata`'s file/VA congruence guard passes, the object
// bytes look sane, and the PT_LOAD that ships breaks the kernel's
// `p_vaddr % p_align == p_offset % p_align` rule, so execve() answers ENOEXEC
// and nothing in the toolchain ever said a word. ✔MEASURED with the guard
// removed, on BOTH ports: a clean image, `errorCount() == 0`, whose PT_LOAD #0
// maps file offset 0x1000 at address 0x800 with p_align 0x1000.
//
// ⚠ SO THE SUBJECT HERE IS A TEXT-ONLY IMAGE, and that is a measurement rather
// than a simplification — a module carrying `.data` is still refused, but by
// the `.data` congruence belt further down, which names the wrong thing. The
// cell below carries that measurement beside the module it chose.
//
// ★ validate() rejects only `virtualAddress == 0` on an ET_EXEC document, so
// this is reachable from a document it accepts — and the DYNAMIC walker has
// refused the same shape since it was written ("baseImageVa would underflow")
// while the static one had no such belt. That asymmetry inside one file is the
// finding; the fix states the rule against the offset actually emitted rather
// than against `pageAlign`, so it stays exact if the headers ever outgrow a
// page.

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
#include "repo_root.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using dss::link_format::test::readU16LE;
using dss::link_format::test::readU32LE;
using dss::link_format::test::readU64LE;

namespace {

struct ElfSectionRow {
    std::string   name;
    std::uint64_t flags = 0;
    std::uint64_t addr = 0;
    std::uint64_t addrAlign = 0;
};

[[nodiscard]] std::string readCStr(std::vector<std::uint8_t> const& b,
                                   std::uint64_t off) {
    std::string s;
    for (std::uint64_t p = off; p < b.size() && b[p] != 0; ++p)
        s.push_back(static_cast<char>(b[p]));
    return s;
}

// Elf64_Shdr is 64 bytes; the field offsets are the gABI's and are spelled out
// rather than named so this reader stays independent of the writer's own record.
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
        row.flags     = readU64LE(b, r + 8);
        row.addr      = readU64LE(b, r + 16);
        row.addrAlign = readU64LE(b, r + 48);
        out.push_back(std::move(row));
    }
    return out;
}

[[nodiscard]] std::string diagText(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) out += d.actual + "\n";
    return out;
}

// A `ret` per port — the only per-port datum in this file.
[[nodiscard]] std::vector<std::uint8_t> retBytes(std::string_view targetName) {
    if (targetName == "arm64") return {0xC0, 0x03, 0x5F, 0xD6};
    return {0xC3};
}

// ★ THE MODULE MUST REACH THE WRITABLE SEGMENT, or every assertion below is
// about a chain that was never built. One item in EACH of the three allocated
// data sections, over-aligned well past the schema floor of 8 so the round-up
// steps are real, and an odd-sized filler after the `.data` item so `.bss`'s
// base is not already aligned by luck (the same reason the over-alignment pin
// next door interleaves fillers).
[[nodiscard]] AssembledModule makeDataBearingModule(std::string_view targetName,
                                                    bool imageArm) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;

    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = retBytes(targetName);
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
    mod.dataItems.push_back(item(10, DataSectionKind::Rodata, 64u, 3));
    mod.dataItems.push_back(item(11, DataSectionKind::Data,   64u, 5));
    mod.dataItems.push_back(item(12, DataSectionKind::Data,    1u, 3));
    mod.dataItems.push_back(item(13, DataSectionKind::Bss,    64u, 7));

    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "entry_fn",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "ro_obj",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{11}, "rw_obj",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{12}, "rw_filler",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{13}, "zero_obj",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    // These writer tests drive the walker directly rather than through
    // `linker::link`, so an IMAGE must SAY it wants no entry trampoline — the
    // entry gate refuses a module that reached a walker with no override. An
    // ET_REL object has no entry at all and must NOT carry one.
    if (imageArm) mod.imageEntryOverride = 0u;
    return mod;
}

// An image with NO writable segment — one function and nothing else. Its whole
// point is that it reaches none of the `.data` / `.got` / `.bss` file-VA
// congruence belts, so a base that underflowed has nothing downstream to catch
// it.
[[nodiscard]] AssembledModule makeTextOnlyModule(std::string_view targetName) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = retBytes(targetName);
    mod.functions.push_back(std::move(fn));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "entry_fn",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.imageEntryOverride = 0u;
    return mod;
}

struct RelocatableDoc {
    char const* targetName;
    char const* formatName;
};

[[nodiscard]] std::vector<RelocatableDoc> relocatableDocs() {
    return {
        {"x86_64", "elf64-x86_64-linux"},
        {"arm64",  "elf64-aarch64-linux"},
        {"x86_64", "elf64-x86_64-linux-staticlib"},
        {"arm64",  "elf64-aarch64-linux-staticlib"},
    };
}

}  // namespace

// ── (1) A document that describes no image gets no image arithmetic ───────
//
// ⚠ THIS CELL'S REAL WORK IS RUNNING AT ALL. With `detail::alignUp`'s
// precondition assert shipped, a walker that rounds to an undeclared page
// aborts the process here rather than returning a collapsed 0 — which is what
// it did, in this exact configuration, until P65.
TEST(ElfRelocatableVaChain,
     NoShippedRelocatableOrStaticlibDocumentDeclaresAPageAndNoneGetsAnAddress) {
    for (auto const& doc : relocatableDocs()) {
        std::string const label = doc.formatName;

        auto target = TargetSchema::loadShipped(doc.targetName);
        ASSERT_TRUE(target.has_value()) << label;
        auto fmt = ObjectFormatSchema::loadShipped(doc.formatName);
        ASSERT_TRUE(fmt.has_value()) << label;

        // THE PREMISE, asserted rather than assumed. This cell exists to drive
        // the walker over a document that declares NO page; if one of these
        // documents ever gains an `elf.pageAlign`, the cell stops exercising
        // the absent-key path and would go quietly vacuous.
        EXPECT_EQ((**fmt).elf().pageAlign, 0u)
            << label << ": this document now DECLARES a page alignment, so it "
                        "no longer exercises the undeclared-page path this cell "
                        "was written for — move the pin to a document that "
                        "still declares none, or delete it and say why";

        DiagnosticReporter rep;
        auto bytes = elf::encode(makeDataBearingModule(doc.targetName,
                                                       /*imageArm=*/false),
                                 **target, **fmt, rep);
        ASSERT_EQ(rep.errorCount(), 0u) << label << "\n" << diagText(rep);
        ASSERT_FALSE(bytes.empty()) << label << "\n" << diagText(rep);

        // The cell reached the arm it names: e_type (Elf64_Ehdr+16) = ET_REL.
        EXPECT_EQ(readU16LE(bytes, 16), 1u)
            << label << ": this is not a relocatable object, so its result says "
                        "nothing about the relocatable path";
        // ... and the writable segment was really built: without these three
        // sections the chain under test is never evaluated and every assertion
        // below passes over a module that asked for nothing.
        auto const sections = readSections(bytes);
        bool sawRodata = false, sawData = false, sawBss = false;
        for (auto const& s : sections) {
            if (s.name == ".rodata") sawRodata = true;
            if (s.name == ".data")   sawData   = true;
            if (s.name == ".bss")    sawBss    = true;
        }
        EXPECT_TRUE(sawRodata) << label << ": no `.rodata` emitted";
        EXPECT_TRUE(sawData)   << label << ": no `.data` emitted";
        EXPECT_TRUE(sawBss)    << label << ": no `.bss` emitted";

        // THE RULE. Every section of an ET_REL object is UNBOUND: the linker
        // that consumes it chooses the addresses, and a non-zero `sh_addr` here
        // would be this writer claiming a placement it has no standing to make.
        for (auto const& s : sections) {
            EXPECT_EQ(s.addr, 0u)
                << label << ": section `" << s.name << "` carries sh_addr=0x"
                << std::hex << s.addr << std::dec
                << " in a relocatable object — an ET_REL section is unbound, "
                   "and a consumer that trusted this would place it twice";
        }
    }
}

// ── (2) An image standing below its own headers is REFUSED ────────────────
TEST(ElfStaticImageBase, ADeclaredTextVaBelowTheEmittedHeadersIsRefused) {
    struct Port { char const* targetName; char const* formatName; };
    for (auto const& port : std::vector<Port>{
             {"x86_64", "elf64-x86_64-linux-exec"},
             {"arm64",  "elf64-aarch64-linux-exec"}}) {
        std::string const label = port.formatName;

        auto target = TargetSchema::loadShipped(port.targetName);
        ASSERT_TRUE(target.has_value()) << label;
        auto shipped = ObjectFormatSchema::loadShipped(port.formatName);
        ASSERT_TRUE(shipped.has_value()) << label;
        std::uint64_t const page = (**shipped).elf().pageAlign;
        ASSERT_GT(page, 1u) << label;

        // THE CONTROL, first and in the same cell so it cannot drift from the
        // subject: the SHIPPED document builds an image. Without it a refusal
        // below would be indistinguishable from a walker that refuses this
        // module for some other reason entirely.
        {
            DiagnosticReporter rep;
            auto bytes = elf::encode(makeDataBearingModule(port.targetName,
                                                           /*imageArm=*/true),
                                     **target, **shipped, rep);
            ASSERT_EQ(rep.errorCount(), 0u)
                << label << " [control, shipped document]\n" << diagText(rep);
            ASSERT_FALSE(bytes.empty())
                << label << " [control, shipped document]\n" << diagText(rep);
            EXPECT_EQ(readU16LE(bytes, 16), 2u)
                << label << " [control]: not an ET_EXEC image, so the subject "
                            "arm below is not testing what it claims";
        }

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

        // A `.text` VA that is non-zero — all validate() asks of it today — but
        // BELOW the page the Ehdr + program headers occupy. Half a page, so the
        // value is still a plausible-looking address and the walker's `.rodata`
        // congruence guard is still satisfied (it compares two quantities that
        // both wrap, and both wraps cancel).
        std::uint64_t const hostileTextVa = page / 2u;
        ASSERT_GT(hostileTextVa, 0u) << label;
        bool movedText = false;
        for (auto& row : doc.at("sections")) {
            if (row.contains("kind") && row.at("kind") == "text") {
                row["virtualAddress"] = hostileTextVa;
                movedText = true;
            }
        }
        ASSERT_TRUE(movedText)
            << label << ": premise broken — the shipped document declares no "
                        "`text` section row to move";

        auto derived = ObjectFormatSchema::loadFromText(
            doc.dump(), "<elf exec, text below its own headers>");
        // The document LOADING is part of the finding: validate() lets a
        // sub-header `.text` VA through, which is why the walker needs a belt at
        // all. If a future validate() closes it, this is where a reader is told
        // the rule moved to the better home.
        if (!derived.has_value()) {
            std::string why;
            for (auto const& d : derived.error()) why += d.message + "\n";
            FAIL() << label
                   << ": validate() now REFUSES a `.text` VA below the image "
                      "headers, which is the better home for this rule — move "
                      "this pin to the schema boundary and delete this arm:\n"
                   << why;
        }

        // ⚠ THE SUBJECT MODULE CARRIES NO WRITABLE DATA, AND THAT IS MEASURED,
        // NOT STYLISTIC. ✔With the guard removed, a module WITH `.data` is
        // still refused — by the `.data` file/VA congruence belt further down,
        // which reports `textVa(2048) + fileDelta(4096) != dataSectionVa(4096)`
        // and names `.data` rather than the base that underflowed. A text-only
        // image reaches no such belt: `.rodata`'s congruence holds (both sides
        // wrap and cancel), so the image is EMITTED CLEAN and the only thing
        // wrong with it is the PT_LOAD the kernel reads. That shape is the one
        // this guard exists for, so that is the shape asserted here.
        DiagnosticReporter rep;
        auto bytes = elf::encode(makeTextOnlyModule(port.targetName),
                                 **target, **derived, rep);
        EXPECT_TRUE(bytes.empty())
            << label << ": the walker EMITTED an image whose base address "
                        "underflowed — every section was written at an offset "
                        "whose two wraps cancel, so the bytes look right and "
                        "the PT_LOAD the kernel reads does not";
        EXPECT_GT(rep.errorCount(), 0u)
            << label << ": no diagnostic at all — a clean build over an image "
                        "execve() will refuse with ENOEXEC";
        EXPECT_NE(diagText(rep).find("UNDERFLOW"), std::string::npos)
            << label << ": the refusal does not name what went wrong:\n"
            << diagText(rep);
        // ★ AND IF AN IMAGE DID SHIP, SAY WHAT IS WRONG WITH IT RATHER THAN
        // ONLY THAT IT SHIPPED. Dead once the guard is in place (`bytes` is
        // empty), and the whole content of the failure when it is not: every
        // PT_LOAD's `p_vaddr % p_align` against its `p_offset % p_align`, which
        // is the equality the Linux kernel checks before it will exec the file.
        if (!bytes.empty()) {
            std::uint64_t const phoff = readU64LE(bytes, 32);
            std::uint16_t const phnum = readU16LE(bytes, 56);
            for (std::uint16_t i = 0; i < phnum; ++i) {
                std::uint64_t const r      = phoff + i * 56;
                std::uint64_t const offset = readU64LE(bytes, r + 8);
                std::uint64_t const vaddr  = readU64LE(bytes, r + 16);
                std::uint64_t const align  = readU64LE(bytes, r + 48);
                if (align <= 1) continue;
                EXPECT_EQ(vaddr % align, offset % align)
                    << label << ": the emitted PT_LOAD #" << i << " maps file "
                    << "offset 0x" << std::hex << offset << " at address 0x"
                    << vaddr << " with p_align 0x" << align << std::dec
                    << " — execve() answers ENOEXEC on that, and the build that "
                       "produced it said nothing";
            }
        }
    }
}
