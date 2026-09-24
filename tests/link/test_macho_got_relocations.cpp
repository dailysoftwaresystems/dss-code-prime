// P68 round 11 — MACH-O GOT_LOAD / GOT: A CLANG MACH-O OBJECT REACHES AN EXTERN
// THROUGH A GOT SLOT, AND AN IMAGE LINK LOWERS IT THE WAY IT LOWERS ELF'S
// (D-LK-MACHO-GOT-RELOCATIONS-REFUSED-AT-READ; the ELF twins
// D-LK-ELF-READER-REFUSES-GOTPCREL-BLOCKS-REAL-GLIBC-MEMBERS and
// D-LK-ELF-AARCH64-EXEC-REFUSES-GOT-RELOCATIONS).
//
// ✔MEASURED 2026-09-24: clang 18 `-O2 --target={arm64,x86_64}-apple-macos11`
// reads an extern datum, takes an extern array element's or function's address,
// null-checks a weak_import function and reads a libSystem datum (`___stdoutp`)
// through a GOT slot — ARM64_RELOC_GOT_LOAD_PAGE21 + GOT_LOAD_PAGEOFF12, and
// X86_64_RELOC_GOT_LOAD / X86_64_RELOC_GOT — and DSS refused every such member at
// READ ("relocation nativeId 1677721600 … is not declared"; x86_64 889192448).
// Apple clang 21 on the Mac writes the same types and the same x86_64 remainders
// for the same sources, and Apple's ld links them to programs that exit 42 on both
// ISAs (✔MEASURED 2026-09-24): it relaxes a GOT_LOAD whose target the image
// defines and keeps a `__got` slot for every other site — the slot DSS keeps for
// all of them.
//
// ── WHAT EACH TEST HERE IS FOR ─────────────────────────────────────────
//  A. x86_64's in-place remainder: Mach-O measures the GOT displacement from the
//     END of its field and keeps the rest IN the field (0, −1 before an imm8, −4
//     before an imm32, +8 for `sym@GOTPCREL+8`), so the reader must recover it —
//     the target's `gotriprel32` rows carry bias −4 as `riprel32` does.
//  B. The real members through `linker::link` into a Mach-O exec: every GOT site
//     addresses ONE slot per symbol (x86_64) or per (symbol, addend) (arm64), and
//     the slot is filled the way the Mach-O writer fills any pointer-holding
//     datum: the definition's address under a REBASE for a symbol the program
//     defines (the weak_import function among them — the program defines it, so
//     clang's null check, an X86_64_RELOC_GOT with remainder −1, reads a live
//     slot), and a BIND for a libSystem datum. (A weak function NOTHING defines
//     is refused by the image link before any slot exists — the image cannot
//     bind it; the slot-holds-0 rule for a weak symbol resolved to nothing is
//     pinned at the unit tier, test_got_slot_lowering.cpp.) An x86_64 site's slot
//     is read back off its patched displacement with the ASSEMBLER's remainder,
//     listed below, never the one the reader recovered: the CPU measures from the
//     instruction's end, so a misread remainder must fail here too, not only in A.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/macho_object_reader.hpp"
#include "link/got_slots.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"

#include "macho_got_member_objects.inc"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] std::shared_ptr<TargetSchema const> target(char const* name) {
    auto t = TargetSchema::loadShipped(name);
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(" << name << ") failed";
        return nullptr;
    }
    return std::move(t).value();
}
[[nodiscard]] std::shared_ptr<ObjectFormatSchema const> format(char const* name) {
    auto f = ObjectFormatSchema::loadShipped(name);
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(" << name << ") failed";
        return nullptr;
    }
    return std::move(f).value();
}
[[nodiscard]] std::string errorText(DiagnosticReporter const& rep) {
    std::string all;
    for (auto const& d : rep.all()) {
        if (d.severity == DiagnosticSeverity::Error) all += d.actual + "\n";
    }
    return all;
}

// ── A Mach-O image, read back ────────────────────────────────────────────────

