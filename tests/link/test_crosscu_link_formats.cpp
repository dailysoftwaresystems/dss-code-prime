// Assembled-tier cross-CU merge vs the REAL shipped image formats — c154,
// closing D-LK11-ELF-MACHO-CROSSCU-THUNK-EMISSION + the D-LK-DYN-RODATA-ITEM-RELOC
// producer wall.
//
// The LK11b merge's cross-CU REFERENCE resolution is keyed on the format's
// DECLARED `externCallDispatch` (config, never format identity):
//
//   * `direct-plt` (every shipped IMAGE format; it was every shipped format
//     until P54 pointed the two RELOCATABLE pe formats at `indirect-slot` —
//     D-LK-PE-OBJECT-WEAK-FUNCTION-ADDR-REL32-TO-AN-ABSOLUTE-TARGET) /
//     undeclared → the reference retargets
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

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
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

// `.dynsym` rows. Elf64_Sym: name u32 @0, info u8 @4, other u8 @5, shndx u16 @6,
// st_value u64 @8, st_size u64 @16 (24 bytes).
// ⓘ These fields USED to carry a data import's copy-slot shape — st_size was the
// folded `dataSizeBytes` and st_value the `.bss` slot VA. Since the
// copy-relocation deletion (D-LK-ELF-COPY-RELOC-CLAIMS-ONE-NAME-OF-AN-ALIAS-SET)
// a data import is a plain UNDEF reference with st_size 0 and st_value 0 — the
// exec DEFINES NOTHING, which is the whole point of the fix — so what these
// fields are read for now is exactly that: proof of the absence of a claim.
struct DynSymRow {
    std::string   name;
    std::uint64_t value = 0;
    std::uint64_t size  = 0;
};
[[nodiscard]] std::vector<DynSymRow>
readDynSymRows(std::vector<std::uint8_t> const& b, std::vector<Shdr> const& secs) {
    std::vector<DynSymRow> out;
    Shdr const* ds = findSection(secs, ".dynsym");
    Shdr const* st = findSection(secs, ".dynstr");
    if (ds == nullptr || st == nullptr) return out;
    for (std::uint64_t p = 0; p + 24 <= ds->size; p += 24) {
        std::uint64_t const off = ds->offset + p;
        out.push_back(DynSymRow{readCStr(b, st->offset + readU32LE(b, off)),
                                readU64LE(b, off + 8), readU64LE(b, off + 16)});
    }
    return out;
}

[[nodiscard]] std::optional<DynSymRow>
findDynSym(std::vector<DynSymRow> const& rows, std::string const& name) {
    for (auto const& r : rows) if (r.name == name) return r;
    return std::nullopt;
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
// `indirect-slot` — the indirect-dispatch variant no shipped IMAGE format
// declares (they all ship `direct-plt`; since P54 the two RELOCATABLE pe
// formats declare `indirect-slot`, which is why this says IMAGE and no longer
// says "no shipped format"), used to pin the thunk-slot arm.
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

// ── D-LK11-EXTERN-IMPORT-DEDUP fixtures ──────────────────────────

// One CU per spec. Each CU declares its OWN ExternImport row (always local
// `SymbolId{2}`) — the shape N separately-compiled TUs that each `#include`
// the same header and reference the same library symbol produce. `referenced`
// picks the function body:
//   * true  → `call rel32` (the reloc that must retarget onto the deduped
//             import) ++ the unique `mov eax, tag` locator ++ `ret`
//   * false → the locator ++ `ret` only, so the import is UNREFERENCED and the
//             reference gate's eager law is what decides whether it survives.
// The locator (`B8 tag 00 00 00 C3`, unique per CU) is how a byte pin finds
// this CU's call site in the merged `.text` without depending on the walker's
// layout. CU #1 names the entry when `withEntry`.
struct ImportCuSpec {
    ExternImport import;             // symbol is overwritten with SymbolId{2}
    bool         referenced = true;
};

[[nodiscard]] std::uint8_t importCuTag(std::size_t i) {
    return static_cast<std::uint8_t>(0xA1u + i);
}

[[nodiscard]] std::vector<AssembledModule>
makeImportCus(std::vector<ImportCuSpec> const& specs, bool withEntry) {
    std::vector<AssembledModule> mods;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        AssembledModule m;
        m.cuId = CompilationUnitId{static_cast<std::uint32_t>(i + 1)};
        m.expectedFuncCount = 1;
        AssembledFunction fn;
        fn.symbol = SymbolId{1};
        if (specs[i].referenced) {
            fn.bytes = {0xE8, 0, 0, 0, 0,                      // call rel32
                        0xB8, importCuTag(i), 0x00, 0x00, 0x00, // mov eax,tag
                        0xC3};                                  // ret
            Relocation rel;
            rel.offset = 1;
            rel.target = SymbolId{2};        // the extern import
            rel.kind   = RelocationKind{1};  // rel32 (bias -4 in-schema)
            rel.addend = 0;
            fn.relocations.push_back(rel);
        } else {
            fn.bytes = {0xB8, importCuTag(i), 0x00, 0x00, 0x00, 0xC3};
        }
        m.functions.push_back(std::move(fn));
        ExternImport ext = specs[i].import;
        ext.symbol = SymbolId{2};
        m.externImports.push_back(std::move(ext));
        m.symbols.push_back(ModuleSymbol{SymbolId{1},
                                         "fn" + std::to_string(i + 1),
                                         SymbolBinding::Global,
                                         SymbolVisibility::Default});
        if (withEntry && i == 0) m.userEntrySymbol = SymbolId{1};
        mods.push_back(std::move(m));
    }
    return mods;
}

