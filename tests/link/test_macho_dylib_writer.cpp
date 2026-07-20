// Mach-O MH_DYLIB (.dylib) writer tests -- c153, the D-LK3-3 anchor
// (the Mach-O mirror of the c150 ELF .so + c152 PE .dll suites).
//
// Pins the dynamic-library contract the macOS loader consumes
// (`dlopen("./libdss.dylib")` + `dlsym(handle, "dss_add")` on Apple
// Silicon):
//   * mach_header: filetype = MH_DYLIB (6), flags = MH_NOUNDEFS |
//     MH_DYLDLINK | MH_TWOLEVEL | MH_NO_REEXPORTED_DYLIBS (NO
//     MH_PIE), LC_ID_DYLIB carrying image.installName, NO LC_MAIN /
//     LC_LOAD_DYLINKER / __PAGEZERO (base-0 image dyld slides).
//   * EXPORTS: externally-visible defined functions + data globals
//     findable by WALKING the emitted LC_DYLD_INFO_ONLY export trie
//     with dyld's own algorithm (the strongest pin -- the exact
//     lookup dlsym performs; mirrors the ELF suite's SysV-hash
//     walk), including the strict-prefix name pair a flat
//     full-name-edge trie would break on. Local symbols stay out.
//   * REBASE completeness: an internal fn-ptr slot (relro fn-ptr
//     table -> __data fold) gets a REBASE opcode at its exact
//     (segment, offset) and the slot bytes carry the link-time VA
//     (red-on-disable for the rebase emission).
//   * D-LK-IMAGE-DATA-SLOT-EXTERN-ADDR (Mach-O half): a data slot
//     whose reloc targets an EXTERN emits a SYMBOL-BASED dyld BIND
//     opcode (slot zeroed, NOT rebased, never the baked
//     got-slot/stub VA) -- data extern AND function extern flavors,
//     on the DYLIB arm and on the EXEC arm (the c117-era latent-bug
//     surface), plus the non-zero-addend SET_ADDEND_SLEB ride.
//   * export-set integrity: a ModuleSymbol row naming an EXTERN
//     IMPORT fails loud (a symbolVa-only lookup would export the
//     local stub/got cell -- the definition-table classification is
//     the c150 ELF mirror).
//   * codesign: the dylib CodeDirectory's execSegFlags is 0 (NOT
//     CS_EXECSEG_MAIN_BINARY -- the exec keeps 1, pinned by
//     test_macho_codesign.cpp).
//   * policy: isImageFlavor() TRUE, allowsUndefinedImports() FALSE
//     (two-level namespace -- every bind names a dylib ordinal),
//     outputExtension ".dylib", tdata/tbss rejected by absence
//     (D-LK3-DYLIB-TLS-MODEL).
//   * validate() shape rules: entry cluster / entryPoint /
//     dylinkerPath / non-zero pageZeroSize / missing installName /
//     installName-on-exec / text VA != segmentPageSize /
//     useChainedFixups all rejected loud.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/macho.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "macho_test_support.hpp"
#include "program/program.hpp"
#include "program/target_spec.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace dss;

namespace {

using dss::macho::test::findLoadCommand;
using dss::macho::test::findSection;
using dss::macho::test::findSegment;
using dss::macho::test::readU32LE;
using dss::macho::test::readU64LE;

[[nodiscard]] std::uint64_t readU64BE(std::span<std::uint8_t const> b,
                                      std::size_t off) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | static_cast<std::uint64_t>(b[off + i]);
    return v;
}

// LC_* constants used by the pins (<mach-o/loader.h>).
constexpr std::uint32_t kLcIdDylib       = 0x0Du;
constexpr std::uint32_t kLcLoadDylinker  = 0x0Eu;
constexpr std::uint32_t kLcMain          = 0x80000028u;
constexpr std::uint32_t kLcDyldInfoOnly  = 0x80000022u;
constexpr std::uint32_t kLcCodeSignature = 0x1Du;
constexpr std::uint32_t kLcDysymtab      = 0x0Bu;

struct Loaded {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

[[nodiscard]] Loaded loadShippedMachoImage(std::string_view formatName) {
    Loaded out;
    auto t = TargetSchema::loadShipped("arm64");
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(arm64) failed";
        for (auto const& d : t.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.target = std::move(t).value();
    }
    auto f = ObjectFormatSchema::loadShipped(formatName);
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(" << formatName << ") failed";
        for (auto const& d : f.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.format = std::move(f).value();
    }
    return out;
}

[[nodiscard]] Loaded loadShippedDylib() {
    return loadShippedMachoImage("macho64-arm64-darwin-dylib");
}

// The exec sibling -- the F2 fold pins the extern-slot bind on the
// EXEC arm too (the D-LK-IMAGE-DATA-SLOT-EXTERN-ADDR latent-bug
// surface was the exec since c117).
[[nodiscard]] Loaded loadShippedExec() {
    return loadShippedMachoImage("macho64-arm64-darwin-exec");
}

[[nodiscard]] bool sawDiagnosticContaining(DiagnosticReporter const& rep,
                                           std::string_view needle) {
    for (auto const& d : rep.all()) {
        if (d.actual.find(needle) != std::string::npos) return true;
    }
    return false;
}

// arm64 `RET` (C0 03 5F D6) -- a real single-instruction body.
[[nodiscard]] std::vector<std::uint8_t> arm64Ret() {
    return {0xC0, 0x03, 0x5F, 0xD6};
}

// ── Module builders (the c150/c152 mirror shapes, Mach-O-mangled:
//    ModuleSymbol names arrive PRE-MANGLED with the leading `_`) ──

// Exported fn `_dss_add` + exported int global `_dss_global`
// (.data {7,0,0,0}) + a LOCAL (static) helper that must NOT export.
// No externs -- the zero-import dylib witness shape.
[[nodiscard]] AssembledModule makeExportModule() {
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = arm64Ret();
    mod.functions.push_back(std::move(fn));
    AssembledFunction loc;
    loc.symbol = SymbolId{2};
    loc.bytes  = arm64Ret();
    mod.functions.push_back(std::move(loc));
    AssembledData d;
    d.symbol    = SymbolId{3};
    d.section   = DataSectionKind::Data;
    d.bytes     = {7, 0, 0, 0};
    d.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(d));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "_dss_add",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{2}, "_hidden_helper",
                                       SymbolBinding::Local,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{3}, "_dss_global",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    return mod;
}

// Two exported fns whose names are a STRICT PREFIX pair
// (`_dss_add` prefixes `_dss_add_two`) -- the shape a flat
// full-name-edge "trie" cannot represent (dyld's greedy first-match
// walk would follow the `_dss_add` edge and lose `_dss_add_two`), so
// finding BOTH proves real radix prefix compression.
[[nodiscard]] AssembledModule makePrefixPairModule() {
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction a;
    a.symbol = SymbolId{1};
    a.bytes  = arm64Ret();
    mod.functions.push_back(std::move(a));
    AssembledFunction b;
    b.symbol = SymbolId{2};
    b.bytes  = arm64Ret();
    mod.functions.push_back(std::move(b));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "_dss_add",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{2}, "_dss_add_two",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    return mod;
}