[[nodiscard]] std::uint64_t rd(std::vector<std::uint8_t> const& b, std::uint64_t off, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= static_cast<std::uint64_t>(b.at(off + i)) << (8 * i);
    return v;
}
[[nodiscard]] std::string fixedName(std::vector<std::uint8_t> const& b, std::uint64_t off) {
    std::string s;
    for (int i = 0; i < 16 && b.at(off + i) != 0; ++i) s.push_back(static_cast<char>(b[off + i]));
    return s;
}

struct Section {
    std::string   seg, name;
    std::uint64_t addr = 0, size = 0;
    std::uint32_t offset = 0, flags = 0;
};
struct Image {
    std::vector<Section>       sections;
    std::vector<std::uint64_t> segmentVmaddr;   // by LC_SEGMENT_64 index
    std::uint32_t symoff = 0, nsyms = 0, stroff = 0;
    std::uint32_t rebaseOff = 0, rebaseSize = 0, bindOff = 0, bindSize = 0;
};

[[nodiscard]] Image readImage(std::vector<std::uint8_t> const& b) {
    Image img;
    auto const ncmds = static_cast<std::uint32_t>(rd(b, 16, 4));
    std::uint64_t off = 32;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        auto const cmd = static_cast<std::uint32_t>(rd(b, off, 4));
        auto const size = static_cast<std::uint32_t>(rd(b, off + 4, 4));
        if (cmd == 0x19u) {  // LC_SEGMENT_64
            img.segmentVmaddr.push_back(rd(b, off + 24, 8));
            auto const nsects = static_cast<std::uint32_t>(rd(b, off + 64, 4));
            for (std::uint32_t s = 0; s < nsects; ++s) {
                std::uint64_t const so = off + 72 + s * 80ull;
                img.sections.push_back({fixedName(b, so + 16), fixedName(b, so), rd(b, so + 32, 8),
                                        rd(b, so + 40, 8), static_cast<std::uint32_t>(rd(b, so + 48, 4)),
                                        static_cast<std::uint32_t>(rd(b, so + 64, 4))});
            }
        } else if (cmd == 0x2u) {  // LC_SYMTAB
            img.symoff = static_cast<std::uint32_t>(rd(b, off + 8, 4));
            img.nsyms  = static_cast<std::uint32_t>(rd(b, off + 12, 4));
            img.stroff = static_cast<std::uint32_t>(rd(b, off + 16, 4));
        } else if (cmd == 0x80000022u) {  // LC_DYLD_INFO_ONLY
            img.rebaseOff  = static_cast<std::uint32_t>(rd(b, off + 8, 4));
            img.rebaseSize = static_cast<std::uint32_t>(rd(b, off + 12, 4));
            img.bindOff    = static_cast<std::uint32_t>(rd(b, off + 16, 4));
            img.bindSize   = static_cast<std::uint32_t>(rd(b, off + 20, 4));
        }
        off += size;
    }
    return img;
}

[[nodiscard]] Section const* sectionHolding(Image const& img, std::uint64_t va) {
    for (auto const& s : img.sections) {
        if (va >= s.addr && va < s.addr + s.size) return &s;
    }
    return nullptr;
}
[[nodiscard]] std::optional<std::uint64_t> fileOffsetOf(Image const& img, std::uint64_t va) {
    auto const* s = sectionHolding(img, va);
    if (s == nullptr || s->offset == 0) return std::nullopt;  // zero-fill has no file bytes
    return s->offset + (va - s->addr);
}
[[nodiscard]] std::optional<std::uint64_t> symbolValue(std::vector<std::uint8_t> const& b,
                                                       Image const& img, std::string const& want) {
    for (std::uint32_t i = 0; i < img.nsyms; ++i) {
        std::uint64_t const e = img.symoff + i * 16ull;
        auto const strx = static_cast<std::uint32_t>(rd(b, e, 4));
        std::string name;
        for (std::uint64_t p = img.stroff + strx; p < b.size() && b[p] != 0; ++p) {
            name.push_back(static_cast<char>(b[p]));
        }
        if (name == want && (b.at(e + 4) & 0x0Eu) == 0x0Eu) return rd(b, e + 8, 8);  // N_SECT
    }
    return std::nullopt;
}

