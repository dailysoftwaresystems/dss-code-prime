// Assembled-tier cross-CU merge vs the REAL shipped image formats — c154,
// closing D-LK11-ELF-MACHO-CROSSCU-THUNK-EMISSION + the D-LK-DYN-RODATA-ITEM-RELOC
// producer wall.
//
// The LK11b merge's cross-CU REFERENCE resolution is keyed on the format's
// DECLARED `externCallDispatch` (config, never format identity):
//
//   * `direct-plt` (every shipped format) / undeclared → the reference retargets
//     DIRECTLY to the sibling definition's merged id. Pre-c154 the merge
//     unconditionally minted a GOT-like RODATA thunk slot and retargeted the
//     DIRECT call site into the slot's DATA bytes — the linked exec SIGSEGV'd
//     (witnessed on elf-exec under WSL + pe-exec natively before the fix), the
//     ET_DYN arm failed loud (D-LK-DYN-RODATA-ITEM-RELOC), and the Mach-O exec
//     arm failed loud (__TEXT,__const is not dyld-rebasable).
//
//   * `indirect-slot` → the call site DEREFERENCES a pointer slot, so the merge
//     mints the 8-byte thunk slot + abs64 reloc to the def and retargets the
//     reference to the SLOT. The slot mints as `RelRoConst` via the shared c145
//     `relocBearingGlobalSection` chokepoint (const + reloc-bearing → relro;
//     pre-c154 it minted `Rodata`, the D-LK-DYN-RODATA-ITEM-RELOC loud wall).
//
// Pins:
//   * direct-plt: all four shipped image formats link a 2-CU cross-CU-call pair
//     CLEAN (elf exec / elf dyn / pe exec / macho exec) — no thunk data item, the
//     extern stripped. RED-ON-DISABLE: reverting the dispatch-keyed direct bind
//     re-mints the slot → the elf-exec call-disp pin fails (branch-to-data) and
//     the dyn/macho legs fail loud.
//   * elf-exec byte pin: the caller's `call rel32` disp resolves EXACTLY to the
//     callee's `.text` VA (the anti-branch-to-data regression).
//   * elf-dyn export pin: the merged module carries the surviving definitions'
//     ModuleSymbol rows re-keyed to merged ids — `.dynsym` exports `caller` +
//     `crossfn` (pre-fix the merge dropped `symbols` and the `.so` exported
//     NOTHING). RED-ON-DISABLE: drop the symbols rebuild in mergeModules.
//   * indirect-slot: the thunk slot mints RelRoConst → the ET_DYN image merges
//     it into `.data` and emits EXACTLY ONE R_X86_64_RELATIVE row for it, whose
//     r_offset/r_addend/slot-bytes agree (the c150 prelinked-slot convention),
//     and the indirect call site's disp dereferences THAT slot. RED-ON-DISABLE:
//     re-minting the slot as Rodata fails loud (K_RelocationKindMismatch,
//     D-LK-DYN-RODATA-ITEM-RELOC) → the clean-link assertion fires.
//   * the K_AbsolutePointerRelocMissing gate fires ONLY on the indirect-slot
//     arm (a direct bind needs no pointer slot): an abs64-less target links
//     clean under direct-plt, fails loud under indirect-slot.
//
// End-to-end runtime witnesses (session-level, not ctest): the linked elf-exec
// pair runs → exit 42 under WSL glibc; the pe-exec pair runs → exit 42 natively;
// the dyn pair gcc-links (`gcc host.c -L. -lcross`) + runs → exit 42.

#include "asm/asm.hpp"
#include "core/types/config_path_walk.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace dss;

namespace {

// ── Little-endian readers + ELF parsers (local mirrors of the
//    test_elf_dyn_writer.cpp helpers) ─────────────────────────────

[[nodiscard]] std::uint16_t readU16LE(std::vector<std::uint8_t> const& b,
                                      std::size_t off) {
    return static_cast<std::uint16_t>(b[off])
         | (static_cast<std::uint16_t>(b[off + 1]) << 8);
}
[[nodiscard]] std::uint32_t readU32LE(std::vector<std::uint8_t> const& b,
                                      std::size_t off) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<std::uint32_t>(b[off + i]) << (i * 8);
    return v;
}
[[nodiscard]] std::uint64_t readU64LE(std::vector<std::uint8_t> const& b,
                                      std::size_t off) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<std::uint64_t>(b[off + i]) << (i * 8);
    return v;
}