// Exported fn `_dss_dispatch` + a RELRO fn-ptr table {&fn} -- the
// internal-absolute-slot shape: the table's abs64 (kind 4 on the
// arm64 schemas) reloc targets the function; the dylib must REBASE
// the slot (dyld adds the load slide).
[[nodiscard]] AssembledModule makeFnPtrTableModule() {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = arm64Ret();
    mod.functions.push_back(std::move(fn));
    AssembledData tab;
    tab.symbol    = SymbolId{5};
    tab.section   = DataSectionKind::RelRoConst;
    tab.bytes     = std::vector<std::uint8_t>(8, 0);
    tab.alignment = Alignment::of<8>();
    Relocation rel;
    rel.offset = 0;
    rel.target = SymbolId{1};
    rel.kind   = RelocationKind{4};   // abs64 (ARM64_RELOC_UNSIGNED)
    rel.addend = 0;
    tab.relocations.push_back(rel);
    mod.dataItems.push_back(std::move(tab));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "_dss_dispatch",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{5}, "_tab",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    return mod;
}

// One exported fn + one EXTERN import (function or data flavor) + a
// mutable data slot whose abs64 reloc targets the extern -- the
// D-LK-IMAGE-DATA-SLOT-EXTERN-ADDR shape (`FILE **pp = &stdout;` /
// `int (*fp)() = puts;`). `addend` rides the reloc (the F5 fold's
// `&stdout + 8` shape exercising SET_ADDEND_SLEB).
[[nodiscard]] AssembledModule makeExternSlotModule(bool externIsData,
                                                   std::int64_t addend = 0) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = arm64Ret();
    mod.functions.push_back(std::move(fn));
    ExternImport imp;
    imp.symbol      = SymbolId{99};
    imp.mangledName = externIsData ? "___stdoutp" : "_puts";
    imp.libraryPath = "/usr/lib/libSystem.B.dylib";
    imp.isData      = externIsData;
    mod.externImports.push_back(std::move(imp));
    AssembledData slot;
    slot.symbol    = SymbolId{5};
    slot.section   = DataSectionKind::Data;
    slot.bytes     = std::vector<std::uint8_t>(8, 0);
    slot.alignment = Alignment::of<8>();
    Relocation rel;
    rel.offset = 0;
    rel.target = SymbolId{99};
    rel.kind   = RelocationKind{4};   // abs64
    rel.addend = addend;
    slot.relocations.push_back(rel);
    mod.dataItems.push_back(std::move(slot));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "_dss_add",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{5}, "_dss_slot",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    return mod;
}

// ── LC_DYLD_INFO_ONLY field readers ──────────────────────────────
// dyld_info_command layout: cmd(0) cmdsize(4) rebase_off(8)
// rebase_size(12) bind_off(16) bind_size(20) weak_bind(24,28)
// lazy_bind(32,36) export_off(40) export_size(44).

struct DyldInfoView {
    std::uint32_t rebaseOff = 0, rebaseSize = 0;
    std::uint32_t bindOff = 0, bindSize = 0;
    std::uint32_t exportOff = 0, exportSize = 0;
    bool          found = false;
};

[[nodiscard]] DyldInfoView readDyldInfo(std::vector<std::uint8_t> const& b) {
    DyldInfoView out;
    auto const lc = findLoadCommand(b, kLcDyldInfoOnly);
    if (!lc) return out;
    out.rebaseOff  = readU32LE(b, *lc + 8);
    out.rebaseSize = readU32LE(b, *lc + 12);
    out.bindOff    = readU32LE(b, *lc + 16);
    out.bindSize   = readU32LE(b, *lc + 20);
    out.exportOff  = readU32LE(b, *lc + 40);
    out.exportSize = readU32LE(b, *lc + 44);
    out.found      = true;
    return out;
}

// ── The dyld export-trie walk (MachOLoaded::trieWalk's algorithm,
//    re-implemented HERE so the test consumes the emitted bytes the
//    way dlsym does): at each node, read the terminal payload if the
//    search string is exhausted; otherwise follow the ONE child edge
//    that prefixes the remaining string. Returns (flags, address) on
//    a hit. ──

struct TrieHit {
    std::uint64_t flags   = 0;
    std::uint64_t address = 0;
};

[[nodiscard]] std::optional<std::uint64_t>
readUleb(std::span<std::uint8_t const> b, std::size_t& p) {
    std::uint64_t v = 0;
    int shift = 0;
    for (;;) {
        if (p >= b.size() || shift > 63) return std::nullopt;
        std::uint8_t const byte = b[p++];
        v |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) break;
        shift += 7;
    }
    return v;
}

[[nodiscard]] std::optional<TrieHit>
trieWalk(std::span<std::uint8_t const> trie, std::string_view symbol) {
    std::size_t p = 0;
    int guard = 0;
    for (;;) {
        if (++guard > 128 || p >= trie.size()) return std::nullopt;
        auto const terminalSize = readUleb(trie, p);
        if (!terminalSize) return std::nullopt;
        if (symbol.empty()) {
            if (*terminalSize == 0) return std::nullopt;   // interior only
            TrieHit hit;
            auto const flags = readUleb(trie, p);
            auto const addr  = readUleb(trie, p);
            if (!flags || !addr) return std::nullopt;
            hit.flags   = *flags;
            hit.address = *addr;
            return hit;
        }
        std::size_t children = p + static_cast<std::size_t>(*terminalSize);
        if (children >= trie.size()) return std::nullopt;
        std::uint8_t childCount = trie[children++];
        p = children;
        std::optional<std::uint64_t> nextNode;
        for (std::uint8_t c = 0; c < childCount; ++c) {
            // Read the NUL-terminated edge.
            std::string edge;
            while (p < trie.size() && trie[p] != 0) {
                edge.push_back(static_cast<char>(trie[p++]));
            }
            if (p >= trie.size()) return std::nullopt;
            ++p;   // NUL
            auto const off = readUleb(trie, p);
            if (!off) return std::nullopt;
            if (!nextNode && symbol.starts_with(edge)) {
                symbol.remove_prefix(edge.size());
                nextNode = *off;
                // dyld follows the FIRST matching edge and never
                // backtracks -- keep scanning only to consume the
                // remaining sibling records (we jump by offset, so
                // just break).
                break;
            }
        }
        if (!nextNode) return std::nullopt;
        p = static_cast<std::size_t>(*nextNode);
    }
}

// ── Bind / rebase opcode-stream decoders (<mach-o/loader.h>) ──────

struct BindRecord {
    std::uint32_t ordinal = 0;
    std::string   symbol;
    std::int64_t  addend  = 0;
    std::uint8_t  segIdx  = 0;
    std::uint64_t segOff  = 0;
};