[[nodiscard]] std::uint64_t uleb(std::vector<std::uint8_t> const& b, std::uint64_t& p) {
    std::uint64_t v = 0;
    int shift = 0;
    while (true) {
        std::uint8_t const c = b.at(p++);
        v |= static_cast<std::uint64_t>(c & 0x7Fu) << shift;
        shift += 7;
        if ((c & 0x80u) == 0) return v;
    }
}

// Every REBASE site of the legacy opcode stream, as a VA.
[[nodiscard]] std::vector<std::uint64_t> rebaseSites(std::vector<std::uint8_t> const& b, Image const& img) {
    std::vector<std::uint64_t> out;
    std::uint64_t p = img.rebaseOff, end = img.rebaseOff + img.rebaseSize, segOff = 0;
    std::size_t seg = 0;
    auto va = [&] { return img.segmentVmaddr.at(seg) + segOff; };
    while (p < end) {
        std::uint8_t const byte = b.at(p++);
        std::uint8_t const op = byte & 0xF0u, imm = byte & 0x0Fu;
        if (op == 0x00) break;
        switch (op) {
            case 0x10: break;                                              // SET_TYPE_IMM
            case 0x20: seg = imm; segOff = uleb(b, p); break;              // SET_SEGMENT_AND_OFFSET_ULEB
            case 0x30: segOff += uleb(b, p); break;                        // ADD_ADDR_ULEB
            case 0x40: segOff += imm * 8ull; break;                        // ADD_ADDR_IMM_SCALED
            case 0x50: for (int i = 0; i < imm; ++i) { out.push_back(va()); segOff += 8; } break;
            case 0x60: { auto const n = uleb(b, p); for (std::uint64_t i = 0; i < n; ++i) { out.push_back(va()); segOff += 8; } break; }
            case 0x70: out.push_back(va()); segOff += uleb(b, p) + 8; break;
            case 0x80: { auto const n = uleb(b, p); auto const skip = uleb(b, p);
                         for (std::uint64_t i = 0; i < n; ++i) { out.push_back(va()); segOff += skip + 8; } break; }
            default: ADD_FAILURE() << "unknown rebase opcode 0x" << std::hex << +byte; return out;
        }
    }
    return out;
}

// Every BIND of the non-lazy opcode stream: VA -> symbol name.
[[nodiscard]] std::map<std::uint64_t, std::string> bindSites(std::vector<std::uint8_t> const& b, Image const& img) {
    std::map<std::uint64_t, std::string> out;
    std::uint64_t p = img.bindOff, end = img.bindOff + img.bindSize, segOff = 0;
    std::size_t seg = 0;
    std::string sym;
    auto bind = [&] { out[img.segmentVmaddr.at(seg) + segOff] = sym; };
    while (p < end) {
        std::uint8_t const byte = b.at(p++);
        std::uint8_t const op = byte & 0xF0u, imm = byte & 0x0Fu;
        if (op == 0x00) break;
        switch (op) {
            case 0x10: case 0x30: case 0x50: break;                        // ordinal / special / type
            case 0x20: (void)uleb(b, p); break;                            // SET_DYLIB_ORDINAL_ULEB
            case 0x40: sym.clear(); while (b.at(p) != 0) sym.push_back(static_cast<char>(b[p++])); ++p; break;
            case 0x60: (void)uleb(b, p); break;                            // SET_ADDEND_SLEB (7-bit groups)
            case 0x70: seg = imm; segOff = uleb(b, p); break;
            case 0x80: segOff += uleb(b, p); break;
            case 0x90: bind(); segOff += 8; break;
            case 0xA0: bind(); segOff += uleb(b, p) + 8; break;
            case 0xB0: bind(); segOff += imm * 8ull + 8; break;
            case 0xC0: { auto const n = uleb(b, p); auto const skip = uleb(b, p);
                         for (std::uint64_t i = 0; i < n; ++i) { bind(); segOff += skip + 8; } break; }
            default: ADD_FAILURE() << "unknown bind opcode 0x" << std::hex << +byte; return out;
        }
    }
    return out;
}

// ── The program the members are linked into ──────────────────────────────────