struct Shdr {
    std::string   name;
    std::uint32_t type = 0;
    std::uint64_t flags = 0, addr = 0, offset = 0, size = 0;
    std::uint32_t link = 0;
    std::uint64_t entsize = 0;
};

[[nodiscard]] std::string readCStr(std::vector<std::uint8_t> const& b,
                                   std::uint64_t off) {
    std::string s;
    for (std::uint64_t p = off; p < b.size() && b[p] != 0; ++p)
        s.push_back(static_cast<char>(b[p]));
    return s;
}

[[nodiscard]] std::vector<Shdr> readSections(std::vector<std::uint8_t> const& b) {
    std::uint64_t const shoff    = readU64LE(b, 40);
    std::uint16_t const shnum    = readU16LE(b, 60);
    std::uint16_t const shstrndx = readU16LE(b, 62);
    std::uint64_t const strOff   = readU64LE(b, shoff + shstrndx * 64ull + 24);
    std::vector<Shdr> out;
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::uint64_t const off = shoff + static_cast<std::uint64_t>(i) * 64;
        Shdr s;
        s.name    = readCStr(b, strOff + readU32LE(b, off + 0));
        s.type    = readU32LE(b, off + 4);
        s.flags   = readU64LE(b, off + 8);
        s.addr    = readU64LE(b, off + 16);
        s.offset  = readU64LE(b, off + 24);
        s.size    = readU64LE(b, off + 32);
        s.link    = readU32LE(b, off + 40);
        s.entsize = readU64LE(b, off + 56);
        out.push_back(std::move(s));
    }
    return out;
}

[[nodiscard]] Shdr const* findSection(std::vector<Shdr> const& secs,
                                      std::string const& name) {
    for (auto const& s : secs) if (s.name == name) return &s;
    return nullptr;
}

struct RelaRow {
    std::uint64_t offset = 0;
    std::uint32_t sym = 0, type = 0;
    std::int64_t  addend = 0;
};
[[nodiscard]] std::vector<RelaRow>
readRelaDyn(std::vector<std::uint8_t> const& b, std::vector<Shdr> const& secs) {
    std::vector<RelaRow> out;
    Shdr const* ra = findSection(secs, ".rela.dyn");
    if (ra == nullptr) return out;
    for (std::uint64_t p = 0; p + 24 <= ra->size; p += 24) {
        std::uint64_t const off  = ra->offset + p;
        std::uint64_t const info = readU64LE(b, off + 8);
        out.push_back(RelaRow{
            readU64LE(b, off + 0),
            static_cast<std::uint32_t>(info >> 32),
            static_cast<std::uint32_t>(info & 0xFFFFFFFFull),
            static_cast<std::int64_t>(readU64LE(b, off + 16))});
    }
    return out;
}

// All `.dynsym` (name, value) pairs, names resolved through `.dynstr`.
[[nodiscard]] std::vector<std::pair<std::string, std::uint64_t>>
readDynSyms(std::vector<std::uint8_t> const& b, std::vector<Shdr> const& secs) {
    std::vector<std::pair<std::string, std::uint64_t>> out;
    Shdr const* ds = findSection(secs, ".dynsym");
    Shdr const* st = findSection(secs, ".dynstr");
    if (ds == nullptr || st == nullptr) return out;
    for (std::uint64_t p = 0; p + 24 <= ds->size; p += 24) {
        std::uint64_t const off = ds->offset + p;
        out.emplace_back(readCStr(b, st->offset + readU32LE(b, off)),
                         readU64LE(b, off + 8));
    }
    return out;
}

struct Loaded {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

[[nodiscard]] Loaded loadShippedPair(std::string const& targetName,
                                     std::string const& formatName) {
    Loaded out;
    auto t = TargetSchema::loadShipped(targetName);
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(" << targetName << ") failed";
        return out;
    }
    out.target = std::move(t).value();
    auto f = ObjectFormatSchema::loadShipped(formatName);
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(" << formatName << ") failed";
        return out;
    }
    out.format = std::move(f).value();
    return out;
}