[[nodiscard]] std::optional<std::int64_t>
readSleb(std::span<std::uint8_t const> b, std::size_t& p) {
    std::int64_t v = 0;
    int shift = 0;
    std::uint8_t byte = 0;
    do {
        if (p >= b.size() || shift > 63) return std::nullopt;
        byte = b[p++];
        v |= static_cast<std::int64_t>(
                 static_cast<std::uint64_t>(byte & 0x7F) << shift);
        shift += 7;
    } while ((byte & 0x80) != 0);
    if (shift < 64 && (byte & 0x40) != 0) {
        v |= -(std::int64_t{1} << shift);
    }
    return v;
}

[[nodiscard]] std::vector<BindRecord>
decodeBindStream(std::span<std::uint8_t const> stream) {
    std::vector<BindRecord> out;
    BindRecord cur;
    std::size_t p = 0;
    while (p < stream.size()) {
        std::uint8_t const byte = stream[p++];
        std::uint8_t const op  = byte & 0xF0u;
        std::uint8_t const imm = byte & 0x0Fu;
        if (byte == 0x00) break;                       // BIND_OPCODE_DONE
        switch (op) {
            case 0x10:                                  // SET_DYLIB_ORDINAL_IMM
                cur.ordinal = imm;
                break;
            case 0x20: {                                // SET_DYLIB_ORDINAL_ULEB
                auto const v = readUleb(stream, p);
                if (!v) return out;
                cur.ordinal = static_cast<std::uint32_t>(*v);
                break;
            }
            case 0x40: {                                // SET_SYMBOL
                cur.symbol.clear();
                while (p < stream.size() && stream[p] != 0) {
                    cur.symbol.push_back(static_cast<char>(stream[p++]));
                }
                if (p < stream.size()) ++p;             // NUL
                break;
            }
            case 0x50:                                  // SET_TYPE_IMM
                break;
            case 0x60: {                                // SET_ADDEND_SLEB
                auto const v = readSleb(stream, p);
                if (!v) return out;
                cur.addend = *v;
                break;
            }
            case 0x70: {                                // SET_SEGMENT_AND_OFFSET
                cur.segIdx = imm;
                auto const v = readUleb(stream, p);
                if (!v) return out;
                cur.segOff = *v;
                break;
            }
            case 0x90:                                  // DO_BIND
                out.push_back(cur);
                break;
            default:
                ADD_FAILURE() << "unexpected bind opcode 0x" << std::hex
                              << static_cast<int>(byte);
                return out;
        }
    }
    return out;
}

struct RebaseRecord {
    std::uint8_t  segIdx = 0;
    std::uint64_t segOff = 0;
};

[[nodiscard]] std::vector<RebaseRecord>
decodeRebaseStream(std::span<std::uint8_t const> stream) {
    std::vector<RebaseRecord> out;
    RebaseRecord cur;
    std::size_t p = 0;
    while (p < stream.size()) {
        std::uint8_t const byte = stream[p++];
        std::uint8_t const op  = byte & 0xF0u;
        std::uint8_t const imm = byte & 0x0Fu;
        if (byte == 0x00) break;                       // REBASE_OPCODE_DONE
        switch (op) {
            case 0x10:                                  // SET_TYPE_IMM
                break;
            case 0x20: {                                // SET_SEGMENT_AND_OFFSET
                cur.segIdx = imm;
                auto const v = readUleb(stream, p);
                if (!v) return out;
                cur.segOff = *v;
                break;
            }
            case 0x50:                                  // DO_REBASE_IMM_TIMES
                for (std::uint8_t i = 0; i < imm; ++i) {
                    out.push_back(cur);
                    cur.segOff += 8;                    // pointer stride
                }
                break;
            default:
                ADD_FAILURE() << "unexpected rebase opcode 0x" << std::hex
                              << static_cast<int>(byte);
                return out;
        }
    }
    return out;
}

// Encode + basic sanity in one step (any Mach-O image flavor).
[[nodiscard]] std::vector<std::uint8_t>
encodeDylib(AssembledModule const& mod, Loaded const& loaded) {
    DiagnosticReporter rep;
    auto bytes =
        dss::macho::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    EXPECT_FALSE(bytes.empty());
    return bytes;
}

} // namespace

// ── (0) Shipped JSON loads + policy predicates ───────────────────

TEST(MachoDylibFormatJson, ShippedFileLoadsCleanly) {
    auto loaded = loadShippedDylib();
    ASSERT_TRUE(loaded.format);
    EXPECT_EQ(loaded.format->kind(), ObjectFormatKind::MachO);
    EXPECT_EQ(loaded.format->name(), "macho64-arm64-darwin-dylib");
    EXPECT_EQ(loaded.format->macho().filetype, MachOObjectType::Dylib);
    EXPECT_EQ(loaded.format->macho().cputype, 0x0100000Cu);
    EXPECT_EQ(loaded.format->machoImage().pageZeroSize, 0u);
    EXPECT_TRUE(loaded.format->machoImage().dylinkerPath.empty());
    EXPECT_EQ(loaded.format->machoImage().installName,
              "@rpath/libdss.dylib");
}

TEST(MachoDylibFormatPolicy, ImageFlavorTrueUndefinedImportsFalse) {
    auto loaded = loadShippedDylib();
    ASSERT_TRUE(loaded.format);
    // A dylib IS a load-time-bound image (dyld maps + rebases +
    // binds it) ...
    EXPECT_TRUE(loaded.format->isImageFlavor());
    // ... and its two-level namespace binds every import against a
    // NAMED dylib ordinal, so a library-less referenced extern has
    // nothing to resolve it later -- reject at build time (unlike
    // the ELF .so's flat global scope).
    EXPECT_FALSE(loaded.format->allowsUndefinedImports());
    // lib profile served; tdata/tbss NOT accepted
    // (D-LK3-DYLIB-TLS-MODEL rejects by absence).
    EXPECT_TRUE(loaded.format->acceptsDataSection(DataSectionKind::Data));
    EXPECT_TRUE(
        loaded.format->acceptsDataSection(DataSectionKind::RelRoConst));
    EXPECT_FALSE(loaded.format->acceptsDataSection(DataSectionKind::Tdata));
    EXPECT_FALSE(loaded.format->acceptsDataSection(DataSectionKind::Tbss));
}

TEST(MachoDylibFormatPolicy, OutputExtensionIsDylib) {
    auto loaded = loadShippedDylib();
    ASSERT_TRUE(loaded.format);
    TargetSpec const spec{"arm64", "macho64-arm64-darwin-dylib"};
    EXPECT_EQ(spec.outputExtension(*loaded.format), ".dylib");
}

// ── (1) Header pins: the MH_DYLIB shape ──────────────────────────