constexpr std::uint8_t kDatumMarker[8]  = {0x4D, 0x41, 0x43, 0x48, 0x2D, 0x4F, 0x21, 0x07};
constexpr std::uint8_t kTableMarker[16] = {0x01, 0x51, 0xC3, 0x5A, 0x02, 0x51, 0xC3, 0x5A,
                                           0x19, 0x51, 0xC3, 0x5A, 0x04, 0x51, 0xC3, 0x5A};

[[nodiscard]] AssembledModule programModule(bool arm64) {
    AssembledModule m;
    m.cuId = CompilationUnitId{1};
    std::vector<std::uint8_t> const ret =
        arm64 ? std::vector<std::uint8_t>{0xC0, 0x03, 0x5F, 0xD6} : std::vector<std::uint8_t>{0xC3};
    AssembledFunction mainFn;
    mainFn.symbol = SymbolId{1};
    mainFn.bytes  = ret;
    AssembledFunction extFn;
    extFn.symbol = SymbolId{2};
    extFn.bytes  = ret;
    AssembledFunction weakFn;
    weakFn.symbol = SymbolId{5};
    weakFn.bytes  = ret;
    m.functions = {mainFn, extFn, weakFn};
    m.expectedFuncCount = 3;
    AssembledData datum;
    datum.symbol    = SymbolId{3};
    datum.section   = DataSectionKind::Data;
    datum.bytes.assign(std::begin(kDatumMarker), std::end(kDatumMarker));
    datum.alignment = Alignment::ofRuntimePow2(8);
    AssembledData table;
    table.symbol    = SymbolId{4};
    table.section   = DataSectionKind::Data;
    table.bytes.assign(std::begin(kTableMarker), std::end(kTableMarker));
    table.alignment = Alignment::ofRuntimePow2(8);
    m.dataItems = {datum, table};
    for (auto const& [id, name] : std::vector<std::pair<std::uint32_t, char const*>>{
             {1, "_main"}, {2, "_ext_fn"}, {3, "_ext_datum"}, {4, "_ext_table"}, {5, "_weak_fn"}}) {
        m.symbols.push_back(ModuleSymbol{SymbolId{id}, name, SymbolBinding::Global,
                                         SymbolVisibility::Default});
    }
    m.userEntrySymbol = SymbolId{1};
    return m;
}

// The address of one of the program's definitions: a function by `nlist`, a
// datum by its marker bytes (found in a file-backed section).
[[nodiscard]] std::optional<std::uint64_t> addressOf(std::vector<std::uint8_t> const& b,
                                                     Image const& img, std::string const& name) {
    std::vector<std::uint8_t> marker;
    if (name == "_ext_datum") marker.assign(std::begin(kDatumMarker), std::end(kDatumMarker));
    if (name == "_ext_table") marker.assign(std::begin(kTableMarker), std::end(kTableMarker));
    if (marker.empty()) return symbolValue(b, img, name);
    for (auto const& s : img.sections) {
        if (s.offset == 0) continue;
        for (std::uint64_t o = s.offset; o + marker.size() <= s.offset + s.size; ++o) {
            if (std::equal(marker.begin(), marker.end(), b.begin() + static_cast<std::ptrdiff_t>(o))) {
                return s.addr + (o - s.offset);
            }
        }
    }
    return std::nullopt;
}

// One GOT-slot-relative site of a member, by the names the image will carry.
struct GotSite {
    std::string      function;
    std::uint64_t    offset = 0;
    std::int64_t     addend = 0;
    std::string      target;
    std::string      kindName;
    RelocFormulaKind formula = RelocFormulaKind::Linear;
};
struct Members {
    std::vector<AssembledModule> modules;
    std::vector<GotSite>         sites;
};