// The shipped ELF dyn format's JSON text with `externCallDispatch` flipped to
// `indirect-slot` — the indirect-dispatch variant no shipped format declares
// (all ship `direct-plt`), used to pin the thunk-slot arm.
[[nodiscard]] std::shared_ptr<ObjectFormatSchema> loadIndirectSlotDynVariant() {
    auto path = findShippedConfig({"elf64-x86_64-linux-dyn", "object-formats",
                                   ".format.json", "object format",
                                   DiagnosticCode::C_InvalidFormatName});
    if (!path.has_value()) {
        ADD_FAILURE() << "cannot locate shipped elf64-x86_64-linux-dyn.format.json";
        return nullptr;
    }
    std::ifstream in(*path, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string text = std::move(buf).str();
    std::string const from = "\"direct-plt\"";
    std::string const to   = "\"indirect-slot\"";
    auto const pos = text.find(from);
    if (pos == std::string::npos) {
        ADD_FAILURE() << "shipped dyn format no longer declares direct-plt — "
                         "re-point this variant builder";
        return nullptr;
    }
    text.replace(pos, from.size(), to);
    auto f = ObjectFormatSchema::loadFromText(text, "indirect-slot-dyn-variant");
    if (!f.has_value()) {
        ADD_FAILURE() << "indirect-slot dyn variant rejected by the loader";
        for (auto const& d : f.error()) ADD_FAILURE() << "  " << d.message;
        return nullptr;
    }
    return std::move(f).value();
}

// Two-CU cross-CU-call pair: CU#1 `caller` references extern "crossfn" via one
// call-site relocation; CU#2 DEFINES `crossfn` (returns 42). `indirectSite`
// selects the call-site byte shape: the plain direct call (`E8 rel32` /
// `BL imm26`) the shipped direct-plt formats lower, or the deref-the-slot form
// (`FF 15 disp32`) an indirect-slot format lowers. Producer-side addends are 0 —
// the psABI bias lives in the target schema rows (rel32 addendBias=-4).
[[nodiscard]] std::vector<AssembledModule>
makeCrossCuPair(bool arm64, bool withEntry, bool indirectSite = false) {
    std::vector<AssembledModule> mods;
    {
        AssembledModule m;
        m.cuId = CompilationUnitId{1};
        m.expectedFuncCount = 1;
        AssembledFunction caller;
        caller.symbol = SymbolId{1};
        Relocation rel;
        if (arm64) {
            caller.bytes = {0x00, 0x00, 0x00, 0x94,   // bl 0
                            0xC0, 0x03, 0x5F, 0xD6};  // ret
            rel.offset = 0;
            rel.kind   = RelocationKind{1};           // call26
        } else if (indirectSite) {
            caller.bytes = {0xFF, 0x15, 0, 0, 0, 0,   // call qword ptr [rip+disp32]
                            0xC3};                    // ret
            rel.offset = 2;
            rel.kind   = RelocationKind{1};           // rel32 (bias -4 in-schema)
        } else {
            caller.bytes = {0xE8, 0, 0, 0, 0,         // call rel32
                            0xC3};                    // ret
            rel.offset = 1;
            rel.kind   = RelocationKind{1};           // rel32 (bias -4 in-schema)
        }
        rel.target = SymbolId{2};
        rel.addend = 0;
        caller.relocations.push_back(rel);
        m.functions.push_back(std::move(caller));
        ExternImport ext;
        ext.symbol      = SymbolId{2};
        ext.mangledName = "crossfn";
        m.externImports.push_back(std::move(ext));
        m.symbols.push_back(ModuleSymbol{SymbolId{1}, "caller",
                                         SymbolBinding::Global,
                                         SymbolVisibility::Default});
        if (withEntry) m.userEntrySymbol = SymbolId{1};
        mods.push_back(std::move(m));
    }
    {
        AssembledModule m;
        m.cuId = CompilationUnitId{2};
        m.expectedFuncCount = 1;
        AssembledFunction callee;
        callee.symbol = SymbolId{1};
        if (arm64) {
            callee.bytes = {0x40, 0x05, 0x80, 0x52,   // mov w0,#42
                            0xC0, 0x03, 0x5F, 0xD6};  // ret
        } else {
            callee.bytes = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};  // mov eax,42; ret
        }
        m.functions.push_back(std::move(callee));
        m.symbols.push_back(ModuleSymbol{SymbolId{1}, "crossfn",
                                         SymbolBinding::Global,
                                         SymbolVisibility::Default});
        mods.push_back(std::move(m));
    }
    return mods;
}

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& rep,
                                    DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : rep.all()) if (d.code == code) ++n;
    return n;
}