TEST(MachoDylibWriter, HeaderPinsDylibShape) {
    auto loaded = loadShippedDylib();
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const bytes = encodeDylib(makeExportModule(), loaded);
    ASSERT_GE(bytes.size(), 32u);

    // mach_header_64: magic / cputype / filetype = MH_DYLIB (6) /
    // flags = MH_NOUNDEFS|MH_DYLDLINK|MH_TWOLEVEL|
    // MH_NO_REEXPORTED_DYLIBS = 0x100085 (NO MH_PIE -- an
    // executable-only flag).
    EXPECT_EQ(readU32LE(bytes, 0), 0xFEEDFACFu);
    EXPECT_EQ(readU32LE(bytes, 4), 0x0100000Cu);
    EXPECT_EQ(readU32LE(bytes, 12), 6u);
    EXPECT_EQ(readU32LE(bytes, 24), 0x100085u);

    // LC_ID_DYLIB present, name offset 24, the configured install
    // name at that offset.
    auto const idLc = findLoadCommand(bytes, kLcIdDylib);
    ASSERT_TRUE(idLc.has_value());
    EXPECT_EQ(readU32LE(bytes, *idLc + 8), 24u);   // lc_str offset
    std::string const name(
        reinterpret_cast<char const*>(&bytes[*idLc + 24]));
    EXPECT_EQ(name, "@rpath/libdss.dylib");

    // NO LC_MAIN, NO LC_LOAD_DYLINKER, NO __PAGEZERO.
    EXPECT_FALSE(findLoadCommand(bytes, kLcMain).has_value());
    EXPECT_FALSE(findLoadCommand(bytes, kLcLoadDylinker).has_value());
    EXPECT_FALSE(findSegment(bytes, "__PAGEZERO").has_value());

    // __TEXT is the FIRST segment at vmaddr 0 (base-0 image), and
    // __text sits at VA 0x4000 = one segment page (header page 0).
    auto const textSeg = findSegment(bytes, "__TEXT");
    ASSERT_TRUE(textSeg.has_value());
    EXPECT_EQ(readU64LE(bytes, *textSeg + 24), 0u);        // vmaddr
    auto const textSec = findSection(bytes, "__TEXT", "__text");
    ASSERT_TRUE(textSec.has_value());
    EXPECT_EQ(readU64LE(bytes, *textSec + 32), 0x4000u);   // addr

    // Zero externs: no __DATA_CONST segment ships (its only content
    // would be an empty __got). LC_DYSYMTAB still present (legacy
    // dyld-info path).
    EXPECT_FALSE(findSegment(bytes, "__DATA_CONST").has_value());
    EXPECT_TRUE(findLoadCommand(bytes, kLcDysymtab).has_value());
}

// ── (2) The export trie -- walked with dyld's own algorithm ──────

TEST(MachoDylibWriter, ExportTrieFindsFunctionAndDataAndOmitsLocal) {
    auto loaded = loadShippedDylib();
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const bytes = encodeDylib(makeExportModule(), loaded);

    auto const di = readDyldInfo(bytes);
    ASSERT_TRUE(di.found);
    ASSERT_GT(di.exportSize, 0u);
    ASSERT_LE(static_cast<std::size_t>(di.exportOff) + di.exportSize,
              bytes.size());
    std::span<std::uint8_t const> const trie{bytes.data() + di.exportOff,
                                             di.exportSize};

    // The exported FUNCTION: address = image offset of _dss_add =
    // __text VA (mach header at VA 0) + funcTextStart 0 = 0x4000.
    auto const fn = trieWalk(trie, "_dss_add");
    ASSERT_TRUE(fn.has_value());
    EXPECT_EQ(fn->flags, 0u);   // EXPORT_SYMBOL_FLAGS_KIND_REGULAR
    EXPECT_EQ(fn->address, 0x4000u);

    // The exported DATA global: address == the __data section addr
    // (the global is its first item), read from the emitted
    // section_64 so the pin tracks the real layout.
    auto const dataSec = findSection(bytes, "__DATA", "__data");
    ASSERT_TRUE(dataSec.has_value());
    std::uint64_t const dataVa = readU64LE(bytes, *dataSec + 32);
    auto const dg = trieWalk(trie, "_dss_global");
    ASSERT_TRUE(dg.has_value());
    EXPECT_EQ(dg->address, dataVa);

    // The LOCAL (static) helper must NOT be exported.
    EXPECT_FALSE(trieWalk(trie, "_hidden_helper").has_value());
    // Nor a name that never existed.
    EXPECT_FALSE(trieWalk(trie, "_nope").has_value());
}

TEST(MachoDylibWriter, ExportTriePrefixPairBothFindable) {
    // `_dss_add` is a STRICT PREFIX of `_dss_add_two`: a flat
    // full-name-edge root would swallow the longer lookup (dyld
    // follows the first prefixing edge and never backtracks). Both
    // resolving proves the radix split.
    auto loaded = loadShippedDylib();
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const bytes = encodeDylib(makePrefixPairModule(), loaded);

    auto const di = readDyldInfo(bytes);
    ASSERT_TRUE(di.found);
    ASSERT_GT(di.exportSize, 0u);
    std::span<std::uint8_t const> const trie{bytes.data() + di.exportOff,
                                             di.exportSize};
    auto const a = trieWalk(trie, "_dss_add");
    auto const b = trieWalk(trie, "_dss_add_two");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(a->address, 0x4000u);        // fn[0] at __text start
    EXPECT_EQ(b->address, 0x4004u);        // fn[1] right after (4-byte RET)
    EXPECT_FALSE(trieWalk(trie, "_dss_ad").has_value());
    EXPECT_FALSE(trieWalk(trie, "_dss_add_t").has_value());
}

// ── (3) Rebase completeness: internal fn-ptr slot ────────────────

TEST(MachoDylibWriter, InternalFnPtrSlotGetsRebaseOpcode) {
    auto loaded = loadShippedDylib();
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const bytes = encodeDylib(makeFnPtrTableModule(), loaded);

    auto const di = readDyldInfo(bytes);
    ASSERT_TRUE(di.found);
    // RED-ON-DISABLE: dropping the rebase emission zeroes
    // rebase_size and this assert fires.
    ASSERT_GT(di.rebaseSize, 0u);
    auto const rebases = decodeRebaseStream(
        std::span<std::uint8_t const>{bytes.data() + di.rebaseOff,
                                      di.rebaseSize});
    ASSERT_EQ(rebases.size(), 1u);

    // The rebase targets the fn-ptr slot: __DATA segment (index 1 --
    // __TEXT=0, no __PAGEZERO / __DATA_CONST on this zero-extern
    // dylib), offset 0 (the table is the segment's first bytes).
    auto const dataSeg = findSegment(bytes, "__DATA");
    ASSERT_TRUE(dataSeg.has_value());
    std::uint64_t const dataSegVmaddr = readU64LE(bytes, *dataSeg + 24);
    std::uint64_t const dataSegFileOff = readU64LE(bytes, *dataSeg + 40);
    EXPECT_EQ(rebases[0].segIdx, 1u);
    std::uint64_t const slotVa = dataSegVmaddr + rebases[0].segOff;

    // The slot BYTES carry the fn's link-time VA (0x4000); dyld adds
    // the slide at load. Read them through the segment file mapping.
    std::uint64_t const slotFileOff =
        dataSegFileOff + (slotVa - dataSegVmaddr);
    ASSERT_LE(slotFileOff + 8, bytes.size());
    EXPECT_EQ(readU64LE(bytes, static_cast<std::size_t>(slotFileOff)),
              0x4000u);
}