[[nodiscard]] Members readMembers(TargetSchema const& t, ObjectFormatSchema const& reader,
                                  std::vector<std::vector<std::uint8_t>> const& objects) {
    Members out;
    std::uint32_t cu = 2;
    for (auto const& bytes : objects) {
        DiagnosticReporter rep;
        auto m = macho::readRelocatableObject(bytes, t, reader, rep, CompilationUnitId{cu++});
        if (!m.has_value()) {
            ADD_FAILURE() << "a clang Mach-O member failed to READ:\n" << errorText(rep);
            continue;
        }
        auto const nameOf = [&](SymbolId id) {
            for (auto const& s : m->symbols) if (s.symbol == id) return s.name;
            for (auto const& e : m->externImports) if (e.symbol == id) return e.mangledName;
            return std::string{};
        };
        for (auto const& fn : m->functions) {
            for (auto const& rel : fn.relocations) {
                auto const* tri = t.relocationInfo(rel.kind);
                if (tri == nullptr || !relocFormulaFacts(tri->formulaKind).isGotSlotRelative) continue;
                out.sites.push_back({nameOf(fn.symbol), rel.offset, rel.addend, nameOf(rel.target),
                                     tri->name, tri->formulaKind});
            }
        }
        // `___stdoutp` is libSystem's; the pipeline's archive binder would say so.
        for (auto& e : m->externImports) {
            if (e.mangledName == "___stdoutp") e.libraryPath = "/usr/lib/libSystem.B.dylib";
        }
        out.modules.push_back(std::move(*m));
    }
    return out;
}

// `_shapes`' sites as the assembler wrote them: (offset, row, remainder in place).
// The .inc lists the instructions; Apple's assembler writes the same bytes.
std::vector<std::tuple<std::uint64_t, std::string, std::int64_t>> const kShapesSites{
    {0x03, "gotriprel32_load", 0},   // movq  sym@GOTPCREL(%rip), %rax
    {0x0a, "gotriprel32", -1},       // cmpq  $0, sym@GOTPCREL(%rip)       — an imm8 follows
    {0x12, "gotriprel32", 0},        // addq
    {0x18, "gotriprel32", 0},        // pushq
    {0x1e, "gotriprel32", 0},        // callq *
    {0x25, "gotriprel32", 8},        // movq  sym@GOTPCREL+8(%rip)
    {0x2b, "gotriprel32", -4},       // movl  $imm32, sym@GOTPCREL(%rip)    — an imm32 follows
};

// The remainder the ASSEMBLER left in an x86_64 site's field, known without the
// reader: `_shapes`' from the table above, and c_refs' from its disassembly (every
// site a plain load, 0, but the weak null check `cmpq $0, …`, −1).
[[nodiscard]] std::optional<std::int64_t> assemblerRemainder(std::string const& function, std::uint64_t offset) {
    if (function == "_shapes") {
        for (auto const& [o, row, remainder] : kShapesSites) {
            if (o == offset) return remainder;
        }
        return std::nullopt;
    }
    if (function == "_weak_check") return -1;
    if (function == "_read_datum" || function == "_elem_addr" || function == "_fn_addr"
        || function == "_read_stdoutp") {
        return 0;
    }
    return std::nullopt;
}

// The slot a site names, read back off the patched instruction(s). An x86_64
// site's is derived with the assembler's remainder (`assemblerRemainder`), so the
// derivation does not trust the reader under test.
[[nodiscard]] std::optional<std::uint64_t> slotNamedBy(std::vector<std::uint8_t> const& b, Image const& img,
                                                       GotSite const& s, std::uint64_t pairLdrOffset = 0) {
    auto const fnVa = symbolValue(b, img, s.function);
    if (!fnVa.has_value()) return std::nullopt;
    std::uint64_t const siteVa = *fnVa + s.offset;
    auto const at = fileOffsetOf(img, siteVa);
    if (!at.has_value()) return std::nullopt;
    if (s.formula == RelocFormulaKind::X86_64GotPcRel) {
        // Mach-O: disp = slot + remainder − (P + 4)  ⇒  slot = disp − remainder + P + 4
        auto const remainder = assemblerRemainder(s.function, s.offset);
        if (!remainder.has_value()) return std::nullopt;
        auto const disp = static_cast<std::int32_t>(rd(b, *at, 4));
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(disp) - *remainder
                                          + static_cast<std::int64_t>(siteVa) + 4);
    }
    auto const adrp = static_cast<std::uint32_t>(rd(b, *at, 4));
    auto const ldrAt = fileOffsetOf(img, *fnVa + pairLdrOffset);
    if (!ldrAt.has_value()) return std::nullopt;
    auto const ldr = static_cast<std::uint32_t>(rd(b, *ldrAt, 4));
    std::int64_t pages = ((static_cast<std::int64_t>(adrp >> 5) & 0x7FFFF) << 2) | ((adrp >> 29) & 0x3);
    if ((pages & (std::int64_t{1} << 20)) != 0) pages -= std::int64_t{1} << 21;
    std::uint64_t const page = (siteVa & ~std::uint64_t{0xFFF}) + static_cast<std::uint64_t>(pages * 4096);
    return page + (((ldr >> 10) & 0xFFFu) << 3);
}