// The callee's recognizable x86_64 body (mov eax,42; ret) — located by byte
// search inside the emitted `.text` to recover its VA without symbol names.
std::vector<std::uint8_t> const kCalleeBodyX86{0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};

[[nodiscard]] std::optional<std::uint64_t>
findBytes(std::vector<std::uint8_t> const& hay, std::uint64_t begin,
          std::uint64_t end, std::vector<std::uint8_t> const& needle) {
    if (needle.empty() || end > hay.size() || begin + needle.size() > end)
        return std::nullopt;
    for (std::uint64_t i = begin; i + needle.size() <= end; ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (hay[i + j] != needle[j]) { match = false; break; }
        }
        if (match) return i;
    }
    return std::nullopt;
}

} // namespace

// ── direct-plt: the four shipped image formats ───────────────────

// Every shipped image format links the 2-CU cross-CU-call pair CLEAN — the
// reference binds directly to the sibling definition (no thunk slot, so no
// reloc-bearing rodata for the dyn/macho belts to reject), and the resolved
// extern is stripped from the import surface.
TEST(CrossCuLinkFormats, DirectPltLinksCleanOnAllFourShippedImageFormats) {
    struct Leg {
        char const* label;
        char const* target;
        char const* format;
        bool        arm64;
        bool        withEntry;
    };
    Leg const legs[] = {
        {"elf-exec",   "x86_64", "elf64-x86_64-linux-exec",    false, true},
        {"elf-dyn",    "x86_64", "elf64-x86_64-linux-dyn",     false, false},
        {"pe-exec",    "x86_64", "pe64-x86_64-windows-exec",   false, true},
        {"macho-exec", "arm64",  "macho64-arm64-darwin-exec",  true,  true},
    };
    for (auto const& leg : legs) {
        SCOPED_TRACE(leg.label);
        auto loaded = loadShippedPair(leg.target, leg.format);
        ASSERT_TRUE(loaded.target && loaded.format);
        auto mods = makeCrossCuPair(leg.arm64, leg.withEntry);
        DiagnosticReporter rep;
        auto image = linker::link(
            std::span<AssembledModule const>{mods.data(), mods.size()},
            *loaded.target, *loaded.format, rep);
        EXPECT_FALSE(rep.hasErrors())
            << "cross-CU direct bind must link clean; first diagnostic: "
            << (rep.all().empty() ? "" : rep.all().front().actual);
        EXPECT_FALSE(image.bytes.empty());
        EXPECT_EQ(std::count(image.externImportNames.begin(),
                             image.externImportNames.end(),
                             std::string{"crossfn"}), 0)
            << "the sibling-resolved extern must be stripped from the imports";
    }
}