// ── (4) D-LK-IMAGE-DATA-SLOT-EXTERN-ADDR: symbol-based BIND ───────

namespace {
// The D-LK-IMAGE-DATA-SLOT-EXTERN-ADDR pin, parameterized over the
// IMAGE FLAVOR (F2 fold: the exec arm was the latent-bug surface
// since c117, so it gets its own pins — expected __DATA segment
// index 3 there: __PAGEZERO 0 / __TEXT 1 / __DATA_CONST 2 / __DATA
// 3; the dylib has no __PAGEZERO, so 2) and the reloc ADDEND (F5
// fold: a non-zero addend must ride a SET_ADDEND_SLEB opcode).
void expectExternSlotBind(Loaded const&    loaded,
                          std::uint8_t     expectedDataSegIdx,
                          bool             externIsData,
                          std::string_view symName,
                          std::int64_t     addend = 0) {
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const bytes =
        encodeDylib(makeExternSlotModule(externIsData, addend), loaded);

    auto const di = readDyldInfo(bytes);
    ASSERT_TRUE(di.found);

    auto const dataSeg = findSegment(bytes, "__DATA");
    ASSERT_TRUE(dataSeg.has_value());
    std::uint64_t const dataSegVmaddr  = readU64LE(bytes, *dataSeg + 24);
    std::uint64_t const dataSegFileOff = readU64LE(bytes, *dataSeg + 40);
    auto const dataSec = findSection(bytes, "__DATA", "__data");
    ASSERT_TRUE(dataSec.has_value());
    std::uint64_t const slotVa = readU64LE(bytes, *dataSec + 32);

    ASSERT_GT(di.bindSize, 0u);
    std::span<std::uint8_t const> const bindStream{
        bytes.data() + di.bindOff, di.bindSize};
    auto const binds = decodeBindStream(bindStream);
    // Two DO_BINDs: the extern's __got slot (__DATA_CONST) + the
    // extern-addr data slot (__DATA). Find the data-slot one.
    bool sawSlotBind = false;
    for (auto const& b : binds) {
        if (b.segIdx != expectedDataSegIdx) continue;
        EXPECT_EQ(dataSegVmaddr + b.segOff, slotVa);
        EXPECT_EQ(b.symbol, symName);
        EXPECT_EQ(b.ordinal, 1u);   // libSystem = LC_LOAD_DYLIB #1
        EXPECT_EQ(b.addend, addend);
        sawSlotBind = true;
    }
    // RED-ON-DISABLE: without the c153 fold there is NO bind row
    // against the data slot (the walker used to bake the got-slot /
    // stub VA + a rebase).
    EXPECT_TRUE(sawSlotBind);

    // F5: a non-zero addend must arrive via a raw SET_ADDEND_SLEB
    // (0x60) opcode byte in the stream — the decoder's addend match
    // above proves the VALUE; this proves the OPCODE carried it.
    if (addend != 0) {
        bool sawSetAddend = false;
        for (auto const byte : bindStream) {
            if (byte == 0x60u) { sawSetAddend = true; break; }
        }
        EXPECT_TRUE(sawSetAddend)
            << "non-zero addend must ride BIND_OPCODE_SET_ADDEND_SLEB";
    }

    // The slot bytes are ZEROED (dyld writes resolved + addend).
    std::uint64_t const slotFileOff =
        dataSegFileOff + (slotVa - dataSegVmaddr);
    ASSERT_LE(slotFileOff + 8, bytes.size());
    EXPECT_EQ(readU64LE(bytes, static_cast<std::size_t>(slotFileOff)), 0u);

    // And the slot is NOT in the rebase stream (bind owns it).
    if (di.rebaseSize > 0) {
        auto const rebases = decodeRebaseStream(
            std::span<std::uint8_t const>{bytes.data() + di.rebaseOff,
                                          di.rebaseSize});
        for (auto const& r : rebases) {
            EXPECT_FALSE(r.segIdx == expectedDataSegIdx
                         && dataSegVmaddr + r.segOff == slotVa)
                << "extern-addr slot must not be rebased AND bound";
        }
    }
}
} // namespace

TEST(MachoDylibWriter, ExternDataAddrSlotEmitsSymbolBasedBindNotBake) {
    expectExternSlotBind(loadShippedDylib(), /*expectedDataSegIdx=*/2,
                         /*externIsData=*/true, "___stdoutp");
}

TEST(MachoDylibWriter, ExternFunctionAddrSlotEmitsSymbolBasedBindNotBake) {
    // `int (*fp)() = puts;` -- the function-pointer identity half
    // (C11 6.5.9): the bind resolves the REAL `puts`, never the
    // image-local stub.
    expectExternSlotBind(loadShippedDylib(), /*expectedDataSegIdx=*/2,
                         /*externIsData=*/false, "_puts");
}

TEST(MachoDylibWriter, ExternAddrSlotNonZeroAddendRidesSetAddendSleb) {
    // F5: `&stdout + 8` -- the bind carries the reloc addend through
    // SET_ADDEND_SLEB so dyld stores `resolved + 8`; the slot stays
    // zeroed on disk.
    expectExternSlotBind(loadShippedDylib(), /*expectedDataSegIdx=*/2,
                         /*externIsData=*/true, "___stdoutp",
                         /*addend=*/8);
}

TEST(MachoExecWriterExternSlot, ExternDataAddrSlotBindsOnExecArmToo) {
    // F2: the EXEC arm carried the latent bake since c117 -- the
    // c153 fold applies to it identically (__DATA is segment 3
    // there: __PAGEZERO/__TEXT/__DATA_CONST precede it).
    expectExternSlotBind(loadShippedExec(), /*expectedDataSegIdx=*/3,
                         /*externIsData=*/true, "___stdoutp");
}

TEST(MachoExecWriterExternSlot, ExternFunctionAddrSlotBindsOnExecArmToo) {
    expectExternSlotBind(loadShippedExec(), /*expectedDataSegIdx=*/3,
                         /*externIsData=*/false, "_puts");
}