// For each arm64 PAGE21 site, the offset of the PAGEOFF12 half naming the same
// symbol in the same function.
[[nodiscard]] std::map<std::size_t, std::uint64_t> pairUp(std::vector<GotSite> const& sites) {
    std::map<std::size_t, std::uint64_t> ldrOf;
    for (std::size_t i = 0; i < sites.size(); ++i) {
        if (sites[i].formula != RelocFormulaKind::Aarch64AdrGotPage) continue;
        for (std::size_t j = 0; j < sites.size(); ++j) {
            if (sites[j].formula == RelocFormulaKind::Aarch64Ld64GotLo12
                && sites[j].function == sites[i].function && sites[j].target == sites[i].target) {
                ldrOf[i] = sites[j].offset;
                break;
            }
        }
    }
    return ldrOf;
}

[[nodiscard]] std::vector<std::uint8_t> linkWithProgram(Members members, bool arm64, TargetSchema const& t,
                                                        ObjectFormatSchema const& image, std::string& errors) {
    std::vector<AssembledModule> mods;
    mods.push_back(programModule(arm64));
    for (auto& m : members.modules) mods.push_back(std::move(m));
    DiagnosticReporter rep;
    // A Darwin image signs itself, and its declared signature identity is a
    // function of the artifact's name.
    auto linked = linker::link(std::span<AssembledModule const>{mods.data(), mods.size()}, t, image, rep,
                               ImageRequest{.artifactFileName = "macho_got_probe"});
    // Every diagnostic, not only errors: a link that returns no image must say why.
    for (auto const& d : rep.all()) errors += d.actual + "\n";
    errors += std::format("(linkedCleanly={} funcs {}/{} bytes={})\n", linked.linkedCleanly,
                          linked.resolvedFuncCount, linked.expectedFuncCount, linked.bytes.size());
    return rep.hasErrors() ? std::vector<std::uint8_t>{} : std::move(linked.bytes);
}

// What the slot of each target must be, in any Mach-O image of these members.
void expectSlotFilledPerTarget(std::vector<std::uint8_t> const& b, Image const& img,
                               std::string const& where, std::string const& target, std::uint64_t slot) {
    auto const at = fileOffsetOf(img, slot);
    ASSERT_TRUE(at.has_value()) << where << ": the slot of " << target << " has no file bytes";
    auto const rebases = rebaseSites(b, img);
    auto const binds = bindSites(b, img);
    bool const rebased = std::find(rebases.begin(), rebases.end(), slot) != rebases.end();
    auto const bound = binds.find(slot);
    if (target == "___stdoutp") {
        EXPECT_EQ(rd(b, *at, 8), 0u) << where << ": a library datum's slot is written by dyld";
        ASSERT_NE(bound, binds.end()) << where << ": the slot of a libSystem datum must be BOUND";
        EXPECT_EQ(bound->second, "___stdoutp") << where;
        EXPECT_FALSE(rebased) << where << ": a bound slot is not also rebased";
    } else {
        auto const want = addressOf(b, img, target);
        ASSERT_TRUE(want.has_value()) << target;
        EXPECT_EQ(rd(b, *at, 8), *want) << where << ": the slot must hold " << target << "'s address";
        EXPECT_TRUE(rebased) << where << ": an MH_PIE image slides it, so the slot must be REBASED";
        EXPECT_EQ(bound, binds.end()) << where;
    }
}

}  // namespace

// ══ A. x86_64's in-place remainder ═══════════════════════════════════════════