// elf-exec byte pin (the anti-branch-to-data regression): the caller's
// `call rel32` disp resolves EXACTLY to the callee's `.text` VA. Pre-c154 the
// merge retargeted the call into the minted RODATA slot — the branch target
// was the slot's DATA bytes (the linked binary SIGSEGV'd, witnessed under
// WSL); with the fix it is the definition itself (the same binary exits 42).
TEST(CrossCuLinkFormats, DirectPltElfExecCallDispTargetsTheDefinition) {
    auto loaded = loadShippedPair("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto mods = makeCrossCuPair(/*arm64=*/false, /*withEntry=*/true);
    DiagnosticReporter rep;
    auto image = linker::link(
        std::span<AssembledModule const>{mods.data(), mods.size()},
        *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(rep.hasErrors());
    ASSERT_FALSE(image.bytes.empty());

    auto const secs = readSections(image.bytes);
    Shdr const* text = findSection(secs, ".text");
    ASSERT_NE(text, nullptr);

    // Locate the callee body inside `.text` (trampoline ++ caller ++ callee).
    auto const calleeOff = findBytes(image.bytes, text->offset,
                                     text->offset + text->size, kCalleeBodyX86);
    ASSERT_TRUE(calleeOff.has_value()) << "callee body not found in .text";
    std::uint64_t const calleeVa = text->addr + (*calleeOff - text->offset);

    // The caller (`E8 disp32; C3`) directly precedes the callee in the merged
    // module order. Decode its disp32 and compute the branch target.
    std::uint64_t const callerOff = *calleeOff - 6;
    ASSERT_EQ(image.bytes[callerOff], 0xE8u) << "caller call opcode not at the "
                                                "expected merged-module position";
    std::int32_t const disp =
        static_cast<std::int32_t>(readU32LE(image.bytes, callerOff + 1));
    std::uint64_t const siteNextVa = text->addr + (callerOff - text->offset) + 5;
    EXPECT_EQ(siteNextVa + static_cast<std::int64_t>(disp), calleeVa)
        << "the cross-CU call must branch to the DEFINITION, not a data slot";
}

// elf-dyn export pin: the merge rebuilds `combined.symbols` from the surviving
// definitions (re-keyed to merged ids), so the ET_DYN image exports BOTH CUs'
// externally-visible functions through `.dynsym` at their real `.text` VAs.
// Pre-fix the merged module carried NO symbols and the `.so` exported nothing.
TEST(CrossCuLinkFormats, DirectPltDynExportsBothCusDefinitions) {
    auto loaded = loadShippedPair("x86_64", "elf64-x86_64-linux-dyn");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto mods = makeCrossCuPair(/*arm64=*/false, /*withEntry=*/false);
    DiagnosticReporter rep;
    auto image = linker::link(
        std::span<AssembledModule const>{mods.data(), mods.size()},
        *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(rep.hasErrors());
    ASSERT_FALSE(image.bytes.empty());

    auto const secs = readSections(image.bytes);
    Shdr const* text = findSection(secs, ".text");
    ASSERT_NE(text, nullptr);
    auto const syms = readDynSyms(image.bytes, secs);
    auto const valueOf =
        [&](std::string const& name) -> std::optional<std::uint64_t> {
        for (auto const& [n, v] : syms) if (n == name) return v;
        return std::nullopt;
    };
    auto const callerVa  = valueOf("caller");
    auto const crossfnVa = valueOf("crossfn");
    ASSERT_TRUE(callerVa.has_value())  << "`caller` must export via .dynsym";
    ASSERT_TRUE(crossfnVa.has_value()) << "`crossfn` must export via .dynsym";
    // Both land inside `.text`; the callee directly follows the 6-byte caller.
    EXPECT_EQ(*callerVa, text->addr);
    EXPECT_EQ(*crossfnVa, text->addr + 6);
}

// ── indirect-slot: the thunk-slot arm ────────────────────────────

// Under `indirect-slot` dispatch the merge mints the thunk slot as RelRoConst;
// the ET_DYN walker merges relro into `.data` and emits EXACTLY ONE
// R_X86_64_RELATIVE row for it (r_offset == the slot VA; r_addend == the slot's
// prelinked base-relative value == the callee's `.text` VA), and the indirect
// call site dereferences THAT slot. RED-ON-DISABLE (the mint's section choice):
// re-minting the slot as `Rodata` trips the D-LK-DYN-RODATA-ITEM-RELOC belt
// (K_RelocationKindMismatch) and the clean-link assertion fails.
TEST(CrossCuLinkFormats, IndirectSlotDynMintsRelRoThunkSlotWithRelativeRow) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = loadIndirectSlotDynVariant();
    ASSERT_NE(format, nullptr);
    auto mods = makeCrossCuPair(/*arm64=*/false, /*withEntry=*/false,
                                /*indirectSite=*/true);
    DiagnosticReporter rep;
    auto image = linker::link(
        std::span<AssembledModule const>{mods.data(), mods.size()},
        **target, *format, rep);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_RelocationKindMismatch), 0u)
        << "a RelRoConst thunk slot must not trip the dyn rodata-reloc belt "
           "(re-minting it as Rodata is the red-on-disable)";
    ASSERT_FALSE(rep.hasErrors())
        << (rep.all().empty() ? "" : rep.all().front().actual);
    ASSERT_FALSE(image.bytes.empty());

    auto const secs = readSections(image.bytes);
    Shdr const* text = findSection(secs, ".text");
    Shdr const* data = findSection(secs, ".data");
    ASSERT_NE(text, nullptr);
    ASSERT_NE(data, nullptr);

    // The callee's `.text` VA (caller is 7 bytes: FF 15 xx xx xx xx C3).
    auto const calleeOff = findBytes(image.bytes, text->offset,
                                     text->offset + text->size, kCalleeBodyX86);
    ASSERT_TRUE(calleeOff.has_value());
    std::uint64_t const calleeVa = text->addr + (*calleeOff - text->offset);
    std::uint64_t const callerOff = *calleeOff - 7;
    ASSERT_EQ(image.bytes[callerOff],     0xFFu);
    ASSERT_EQ(image.bytes[callerOff + 1], 0x15u);

    // Exactly ONE RELATIVE row — the thunk slot's fixup (the pair has no other
    // reloc-bearing data). R_X86_64_RELATIVE = 8, sym index 0.
    std::vector<RelaRow> relative;
    for (auto const& r : readRelaDyn(image.bytes, secs)) {
        if (r.type == 8u) relative.push_back(r);
    }
    ASSERT_EQ(relative.size(), 1u)
        << "the minted thunk slot must emit exactly one RELATIVE row";
    RelaRow const& slotRow = relative[0];
    EXPECT_EQ(slotRow.sym, 0u);
    // The slot lives in the `.data` span (relro merged into `.data` on the
    // image arms — never read-only `.rodata`).
    EXPECT_GE(slotRow.offset, data->addr);
    EXPECT_LE(slotRow.offset + 8, data->addr + data->size);
    // Prelinked-slot convention: slot bytes == r_addend == the callee VA
    // (base-relative — the dyn image is base-0).
    std::uint64_t const slotFileOff = data->offset + (slotRow.offset - data->addr);
    EXPECT_EQ(readU64LE(image.bytes, slotFileOff),
              static_cast<std::uint64_t>(slotRow.addend));
    EXPECT_EQ(static_cast<std::uint64_t>(slotRow.addend), calleeVa)
        << "the thunk slot must hold the sibling DEFINITION's address";
    // The indirect call site dereferences the slot: FF 15 disp32 targets
    // next-insn + disp == the slot VA.
    std::int32_t const disp =
        static_cast<std::int32_t>(readU32LE(image.bytes, callerOff + 2));
    std::uint64_t const siteNextVa = text->addr + (callerOff - text->offset) + 6;
    EXPECT_EQ(siteNextVa + static_cast<std::int64_t>(disp), slotRow.offset)
        << "the indirect cross-CU call must dereference the minted slot";
}