// ── (4b) D-LK-IMAGE-DATA-SLOT-EXTERN-ADDR: the EXEC arm END-TO-END,
//    from REAL SOURCE through the shipped stdio.json ────────────────
//
// The (4) pins above prove the FOLD given a HAND-BUILT extern-targeted
// data-item reloc. This pin proves the source->reloc chain the hand-
// built module assumes: `FILE **pp = &stdout;` compiled through the
// FULL pipeline (Program::compileFiles -- the CLI's own path) with the
// shipped <stdio.h>, whose macho macro rewrites `stdout` to the real
// `___stdoutp` data export, actually EMITS the symbol-based bind for
// the `pp` __data slot (not a slot-VA bake). Host-independent (byte
// inspection), so it witnesses the Mach-O exec arm on ANY host -- not
// only the macos-latest run of the extern_data_addr_static_init corpus.
namespace {
namespace efs = std::filesystem;

// Each LC_SEGMENT_64's vmaddr in load-command (index) order -- a bind
// opcode addresses a slot by (segment INDEX, offset), so the bind's
// segIdx indexes THIS vector (index 0 = __PAGEZERO on the exec arm).
[[nodiscard]] std::vector<std::uint64_t>
collectSegmentVmaddrs(std::vector<std::uint8_t> const& b) {
    std::vector<std::uint64_t> out;
    if (b.size() < 32) return out;
    std::uint32_t const ncmds = readU32LE(b, 16);
    std::size_t off = 32;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        if (off + 8 > b.size()) break;
        std::uint32_t const cmd     = readU32LE(b, off);
        std::uint32_t const cmdsize = readU32LE(b, off + 4);
        if (cmd == 0x19u && off + 32 <= b.size()) {   // LC_SEGMENT_64
            out.push_back(readU64LE(b, off + 24));    // segment_command_64.vmaddr
        }
        if (cmdsize == 0) break;
        off += cmdsize;
    }
    return out;
}

[[nodiscard]] std::vector<std::uint8_t> readFileBytes(efs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}
} // namespace

TEST(MachoExecSourceExternSlot,
     StdoutAddrStaticInitEmitsSymbolBasedBindThroughShippedStdio) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;

    // Compile `FILE **pp = &stdout;` -> arm64 macho exec via the FULL
    // driver pipeline (the same path the CLI runs). The shipped
    // <stdio.h> macho macro rewrites `stdout` -> `__stdoutp` (a real
    // extern DATA export); the file-scope `&stdout` initializer must
    // emit the c153 symbol-based bind, NOT a slot-VA bake.
    ScratchDir scratch{Location::InsideRepo, "c158_macho_extern_addr"};
    auto const src = scratch.path() / "extern_addr.c";
    {
        std::ofstream f(src);
        f << "#include <stdio.h>\n"
             "FILE **pp = &stdout;\n"
             "int main(void) { return pp == &stdout ? 42 : 1; }\n";
    }
    scratch.useAsCwd();
    auto const outDir = scratch.path() / "out";

    Program            prog;
    DiagnosticReporter rep;
    prog.setOutputDir(outDir);
    int const rc = prog.compileFiles({src.generic_string()}, "c-subset",
                                     {"arm64:macho64-arm64-darwin-exec"}, rep);
    std::ostringstream diag;
    for (auto const& d : rep.all()) diag << "\n  " << d.actual;
    ASSERT_EQ(rc, 0) << "compile failed:" << diag.str();
    ASSERT_EQ(rep.errorCount(), 0u) << diag.str();

    // Single-target build => <outputDir>/<stem>.
    auto const artifact = outDir / "extern_addr";
    ASSERT_TRUE(efs::exists(artifact))
        << "no macho exec at " << artifact.generic_string();
    auto const bytes = readFileBytes(artifact);
    ASSERT_FALSE(bytes.empty());

    auto const di = readDyldInfo(bytes);
    ASSERT_TRUE(di.found);
    ASSERT_GT(di.bindSize, 0u);

    auto const segVmaddrs = collectSegmentVmaddrs(bytes);
    auto const dataSegOff = findSegment(bytes, "__DATA");
    ASSERT_TRUE(dataSegOff.has_value());
    std::uint64_t const dataSegVmaddr  = readU64LE(bytes, *dataSegOff + 24);
    std::uint64_t const dataSegFileOff = readU64LE(bytes, *dataSegOff + 40);
    auto const dataSec = findSection(bytes, "__DATA", "__data");
    ASSERT_TRUE(dataSec.has_value());
    std::uint64_t const slotVa = readU64LE(bytes, *dataSec + 32);  // section_64.addr

    // Decode the bind stream; find the bind whose (segIdx, segOff)
    // resolves to the `pp` __data slot. It MUST target ___stdoutp.
    std::span<std::uint8_t const> const bindStream{
        bytes.data() + di.bindOff, di.bindSize};
    auto const binds = decodeBindStream(bindStream);
    bool sawSlotBind = false;
    for (auto const& b : binds) {
        if (b.segIdx >= segVmaddrs.size()) continue;
        if (segVmaddrs[b.segIdx] + b.segOff != slotVa) continue;
        EXPECT_EQ(b.symbol, "___stdoutp")
            << "the &stdout static-init slot must bind the real data export";
        EXPECT_EQ(b.ordinal, 1u);   // libSystem = LC_LOAD_DYLIB #1
        sawSlotBind = true;
    }
    // RED-ON-DISABLE: without the c153 fold the walker bakes the got-
    // slot VA + a rebase, so there is NO symbol-based bind at this slot.
    EXPECT_TRUE(sawSlotBind)
        << "no symbol-based bind targets the pp __data slot (VA 0x"
        << std::hex << slotVa << ") -- a slot-VA bake regressed the fold";

    // The slot bytes are ZEROED on disk (dyld writes resolved + addend).
    std::uint64_t const slotFileOff =
        dataSegFileOff + (slotVa - dataSegVmaddr);
    ASSERT_LE(slotFileOff + 8, bytes.size());
    EXPECT_EQ(readU64LE(bytes, static_cast<std::size_t>(slotFileOff)), 0u)
        << "extern-addr slot must be zeroed (a non-zero value is the bake)";

    // And the slot is NOT in the rebase stream (bind owns it).
    if (di.rebaseSize > 0) {
        auto const rebases = decodeRebaseStream(
            std::span<std::uint8_t const>{bytes.data() + di.rebaseOff,
                                          di.rebaseSize});
        for (auto const& r : rebases) {
            if (r.segIdx >= segVmaddrs.size()) continue;
            EXPECT_NE(segVmaddrs[r.segIdx] + r.segOff, slotVa)
                << "extern-addr slot must not be BOTH rebased and bound";
        }
    }
}

// ── (5) Codesign: dylib execSegFlags = 0 (not MAIN_BINARY) ────────

TEST(MachoDylibWriter, CodeSignatureExecSegFlagsZeroOnDylib) {
    auto loaded = loadShippedDylib();
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const bytes = encodeDylib(makeExportModule(), loaded);

    auto const lc = findLoadCommand(bytes, kLcCodeSignature);
    ASSERT_TRUE(lc.has_value());
    std::uint32_t const dataOff = readU32LE(bytes, *lc + 8);
    ASSERT_LT(dataOff, bytes.size());
    // SuperBlob(12) + BlobIndex(8) precede the CodeDirectory.
    std::size_t const cd = static_cast<std::size_t>(dataOff) + 20u;
    ASSERT_LE(cd + 88u, bytes.size());
    EXPECT_EQ(readU32LE(bytes, static_cast<std::size_t>(dataOff)),
              0xC00CDEFAu);   // 0xFADE0CC0 big-endian read as LE
    // execSegFlags (CD offset 80, BIG-ENDIAN u64) == 0: a dylib is
    // NOT the main binary (the exec's == 1 is pinned by
    // test_macho_codesign.cpp).
    EXPECT_EQ(readU64BE(bytes, cd + 80), 0u);
}