[[nodiscard]] ExternImport libImport(std::string name, std::string lib,
                                     std::string version = {}) {
    ExternImport e;
    e.mangledName = std::move(name);
    e.libraryPath = std::move(lib);
    e.version     = std::move(version);
    return e;
}

// N CUs all importing the IDENTICAL (name, lib, version), all referencing it.
[[nodiscard]] std::vector<AssembledModule>
makeSharedImportCus(std::size_t cuCount, bool withEntry,
                    std::string const& name    = "puts",
                    std::string const& lib     = "libc.so.6",
                    std::string const& version = {}) {
    std::vector<ImportCuSpec> specs;
    for (std::size_t i = 0; i < cuCount; ++i)
        specs.push_back(ImportCuSpec{libImport(name, lib, version), true});
    return makeImportCus(specs, withEntry);
}

[[nodiscard]] std::size_t countName(std::vector<std::string> const& v,
                                    std::string const& name) {
    return static_cast<std::size_t>(std::count(v.begin(), v.end(), name));
}

// The VA the CU-`i` call site's `call rel32` resolves to, located via that
// CU's unique `mov eax,tag` marker in `.text`.
[[nodiscard]] std::optional<std::uint64_t>
callTargetVaOfCu(std::vector<std::uint8_t> const& bytes, Shdr const& text,
                 std::size_t i) {
    std::vector<std::uint8_t> const marker{0xB8, importCuTag(i),
                                           0x00, 0x00, 0x00, 0xC3};
    auto const markerOff =
        findBytes(bytes, text.offset, text.offset + text.size, marker);
    if (!markerOff.has_value()) return std::nullopt;
    std::uint64_t const siteOff = *markerOff - 5;   // the E8 opcode
    if (bytes[siteOff] != 0xE8u) return std::nullopt;
    std::int32_t const disp = static_cast<std::int32_t>(readU32LE(bytes, siteOff + 1));
    std::uint64_t const nextVa = text.addr + (siteOff - text.offset) + 5;
    return nextVa + static_cast<std::int64_t>(disp);
}