// The abs64-pointer-relocation gate is scoped to the INDIRECT-SLOT arm: a
// target with no `widthBytes==8 && !pcRelative` row cannot host a thunk slot
// (K_AbsolutePointerRelocMissing, loud) — but the SAME modules under the
// direct-plt shipped format bind directly and never need the pointer row.
TEST(CrossCuLinkFormats, Abs64GateFiresOnlyOnTheIndirectSlotArm) {
    // Minimal x86_64-flavored target that declares ONLY the pc-relative rel32 —
    // deliberately NO absolute-64 row.
    auto target = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"x86_64-no-abs64","version":"0.0"},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ],
      "relocations": [
        { "name":"rel32", "kind":1, "pcRelative":true, "addendBias":-4, "widthBytes":4 }
      ]
    })");
    ASSERT_TRUE(target.has_value());
    auto indirectFmt = loadIndirectSlotDynVariant();
    ASSERT_NE(indirectFmt, nullptr);
    {
        auto mods = makeCrossCuPair(/*arm64=*/false, /*withEntry=*/false,
                                    /*indirectSite=*/true);
        DiagnosticReporter rep;
        auto image = linker::link(
            std::span<AssembledModule const>{mods.data(), mods.size()},
            **target, *indirectFmt, rep);
        EXPECT_EQ(countCode(rep, DiagnosticCode::K_AbsolutePointerRelocMissing), 1u)
            << "an indirect-slot thunk without an abs64 row must fail loud";
        EXPECT_TRUE(image.bytes.empty());
    }
    {
        // Same abs64-less target, DIRECT-plt shipped dyn format: binds direct,
        // no slot, links clean.
        auto directFmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-dyn");
        ASSERT_TRUE(directFmt.has_value());
        auto mods = makeCrossCuPair(/*arm64=*/false, /*withEntry=*/false);
        DiagnosticReporter rep;
        auto image = linker::link(
            std::span<AssembledModule const>{mods.data(), mods.size()},
            **target, **directFmt, rep);
        EXPECT_EQ(countCode(rep, DiagnosticCode::K_AbsolutePointerRelocMissing), 0u)
            << "the direct-bind arm needs no pointer slot — the gate must not fire";
        EXPECT_FALSE(rep.hasErrors())
            << (rep.all().empty() ? "" : rep.all().front().actual);
        EXPECT_FALSE(image.bytes.empty());
    }
}