// ── (6) Fail-loud belts ──────────────────────────────────────────

TEST(MachoDylibWriter, WeakExportFailsLoud) {
    auto loaded = loadShippedDylib();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod = makeExportModule();
    mod.symbols[0].binding = SymbolBinding::Weak;
    DiagnosticReporter rep;
    auto img = dss::macho::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(img.empty());
    EXPECT_GT(rep.errorCount(), 0u);
    EXPECT_TRUE(sawDiagnosticContaining(rep, "D-LK3-DYLIB-WEAK-EXPORT"));
}

TEST(MachoDylibWriter, ModuleSymbolNamingExternImportFailsLoud) {
    // F1: a ModuleSymbol row naming an EXTERN IMPORT is a
    // producer-contract breach -- symbolVa holds the extern's
    // image-local indirection cell (__stubs stub / __got slot), so a
    // symbolVa-only export loop would silently export THAT address
    // and dlsym("puts") on the dylib would return the local stub
    // (the pointer-identity class this cycle fixes). The export set
    // must classify against the DEFINITION tables (the c150 ELF
    // shape) and fail loud here instead.
    auto loaded = loadShippedDylib();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod = makeExportModule();
    ExternImport imp;
    imp.symbol      = SymbolId{99};
    imp.mangledName = "_puts";
    imp.libraryPath = "/usr/lib/libSystem.B.dylib";
    mod.externImports.push_back(std::move(imp));
    mod.symbols.push_back(ModuleSymbol{SymbolId{99}, "_puts",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    DiagnosticReporter rep;
    auto img = dss::macho::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(img.empty());
    EXPECT_GT(rep.errorCount(), 0u);
    EXPECT_TRUE(sawDiagnosticContaining(rep, "EXTERN IMPORT"));
    EXPECT_TRUE(sawDiagnosticContaining(rep, "_puts"));
}

TEST(MachoDylibWriter, ImageEntryOverrideFailsLoud) {
    // A dylib has no image entry; a caller-provided trampoline
    // override is a producer-contract breach (the linker never
    // injects one for a schema without processExit).
    auto loaded = loadShippedDylib();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod = makeExportModule();
    mod.imageEntryOverride = 0u;
    DiagnosticReporter rep;
    auto img = dss::macho::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(img.empty());
    EXPECT_GT(rep.errorCount(), 0u);
    EXPECT_TRUE(sawDiagnosticContaining(rep, "imageEntryOverride"));
}

TEST(MachoDylibWriter, LinkerGateRejectsThreadLocalByAbsence) {
    // D-LK3-DYLIB-TLS-MODEL: tdata is not in supportedDataSections,
    // so the linker's pre-walker gate rejects a thread-local loudly.
    auto loaded = loadShippedDylib();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod = makeExportModule();
    AssembledData t;
    t.symbol    = SymbolId{9};
    t.section   = DataSectionKind::Tdata;
    t.bytes     = {1, 0, 0, 0};
    t.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(t));
    mod.symbols.push_back(ModuleSymbol{SymbolId{9}, "_tls_var",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    DiagnosticReporter rep;
    auto img = linker::link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_GT(rep.errorCount(), 0u);
    EXPECT_TRUE(sawDiagnosticContaining(rep, "supportedDataSections"));
}

// ── (7) validate() shape rules ───────────────────────────────────

namespace {
// A minimal dylib JSON with splice points for extra top-level fields
// and image-block fields (the PE dll test pattern).
[[nodiscard]] std::string dylibJsonWith(std::string_view extraTopLevel,
                                        std::string_view extraImage = "",
                                        std::string_view textVa = "16384") {
    std::string s = R"({
      "dssObjectFormatVersion": 1,
      "dataModel": "LP64",
      "format": {"name":"t-dylib","kind":"macho"},
      )";
    s += extraTopLevel;
    s += R"(
      "macho": { "cputype": 16777228, "cpusubtype": 0, "filetype": "dylib", "flags": 1048709 },
      "image": { )";
    s += extraImage;
    s += R"( "segmentPageSize": 16384, "installName": "@rpath/t.dylib", "loadDylibs": ["/usr/lib/libSystem.B.dylib"] },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":)";
    s += textVa;
    s += R"(}]
    })";
    return s;
}
} // namespace

TEST(MachoDylibFormatJsonValidate, MinimalDylibShapeAccepted) {
    auto r = ObjectFormatSchema::loadFromText(dylibJsonWith(""));
    if (!r.has_value()) {
        for (auto const& d : r.error()) ADD_FAILURE() << d.message;
    }
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->macho().filetype, MachOObjectType::Dylib);
    EXPECT_TRUE((*r)->isImageFlavor());
    EXPECT_FALSE((*r)->allowsUndefinedImports());
}

TEST(MachoDylibFormatJsonValidate, EntryClusterRejected) {
    auto r = ObjectFormatSchema::loadFromText(dylibJsonWith(R"(
      "entryCallingConvention": "apple_arm64",
      "processExit": { "mechanism": "by-name-import", "importMangledName": "_exit", "importLibraryPath": "/usr/lib/libSystem.B.dylib" },
    )"));
    ASSERT_FALSE(r.has_value());
}

TEST(MachoDylibFormatJsonValidate, EntryPointRejected) {
    auto r = ObjectFormatSchema::loadFromText(dylibJsonWith(R"(
      "entryPoint": "main",
    )"));
    ASSERT_FALSE(r.has_value());
}

TEST(MachoDylibFormatJsonValidate, NonZeroPageZeroRejected) {
    auto r = ObjectFormatSchema::loadFromText(
        dylibJsonWith("", R"("pageZeroSize": 4294967296,)"));
    ASSERT_FALSE(r.has_value());
}

TEST(MachoDylibFormatJsonValidate, DylinkerPathRejected) {
    auto r = ObjectFormatSchema::loadFromText(
        dylibJsonWith("", R"("dylinkerPath": "/usr/lib/dyld",)"));
    ASSERT_FALSE(r.has_value());
}

TEST(MachoDylibFormatJsonValidate, TextVaMustEqualSegmentPageSize) {
    // Base-0 image by construction: page 0 = header + load commands,
    // __text opens page 1 (the ELF dyn text-VA == pageAlign mirror).
    auto r = ObjectFormatSchema::loadFromText(
        dylibJsonWith("", "", /*textVa=*/"32768"));
    ASSERT_FALSE(r.has_value());
}

TEST(MachoDylibFormatJsonValidate, ChainedFixupsRejected) {
    // The dylib export trie rides LC_DYLD_INFO_ONLY.export_off; the
    // chained-fixups path would need LC_DYLD_EXPORTS_TRIE
    // (D-LK3-DYLIB-CHAINED-FIXUPS-EXPORT-TRIE).
    auto r = ObjectFormatSchema::loadFromText(
        dylibJsonWith("", R"("useChainedFixups": true,)"));
    ASSERT_FALSE(r.has_value());
}

TEST(MachoDylibFormatJsonValidate, InstallNameOnExecRejected) {
    // installName is the DYLIB's identity; dead config on an
    // executable schema.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "dataModel": "LP64",
      "format": {"name":"t-exec-badid","kind":"macho"},
      "macho": { "cputype": 16777228, "cpusubtype": 0, "filetype": "execute", "flags": 2097285 },
      "image": { "pageZeroSize": 4294967296, "segmentPageSize": 16384, "dylinkerPath": "/usr/lib/dyld", "installName": "@rpath/x.dylib", "loadDylibs": ["/usr/lib/libSystem.B.dylib"] },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294983680}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(MachoDylibFormatJsonValidate, MissingInstallNameRejected) {
    // Config-driven + honest: the walker never derives the identity
    // from the output file name, so an unset installName fails HERE.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "dataModel": "LP64",
      "format": {"name":"t-dylib-noid","kind":"macho"},
      "macho": { "cputype": 16777228, "cpusubtype": 0, "filetype": "dylib", "flags": 1048709 },
      "image": { "segmentPageSize": 16384, "loadDylibs": ["/usr/lib/libSystem.B.dylib"] },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":16384}]
    })");
    ASSERT_FALSE(r.has_value());
}