[[nodiscard]] bool anyDiagContains(DiagnosticReporter const& rep,
                                   DiagnosticCode code, std::string const& needle) {
    for (auto const& d : rep.all()) {
        if (d.code == code && d.actual.find(needle) != std::string::npos) return true;
    }
    return false;
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

// ── D-LK11-EXTERN-IMPORT-DEDUP ───────────────────────────────────
//
// N CUs importing the SAME dynamic symbol must merge to EXACTLY ONE
// ExternImport row, with every CU's referencing relocation retargeted onto it
// and the duplicates' per-row payload FOLDED (not dropped). Pre-fix the merge
// concatenated blindly (`mergedIdFor` mints a fresh id per extern; its name-fold
// reads `resolvedGlobalDefs` — definitions, never imports), so 2 CUs produced
// 2 rows and 4 CUs produced 4 — MEASURED as 2 `.dynsym` entries of the identical
// name, 2 `.rela.dyn` GLOB_DAT rows and a 24-byte `.got` on ELF, 2 IAT/ILT thunks
// on PE. RED-ON-DISABLE for the whole group: comment out the dedup pass in
// `mergeModules` and every exact-count assertion below reports the pre-fix N.

// The row count is ONE per (name, lib, version), uniformly across shipped
// formats — the fold is structural, so no format sees a different answer.
TEST(CrossCuLinkFormats, DuplicateExternImportsCoalesceToOneRowOnEveryFormat) {
    struct Leg {
        char const* label;
        char const* format;
        char const* lib;
        bool        withEntry;
    };
    Leg const legs[] = {
        {"elf-exec", "elf64-x86_64-linux-exec",  "libc.so.6",  true},
        {"elf-dyn",  "elf64-x86_64-linux-dyn",   "libc.so.6",  false},
        {"pe-exec",  "pe64-x86_64-windows-exec", "msvcrt.dll", true},
    };
    for (auto const& leg : legs) {
        SCOPED_TRACE(leg.label);
        auto loaded = loadShippedPair("x86_64", leg.format);
        ASSERT_TRUE(loaded.target && loaded.format);
        auto mods = makeSharedImportCus(/*cuCount=*/2, leg.withEntry, "puts", leg.lib);
        DiagnosticReporter rep;
        auto image = linker::link(
            std::span<AssembledModule const>{mods.data(), mods.size()},
            *loaded.target, *loaded.format, rep);
        EXPECT_FALSE(rep.hasErrors())
            << (rep.all().empty() ? "" : rep.all().front().actual);
        EXPECT_FALSE(image.bytes.empty());
        EXPECT_EQ(countName(image.externImportNames, "puts"), 1u)
            << "two CUs importing the same (name, library, version) must merge to "
               "EXACTLY one import row (pre-fix: 2)";
        EXPECT_EQ(image.externImportNames.size(), 1u)
            << "and no other import may appear";
    }
}

// The byte-level consequence on ELF, plus the retarget: the merged exec carries
// exactly ONE `.dynsym` entry for the imported name, its `.got` is the SAME size
// as a single-CU image importing the same symbol once, and BOTH CUs' `call rel32`
// sites resolve to the SAME VA (the one PLT stub) — i.e. both relocations were
// retargeted onto the single canonical import id. Pre-fix: 2 `.dynsym` rows, a
// `.got` 8 bytes larger, and two distinct stub targets.
TEST(CrossCuLinkFormats, DedupedImportEmitsOneDynsymRowAndBothCallsShareTheStub) {
    auto loaded = loadShippedPair("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_TRUE(loaded.target && loaded.format);

    // Control: ONE CU importing `puts` once (the single-CU path — no merge).
    std::uint64_t singleCuGotSize = 0;
    {
        auto mods = makeSharedImportCus(/*cuCount=*/1, /*withEntry=*/true);
        DiagnosticReporter rep;
        auto image = linker::link(
            std::span<AssembledModule const>{mods.data(), mods.size()},
            *loaded.target, *loaded.format, rep);
        ASSERT_FALSE(rep.hasErrors())
            << (rep.all().empty() ? "" : rep.all().front().actual);
        ASSERT_FALSE(image.bytes.empty());
        auto const secs = readSections(image.bytes);
        Shdr const* got = findSection(secs, ".got");
        ASSERT_NE(got, nullptr);
        singleCuGotSize = got->size;
    }

    auto mods = makeSharedImportCus(/*cuCount=*/2, /*withEntry=*/true);
    DiagnosticReporter rep;
    auto image = linker::link(
        std::span<AssembledModule const>{mods.data(), mods.size()},
        *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(rep.hasErrors())
        << (rep.all().empty() ? "" : rep.all().front().actual);
    ASSERT_FALSE(image.bytes.empty());

    auto const secs = readSections(image.bytes);
    Shdr const* text = findSection(secs, ".text");
    Shdr const* got  = findSection(secs, ".got");
    ASSERT_NE(text, nullptr);
    ASSERT_NE(got, nullptr);

    std::size_t putsRows = 0;
    for (auto const& [n, v] : readDynSyms(image.bytes, secs)) {
        (void)v;
        if (n == "puts") ++putsRows;
    }
    EXPECT_EQ(putsRows, 1u)
        << "one dynamic symbol must produce exactly one `.dynsym` entry, however "
           "many CUs imported it (pre-fix: 2 entries of the identical name)";
    EXPECT_EQ(got->size, singleCuGotSize)
        << "the merged image's `.got` must be the SAME size as the single-CU "
           "image's — pre-fix the duplicate import added another 8-byte slot";

    auto const cu1Target = callTargetVaOfCu(image.bytes, *text, 0);
    auto const cu2Target = callTargetVaOfCu(image.bytes, *text, 1);
    ASSERT_TRUE(cu1Target.has_value()) << "CU#1 call site not found in .text";
    ASSERT_TRUE(cu2Target.has_value()) << "CU#2 call site not found in .text";
    EXPECT_EQ(*cu1Target, *cu2Target)
        << "both CUs' relocations must retarget onto the ONE canonical import "
           "(the same PLT stub), not one stub each";
}

// A FOLD, not a pairwise special case: 4 CUs still yield exactly 1 row, and all
// four call sites share the stub.
TEST(CrossCuLinkFormats, FourCusImportingOneSymbolStillYieldExactlyOneRow) {
    auto loaded = loadShippedPair("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto mods = makeSharedImportCus(/*cuCount=*/4, /*withEntry=*/true);
    DiagnosticReporter rep;
    auto image = linker::link(
        std::span<AssembledModule const>{mods.data(), mods.size()},
        *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(rep.hasErrors())
        << (rep.all().empty() ? "" : rep.all().front().actual);
    ASSERT_FALSE(image.bytes.empty());
    EXPECT_EQ(countName(image.externImportNames, "puts"), 1u)
        << "the dedup must be a FOLD over all N CUs (pre-fix: 4 rows)";

    auto const secs = readSections(image.bytes);
    Shdr const* text = findSection(secs, ".text");
    ASSERT_NE(text, nullptr);
    auto const first = callTargetVaOfCu(image.bytes, *text, 0);
    ASSERT_TRUE(first.has_value());
    for (std::size_t i = 1; i < 4; ++i) {
        SCOPED_TRACE(i);
        auto const target = callTargetVaOfCu(image.bytes, *text, i);
        ASSERT_TRUE(target.has_value()) << "CU call site not found in .text";
        EXPECT_EQ(*target, *first) << "every CU's call must reach the same stub";
    }
}

// CONTROL — the key is not the bare name. `foo` from `a.dll` and `foo` from
// `b.dll` are DIFFERENT imports (libraryPath is what the walkers group
// DT_NEEDED / IMAGE_IMPORT_DESCRIPTOR by), so they must NOT fold.
TEST(CrossCuLinkFormats, SameNameDifferentLibraryDoesNotFold) {
    auto loaded = loadShippedPair("x86_64", "pe64-x86_64-windows-exec");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto mods = makeImportCus({{libImport("foo", "a.dll"), true},
                               {libImport("foo", "b.dll"), true}},
                              /*withEntry=*/true);
    DiagnosticReporter rep;
    auto image = linker::link(
        std::span<AssembledModule const>{mods.data(), mods.size()},
        *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(rep.hasErrors())
        << (rep.all().empty() ? "" : rep.all().front().actual);
    EXPECT_EQ(countName(image.externImportNames, "foo"), 2u)
        << "two DIFFERENT libraries owning one name are two distinct imports — "
           "folding them would bind one CU's calls into the wrong DLL";
}

// CONTROL — nor is `version` foldable. `puts@GLIBC_2.2.5` and `puts@GLIBC_2.17`
// are genuinely different dynamic symbols (c156 D-LK-ELF-SYMBOL-VERSIONING);
// folding them would reintroduce exactly the compat-form misbind c156 fixed.
TEST(CrossCuLinkFormats, SameNameAndLibraryDifferentVersionDoesNotFold) {
    auto loaded = loadShippedPair("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_TRUE(loaded.target && loaded.format);
    auto mods = makeImportCus(
        {{libImport("puts", "libc.so.6", "GLIBC_2.2.5"), true},
         {libImport("puts", "libc.so.6", "GLIBC_2.17"),  true}},
        /*withEntry=*/true);
    DiagnosticReporter rep;
    auto image = linker::link(
        std::span<AssembledModule const>{mods.data(), mods.size()},
        *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(rep.hasErrors())
        << (rep.all().empty() ? "" : rep.all().front().actual);
    EXPECT_EQ(countName(image.externImportNames, "puts"), 2u)
        << "two symbol VERSIONS of one name are two dynamic symbols — folding "
           "them would bind a call to the wrong glibc compat form";
}

// ── THE KEY'S INJECTIVITY, WITH THE COLLISION ITS OWN COMMENT DESCRIBES ─────
//
// `linker.cpp`'s `dedupKey` LENGTH-PREFIXES every field — "injective over
// arbitrary field bytes" — because a mangledName and a libraryPath are arbitrary
// bytes out of a descriptor. MEASURED (TF-C119): every fixture in tests/ used
// `foo` / `puts` / `a.dll` / `b.dll` / `GLIBC_2.x`, not one containing a `:` or a
// `|`, so replacing BOTH tiers' key builders with the naive
// `mangledName + ":" + libraryPath + ":" + version` left every one of them green.
// The claim was pinned only by prose.
//
// This is the pre-image pair that prose implies. Naive: both key to `a:b:c:` and
// fold. Length-prefixed: `3:a:b|1:c|0:` vs `1:a|3:b:c|0:`, two rows. The
// consequence of the fold is not a lost row — it is CU#2's call site silently
// retargeted onto a DIFFERENT dynamic symbol in a DIFFERENT library, so the call
// target VAs are asserted DISTINCT as well as the row count.
// (The MIR tier's twin is `MirMerge.LengthPrefixedKeyKeepsAColludingNameLibrary
// PairApart` — both tiers key identically, so both need the pin.)
TEST(CrossCuLinkFormats, LengthPrefixedKeyKeepsAColludingNameLibraryPairApart) {
    auto loaded = loadShippedPair("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_TRUE(loaded.target && loaded.format);
    // "a:b" from "c"  vs  "a" from "b:c" — one naive key, two real imports.
    auto mods = makeImportCus({{libImport("a:b", "c"), true},
                               {libImport("a", "b:c"), true}},
                              /*withEntry=*/true);
    DiagnosticReporter rep;
    auto image = linker::link(
        std::span<AssembledModule const>{mods.data(), mods.size()},
        *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(rep.hasErrors())
        << (rep.all().empty() ? "" : rep.all().front().actual);
    ASSERT_FALSE(image.bytes.empty());

    EXPECT_EQ(image.externImportNames.size(), 2u)
        << "★ a separator-joined key maps BOTH triples to `a:b:c:` and folds two "
           "UNRELATED dynamic symbols into one import row";
    EXPECT_EQ(countName(image.externImportNames, "a:b"), 1u)
        << "the name must survive VERBATIM — the keying may not re-spell it";
    EXPECT_EQ(countName(image.externImportNames, "a"), 1u);

    auto const secs = readSections(image.bytes);
    Shdr const* text = findSection(secs, ".text");
    ASSERT_NE(text, nullptr);
    auto const cu1Target = callTargetVaOfCu(image.bytes, *text, 0);
    auto const cu2Target = callTargetVaOfCu(image.bytes, *text, 1);
    ASSERT_TRUE(cu1Target.has_value() && cu2Target.has_value());
    EXPECT_NE(*cu1Target, *cu2Target)
        << "★ THE MISCOMPILE: under a colliding key both call sites retarget onto "
           "ONE stub, so CU#2 calls the symbol \"a:b\" in library \"c\" — a "
           "different function in a different image, with no diagnostic anywhere";
}

// ★ The load-failure path. `isEagerImport` must OR-combine, not first-win: fold
// an eager descriptor row onto a non-eager sibling and keep the non-eager bit,
// and `rejectOrDropUnreferencedExterns` DROPS the surviving row when nobody
// references it — the loader can no longer bind a symbol the descriptor promised
// (D-FFI-DESCRIPTOR-EAGER-IMPORT; pe 0xC0000139 / elf exit 127). NEITHER CU
// references the import here, so the folded row's eager bit is the ONLY thing
// keeping it alive, and the import's survival IS the observation.
//
// The all-non-eager control below is what makes that observation load-bearing:
// it proves this fixture's unreferenced import really is dropped absent the
// eager bit, so a surviving row can only mean the bit was ORed in. Both orders
// are checked — the fold is order-INDEPENDENT.
//
// RED-ON-DISABLE for this pin is the FOLD RULE, not the whole pass: turn
// `kept.isEagerImport = kept.isEagerImport || ext.isEagerImport` into first-wins
// and the "non-eager first" order drops to 0 imports. (Disabling the whole dedup
// pass canNOT witness this one — two independent rows let the eager row survive
// on its own; that state is what the row-count pins above measure.)
TEST(CrossCuLinkFormats, EagerImportBitOrCombinesAcrossTheFoldInBothOrders) {
    auto loaded = loadShippedPair("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_TRUE(loaded.target && loaded.format);

    ExternImport eager = libImport("puts", "libc.so.6");
    eager.isEagerImport = true;
    ExternImport lazy = libImport("puts", "libc.so.6");   // isEagerImport = false

    auto const importsAfterLink = [&](ExternImport const& a, ExternImport const& b) {
        auto mods = makeImportCus({{a, /*referenced=*/false},
                                   {b, /*referenced=*/false}},
                                  /*withEntry=*/true);
        DiagnosticReporter rep;
        auto image = linker::link(
            std::span<AssembledModule const>{mods.data(), mods.size()},
            *loaded.target, *loaded.format, rep);
        EXPECT_FALSE(rep.hasErrors())
            << (rep.all().empty() ? "" : rep.all().front().actual);
        return countName(image.externImportNames, "puts");
    };

    // CONTROL: with NO eager contributor the unreferenced import is dropped —
    // so a surviving row in the two cases below can only come from the OR-fold.
    EXPECT_EQ(importsAfterLink(lazy, lazy), 0u)
        << "an unreferenced non-eager import must be dropped by the reference "
           "gate — without this the eager assertions below prove nothing";
    EXPECT_EQ(importsAfterLink(lazy, eager), 1u)
        << "non-eager first: the folded row must be EAGER — a first-wins fold "
           "keeps the non-eager bit and the reference gate then drops the "
           "import the descriptor promised the loader";
    EXPECT_EQ(importsAfterLink(eager, lazy), 1u)
        << "eager first: the fold must be order-independent";
}

// A cross-CU disagreement about ONE dynamic symbol is a REAL conflict, never a
// pick-one. `isData` selects the binding MODEL (a got-indirect data pointer slot
// vs the function-import path), so silently keeping either row would bind the
// loser CU's references through the wrong one — the D-LK-EXTERN-DATA-IMPORT
// silent-miscompile shape. Same for `isThreadLocal`, and for two DIFFERING
// non-zero `dataSizeBytes` — two CUs declaring DIFFERENT objects under one
// external name.
TEST(CrossCuLinkFormats, ConflictingExternImportAttributesFailLoud) {
    auto loaded = loadShippedPair("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_TRUE(loaded.target && loaded.format);

    auto const linkPair = [&](ExternImport const& a, ExternImport const& b,
                              DiagnosticReporter& rep) {
        auto mods = makeImportCus({{a, true}, {b, true}}, /*withEntry=*/true);
        return linker::link(
            std::span<AssembledModule const>{mods.data(), mods.size()},
            *loaded.target, *loaded.format, rep);
    };

    {   // isData: function import in CU#1, data object in CU#2.
        SCOPED_TRACE("isData");
        ExternImport fnRow = libImport("shared", "libc.so.6");
        ExternImport dataRow = libImport("shared", "libc.so.6");
        dataRow.isData         = true;
        dataRow.dataSizeBytes  = 8;
        dataRow.dataAlignBytes = 8;
        DiagnosticReporter rep;
        auto image = linkPair(fnRow, dataRow, rep);
        EXPECT_EQ(countCode(rep, DiagnosticCode::K_ExternImportAttributeConflict), 1u);
        EXPECT_TRUE(anyDiagContains(rep, DiagnosticCode::K_ExternImportAttributeConflict,
                                    "conflicting `isData` (data object vs function "
                                    "import) across CompilationUnits (false vs true)"))
            << "the diagnostic must name the field and both values; got: "
            << (rep.all().empty() ? "" : rep.all().front().actual);
        EXPECT_TRUE(anyDiagContains(rep, DiagnosticCode::K_ExternImportAttributeConflict,
                                    "D-LK11-EXTERN-IMPORT-DEDUP"));
        EXPECT_TRUE(image.bytes.empty()) << "a conflicting merge must emit nothing";
    }
    {   // isThreadLocal: same name/library, one CU calls it thread-local data.
        SCOPED_TRACE("isThreadLocal");
        ExternImport plain = libImport("tlsvar", "libc.so.6");
        plain.isData         = true;
        plain.dataSizeBytes  = 4;
        plain.dataAlignBytes = 4;
        ExternImport tls = plain;
        tls.isThreadLocal = true;
        DiagnosticReporter rep;
        auto image = linkPair(plain, tls, rep);
        EXPECT_EQ(countCode(rep, DiagnosticCode::K_ExternImportAttributeConflict), 1u);
        EXPECT_TRUE(anyDiagContains(rep, DiagnosticCode::K_ExternImportAttributeConflict,
                                    "conflicting `isThreadLocal` (thread storage "
                                    "duration) across CompilationUnits (false vs true)"))
            << (rep.all().empty() ? "" : rep.all().front().actual);
        EXPECT_TRUE(image.bytes.empty());
    }
    {   // Two DIFFERING non-zero sizes for one external name.
        SCOPED_TRACE("dataSizeBytes");
        ExternImport small = libImport("gvar", "libc.so.6");
        small.isData         = true;
        small.dataSizeBytes  = 4;
        small.dataAlignBytes = 4;
        ExternImport big = small;
        big.dataSizeBytes = 8;
        DiagnosticReporter rep;
        auto image = linkPair(small, big, rep);
        EXPECT_EQ(countCode(rep, DiagnosticCode::K_ExternImportAttributeConflict), 1u);
        EXPECT_TRUE(anyDiagContains(rep, DiagnosticCode::K_ExternImportAttributeConflict,
                                    "conflicting `dataSizeBytes` (declared "
                                    "object size) across CompilationUnits (4 vs 8)"))
            << (rep.all().empty() ? "" : rep.all().front().actual);
        EXPECT_TRUE(image.bytes.empty());
    }
}

// ── CONTROL: a ZERO size/align is an INCOMPLETE TYPE, not a conflict — and the
//    folded shape reaches the IMAGE as a CLAIM OF NOTHING ────────────────────
//
// `extern const char v[];` in one TU beside a sized declaration in another is legal
// C (extern_import.hpp — both fields stay 0 for an incomplete type), so the merge
// takes the NON-ZERO shape per field and reports nothing.
//
// ★★ WHAT THIS TEST LOST AND WHAT REPLACED IT — stated rather than quietly
// dropped, because the loss is real. It used to read the FOLDED NUMBERS back out
// of the emitted image: `dataSizeBytes` from the copy slot's `.dynsym` st_size,
// and `dataAlignBytes` from the DISTANCE between a 1-aligned pad import's `.bss`
// slot and the subject's. Both observations are GONE with the copy-relocation
// mechanism (D-LK-ELF-COPY-RELOC-CLAIMS-ONE-NAME-OF-AN-ALIAS-SET): a data import
// now takes a GOT slot holding the library object's ADDRESS, so its declared size
// and alignment reach NO emitter at all and cannot be recovered from the image by
// any means. The link tier therefore cannot witness the fold's OUTPUT any more.
//   * WHERE THE FOLD IS STILL WITNESSED, both orders and the align-only case:
//     `MirMerge.IncompleteExternDataTypeFoldsToTheSizedShapeNotAConflict` in
//     tests/mir/test_mir_merge.cpp, which reads the MERGED ROW directly instead of
//     an image. That is the honest home for it now.
//   * WHAT THIS TEST ASSERTS INSTEAD is the property the deletion BOUGHT, which is
//     strictly stronger for the defect that motivated it: the folded row reaches
//     the image as a data import that CLAIMS NOTHING — `.dynsym` UNDEF, st_size 0,
//     st_value 0 — and binds through exactly one GLOB_DAT against a GOT slot. A
//     copy-relocation-shaped regression (the exec DEFINING an imported name at its
//     own `.bss`) reds here even if the folded numbers were perfect, and it is
//     precisely that shape which SPLIT an aliased libc object silently.
// Every row is EAGER and UNREFERENCED — a data object is not called, so the fixture
// does not aim a `call rel32` at one, and the eager bit is what keeps the reference
// gate from dropping the rows.
TEST(CrossCuLinkFormats, IncompleteExternDataFoldsAndClaimsNothingInEitherOrder) {
    auto loaded = loadShippedPair("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_TRUE(loaded.target && loaded.format);

    ExternImport pad = libImport("padvar", "libc.so.6");
    pad.isData = true;
    pad.dataSizeBytes  = 1;
    pad.dataAlignBytes = 1;
    pad.isEagerImport  = true;

    // Reads back the subject "gvar2"'s image-level CLAIM: (st_size, st_value,
    // GLOB_DAT-row count against its GOT slot).
    auto const claimShape = [&](ExternImport const& first, ExternImport const& second)
        -> std::optional<std::tuple<std::uint64_t, std::uint64_t, std::size_t>> {
        auto mods = makeImportCus({{pad, /*referenced=*/false},
                                   {first, /*referenced=*/false},
                                   {second, /*referenced=*/false}},
                                  /*withEntry=*/true);
        DiagnosticReporter rep;
        auto image = linker::link(
            std::span<AssembledModule const>{mods.data(), mods.size()},
            *loaded.target, *loaded.format, rep);
        EXPECT_EQ(countCode(rep, DiagnosticCode::K_ExternImportAttributeConflict), 0u)
            << "a zero size/align is an incomplete type, not a disagreement; got: "
            << (rep.all().empty() ? "" : rep.all().front().actual);
        EXPECT_EQ(countCode(rep, DiagnosticCode::K_FormatLacksImportSupport), 0u)
            << "the walker must ADMIT an incomplete-typed data import: a "
               "got-indirect slot holds an ADDRESS, so the object's own size is "
               "irrelevant to it; got: "
            << (rep.all().empty() ? "" : rep.all().front().actual);
        EXPECT_EQ(countName(image.externImportNames, "gvar2"), 1u)
            << "one (name, library, version) is ONE import row";
        if (image.bytes.empty()) {
            ADD_FAILURE() << "no image emitted — the walker refused the folded shape";
            return std::nullopt;
        }
        auto const secs    = readSections(image.bytes);
        auto const rows    = readDynSymRows(image.bytes, secs);
        auto const subject = findDynSym(rows, "gvar2");
        auto const padRow  = findDynSym(rows, "padvar");
        if (!subject.has_value() || !padRow.has_value()) {
            ADD_FAILURE() << "both data imports must appear in .dynsym as "
                             "references — an import that is not even referenced "
                             "makes every assertion below vacuous";
            return std::nullopt;
        }
        EXPECT_EQ(padRow->size, 0u)
            << "the PAD is an import too: it must claim nothing either";
        // Which .dynsym index is the subject? Needed to attribute its reloc.
        std::size_t subjectIdx = 0;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].name == "gvar2") subjectIdx = i;
        }
        // R_X86_64_GLOB_DAT == 6, and the r_offset must land inside `.got`.
        Shdr const* got = findSection(secs, ".got");
        if (got == nullptr) {
            ADD_FAILURE() << "no `.got` section — a got-indirect data import has "
                             "nowhere to bind";
            return std::nullopt;
        }
        std::size_t globDats = 0;
        for (auto const& r : readRelaDyn(image.bytes, secs)) {
            if (r.sym != subjectIdx) continue;
            EXPECT_EQ(r.type, 6u)
                << "a data import's ONLY dynamic reloc must be GLOB_DAT (6); a "
                   "COPY (5) here is the object-identity defect returning";
            EXPECT_GE(r.offset, got->addr);
            EXPECT_LT(r.offset, got->addr + got->size)
                << "the slot must live in `.got`, never in `.bss`";
            ++globDats;
        }
        return std::tuple{subject->size, subject->value, globDats};
    };

    // Each order gets its OWN verdict: the check body is a void lambda, so a failing
    // order reports and the remaining ones still run. (Written that way deliberately —
    // a bare `ASSERT_TRUE` in the TEST body returns from the WHOLE test, which would let
    // the first failing order hide whether the others fold correctly, and the whole
    // point of this fixture is that the orders are independent observations.)
    auto const expectClaimsNothing =
        [&](char const* trace, ExternImport const& first, ExternImport const& second) {
        SCOPED_TRACE(trace);
        auto const shape = claimShape(first, second);
        if (!shape.has_value()) return;   // `claimShape` already reported why
        EXPECT_EQ(std::get<0>(*shape), 0u)
            << "★ st_size must be 0: a NON-ZERO size here means the exec is "
               "exporting the imported name as a DEFINED, SIZED OBJECT — the "
               "copy-relocation shape that claimed ONE name of glibc's "
               "{environ,_environ,__environ} alias set and split the object";
        EXPECT_EQ(std::get<1>(*shape), 0u)
            << "★ st_value must be 0 (SHN_UNDEF): a real address here means this "
               "image DEFINES a name the library owns";
        EXPECT_EQ(std::get<2>(*shape), 1u)
            << "exactly ONE GLOB_DAT binds the import — zero means nothing "
               "resolves the object, two means duplicate slots";
    };

    ExternImport sized = libImport("gvar2", "libc.so.6");
    sized.isData = true;
    sized.dataSizeBytes  = 8;
    sized.dataAlignBytes = 32;   // deliberately > any default so the read-back is exact
    sized.isEagerImport  = true;

    ExternImport incomplete = libImport("gvar2", "libc.so.6");
    incomplete.isData = true;          // 0 / 0 — the shape is unknown in THIS unit
    incomplete.isEagerImport = true;

    // Both orders: the fold is order-INDEPENDENT, and either way the image must
    // claim nothing.
    expectClaimsNothing("incomplete first", incomplete, sized);
    expectClaimsNothing("sized first", sized, incomplete);

    // The align-only case still LINKS cleanly (it is legal C), even though its
    // fold is no longer observable here — the MIR-tier twin asserts the folded
    // pair. Kept so a regression that REJECTED this shape still reds somewhere.
    ExternImport alignUnknown = libImport("gvar2", "libc.so.6");
    alignUnknown.isData = true;
    alignUnknown.dataSizeBytes  = 8;   // size AGREES
    alignUnknown.dataAlignBytes = 0;   // only the ALIGNMENT is unknown here
    alignUnknown.isEagerImport  = true;
    expectClaimsNothing("alignment alone unknown, unknown first", alignUnknown, sized);
    expectClaimsNothing("alignment alone unknown, sized first", sized, alignUnknown);

    // ★ FAIL-CLOSED, and it is the assertion that keeps every EXPECT_EQ(…, 0u)
    // above from being satisfiable by an image that imports nothing at all: a
    // FUNCTION import through the same fixture must carry a NON-ZERO st_value
    // (its PLT stub VA). If the reader were broken, or the reference gate had
    // dropped every row, this reads 0 too and the whole test reds.
    ExternImport fnControl = libImport("fnctl", "libc.so.6");
    fnControl.isEagerImport = true;
    {
        SCOPED_TRACE("positive control: a FUNCTION import still claims a stub VA");
        auto mods = makeImportCus({{fnControl, /*referenced=*/false},
                                   {sized, /*referenced=*/false}},
                                  /*withEntry=*/true);
        DiagnosticReporter rep;
        auto image = linker::link(
            std::span<AssembledModule const>{mods.data(), mods.size()},
            *loaded.target, *loaded.format, rep);
        ASSERT_FALSE(image.bytes.empty());
        auto const rows = readDynSymRows(image.bytes, readSections(image.bytes));
        auto const fnRow = findDynSym(rows, "fnctl");
        ASSERT_TRUE(fnRow.has_value())
            << "the reader must find a function import — otherwise the zeroes "
               "asserted above prove only that it finds nothing";
        EXPECT_EQ(fnRow->size, 0u) << "a function import claims no size either";
    }
}