TEST(MachoGotRelocations, EveryX86ShapeReadsItsInPlaceRemainderAsTheAddend) {
    auto const t = target("x86_64");
    auto const reader = format("macho64-x86_64-darwin-staticlib");
    ASSERT_TRUE(t && reader);
    auto const members = readMembers(*t, *reader, {dss::test::clangX8664MachoShapesMember()});
    ASSERT_EQ(members.modules.size(), 1u);
    // (offset, row, addend) against the assembler's own remainders, in `_shapes` order.
    std::vector<std::tuple<std::uint64_t, std::string, std::int64_t>> got;
    for (auto const& s : members.sites) got.emplace_back(s.offset, s.kindName, s.addend);
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, kShapesSites) << "every X86_64_RELOC_GOT / GOT_LOAD must read its field's remainder as its "
                                    "addend, and keep the load and the other operands apart";
}

// ══ B. The real members, through the driver's link ═══════════════════════════

TEST(MachoGotRelocations, EveryX86SiteLinksIntoAnExecThroughASlotFilledPerTarget) {
    auto const t = target("x86_64");
    auto const reader = format("macho64-x86_64-darwin-staticlib");
    auto const image = format("macho64-x86_64-darwin-exec");
    ASSERT_TRUE(t && reader && image);
    auto members = readMembers(*t, *reader, {dss::test::clangX8664MachoCRefsMember(),
                                             dss::test::clangX8664MachoShapesMember()});
    auto const sites = members.sites;
    ASSERT_EQ(sites.size(), 12u) << "five c_refs sites + seven shapes sites";
    std::string errors;
    auto const bytes = linkWithProgram(std::move(members), false, *t, *image, errors);
    ASSERT_FALSE(bytes.empty()) << "clang's x86_64 Mach-O GOT sites must LINK. Got:\n" << errors;
    Image const img = readImage(bytes);
    std::map<std::string, std::uint64_t> slotOf;
    for (auto const& s : sites) {
        auto const where = s.function + "+" + std::to_string(s.offset);
        ASSERT_TRUE(assemblerRemainder(s.function, s.offset).has_value())
            << where << ": a site this test knows no assembler remainder for";
        auto const slot = slotNamedBy(bytes, img, s);
        ASSERT_TRUE(slot.has_value()) << where;
        EXPECT_EQ(*slot % 8u, 0u) << where << ": a slot is one aligned pointer";
        auto const [it, fresh] = slotOf.emplace(s.target, *slot);
        EXPECT_EQ(it->second, *slot)
            << where << ": x86-64 keeps the addend on the reference, so every site of " << s.target
            << " reads ONE slot";
        expectSlotFilledPerTarget(bytes, img, where, s.target, *slot);
    }
    EXPECT_EQ(slotOf.size(), 5u) << "one slot each for _ext_datum, _ext_table, _ext_fn, _weak_fn, ___stdoutp";
}

TEST(MachoGotRelocations, EveryArm64PairLinksIntoAnExecAndBothHalvesNameOneSlotFilledPerTarget) {
    auto const t = target("arm64");
    auto const reader = format("macho64-arm64-darwin-staticlib");
    auto const image = format("macho64-arm64-darwin-exec");
    ASSERT_TRUE(t && reader && image);
    auto members = readMembers(*t, *reader, {dss::test::clangArm64MachoCRefsMember()});
    auto const sites = members.sites;
    ASSERT_EQ(sites.size(), 10u) << "five GOT_LOAD pairs";
    auto const pairs = pairUp(sites);
    ASSERT_EQ(pairs.size(), 5u) << "every PAGE21 half has its PAGEOFF12 half";
    std::string errors;
    auto const bytes = linkWithProgram(std::move(members), true, *t, *image, errors);
    ASSERT_FALSE(bytes.empty()) << "clang's arm64 Mach-O GOT pairs must LINK. Got:\n" << errors;
    Image const img = readImage(bytes);
    for (auto const& [i, ldrOffset] : pairs) {
        auto const where = sites[i].function + "+" + std::to_string(sites[i].offset);
        auto const slot = slotNamedBy(bytes, img, sites[i], ldrOffset);
        ASSERT_TRUE(slot.has_value()) << where;
        EXPECT_EQ(*slot % 8u, 0u) << where << ": a 64-bit LDR's slot is 8-aligned";
        expectSlotFilledPerTarget(bytes, img, where, sites[i].target, *slot);
    }
}