// -- (8) c171: the x86_64 .dylib variant-parity sibling -----------
//
// macho64-x86_64-darwin-dylib is the config-only mirror of the arm64
// dylib above: EVERY dylib divergence in macho.cpp is FILETYPE-keyed
// (never cputype-keyed), so MH_DYLIB routes the x86_64 target through
// the SAME encodeExecDynamic substrate. There is NO macOS-x86_64 CI
// leg (macos-latest is Apple Silicon), so this ships with STRUCTURAL
// byte-pins ONLY -- the writer emits it correctly, proven here at the
// byte level. Mirrors HeaderPinsDylibShape with the x86_64 arch fields
// (cputype 0x01000007, __text at segmentPageSize 0x1000).

namespace {

[[nodiscard]] Loaded loadShippedDylibX86_64() {
    Loaded out;
    auto t = TargetSchema::loadShipped("x86_64");
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(x86_64) failed";
        for (auto const& d : t.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.target = std::move(t).value();
    }
    auto f = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-dylib");
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(macho64-x86_64-darwin-dylib) failed";
        for (auto const& d : f.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.format = std::move(f).value();
    }
    return out;
}

// The x86_64 mirror of makeExportModule: exported fn `_dss_add`
// (0xC3 ret) + exported int global `_dss_global` + a LOCAL helper
// that must NOT export. Mach-O names arrive PRE-MANGLED (leading `_`).
[[nodiscard]] AssembledModule makeX86_64ExportModule() {
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    AssembledFunction loc;
    loc.symbol = SymbolId{2};
    loc.bytes  = {0x90, 0xC3};
    mod.functions.push_back(std::move(loc));
    AssembledData d;
    d.symbol    = SymbolId{3};
    d.section   = DataSectionKind::Data;
    d.bytes     = {7, 0, 0, 0};
    d.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(d));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "_dss_add",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{2}, "_hidden_helper",
                                       SymbolBinding::Local,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{3}, "_dss_global",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    return mod;
}

} // namespace

TEST(MachoDylibWriterX86_64, HeaderPinsDylibShape) {
    auto loaded = loadShippedDylibX86_64();
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const bytes = encodeDylib(makeX86_64ExportModule(), loaded);
    ASSERT_GE(bytes.size(), 32u);

    // mach_header_64: magic / cputype = CPU_TYPE_X86_64 (0x01000007 =
    // 16777223) / filetype = MH_DYLIB (6) / flags =
    // MH_NOUNDEFS|MH_DYLDLINK|MH_TWOLEVEL|MH_NO_REEXPORTED_DYLIBS =
    // 0x100085 (NO MH_PIE -- an executable-only flag; same value as the
    // arm64 dylib).
    EXPECT_EQ(readU32LE(bytes, 0), 0xFEEDFACFu);
    EXPECT_EQ(readU32LE(bytes, 4), 0x01000007u) << "cputype x86_64";
    EXPECT_EQ(readU32LE(bytes, 12), 6u) << "filetype MH_DYLIB";
    EXPECT_EQ(readU32LE(bytes, 24), 0x100085u);

    // LC_ID_DYLIB present, name offset 24, the configured install name.
    auto const idLc = findLoadCommand(bytes, kLcIdDylib);
    ASSERT_TRUE(idLc.has_value());
    EXPECT_EQ(readU32LE(bytes, *idLc + 8), 24u);   // lc_str offset
    std::string const name(
        reinterpret_cast<char const*>(&bytes[*idLc + 24]));
    EXPECT_EQ(name, "@rpath/libdss.dylib");

    // NO LC_MAIN, NO LC_LOAD_DYLINKER, NO __PAGEZERO (a dylib is
    // base-0; dyld slides the whole image).
    EXPECT_FALSE(findLoadCommand(bytes, kLcMain).has_value());
    EXPECT_FALSE(findLoadCommand(bytes, kLcLoadDylinker).has_value());
    EXPECT_FALSE(findSegment(bytes, "__PAGEZERO").has_value());

    // __TEXT is the FIRST segment at vmaddr 0 (base-0 image), and
    // __text sits at VA 0x1000 = one x86_64 segment page (header page).
    auto const textSeg = findSegment(bytes, "__TEXT");
    ASSERT_TRUE(textSeg.has_value());
    EXPECT_EQ(readU64LE(bytes, *textSeg + 24), 0u);        // vmaddr
    auto const textSec = findSection(bytes, "__TEXT", "__text");
    ASSERT_TRUE(textSec.has_value());
    EXPECT_EQ(readU64LE(bytes, *textSec + 32), 0x1000u);   // addr

    // A code signature IS emitted (ad-hoc SHA-256 -- KEPT on x86_64 for
    // symmetry with the arm64 dylib + modern ld64, though x86_64 macOS
    // does not require it).
    EXPECT_TRUE(findLoadCommand(bytes, kLcCodeSignature).has_value())
        << "the x86_64 dylib must carry an ad-hoc code signature";

    // The exported FUNCTION is findable in the export trie (dlsym's
    // exact lookup); the LOCAL helper is not.
    auto const di = readDyldInfo(bytes);
    ASSERT_TRUE(di.found);
    ASSERT_GT(di.exportSize, 0u);
    std::span<std::uint8_t const> const trie{bytes.data() + di.exportOff,
                                             di.exportSize};
    auto const fn = trieWalk(trie, "_dss_add");
    ASSERT_TRUE(fn.has_value())
        << "_dss_add must be exported via the LC_DYLD_INFO export trie";
    EXPECT_EQ(fn->address, 0x1000u) << "fn[0] at __text VA (base-0)";
    EXPECT_FALSE(trieWalk(trie, "_hidden_helper").has_value());
}
