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
//   * D-LINK-MACHO-IMAGE-SYMBOL-NAMES-REPLACED-BY-SYNTHETIC-IDS: a FINAL
//     IMAGE's nlist carries every DECLARED function name, `static` included,
//     across ALL THREE image arms (static exec / dynamic exec / dylib) and
//     BOTH ports -- with the dylib cell doubling as the ABI control, since the
//     `static` must gain its nlist name while staying OFF the export trie.
//     That last pin lives HERE rather than beside the exec suites because this
//     file owns the dyld trie walk, and separating the relaxation from its
//     control would be the one place the two could drift.
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
#include "format_reject_support.hpp"   // countAtPath / countWithMessage / rejectSummary
#include "link/format/macho.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "macho_test_support.hpp"
#include "program/program.hpp"
#include "program/target_spec.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;
using dss::link_format::test::countAtPath;
using dss::link_format::test::errorCount;
using dss::link_format::test::countWithMessage;
using dss::link_format::test::rejectSummary;

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
    AssembledModule mod = makeExternSlotModule(externIsData, addend);
    // D-LK10-ENTRY 2.13 gate 6 (`resolveEntryFnIdx`): a format declaring
    // `processExit` CONTRACTS that its image entry is the `_start`
    // trampoline, which only `linker::link` injects. This helper drives
    // `dss::macho::encode` DIRECTLY, so on the EXEC arm the module must
    // state that its untrampolined entry IS functions[0] — semantically a
    // no-op (index 0 is what the pre-gate default returned), so no bind /
    // rebase / slot byte pinned below moves. Keyed on the FORMAT's own
    // `processExit` rather than on the caller so the DYLIB arm is left
    // alone: a dylib declares none, has no entry, and its walker REJECTS a
    // caller-supplied override outright (ImageEntryOverrideFailsLoud).
    if (loaded.format->processExit().has_value()) {
        mod.imageEntryOverride = 0u;
    }
    auto const bytes = encodeDylib(mod, loaded);

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
      "cSymbolDecoration": { "scheme": "leading-underscore" },
      "dataModel": "LP64",
      "headerNameMatching": "case-sensitive",
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
      "runtimeLibraries": [{"role":"cLibrary","image":"/usr/lib/libSystem.B.dylib"}],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "_exit" },
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
      "cSymbolDecoration": { "scheme": "leading-underscore" },
      "dataModel": "LP64",
      "headerNameMatching": "case-sensitive",
      "format": {"name":"t-exec-badid","kind":"macho"},
      "macho": { "cputype": 16777228, "cpusubtype": 0, "filetype": "execute", "flags": 2097285 },
      "image": { "pageZeroSize": 4294967296, "segmentPageSize": 16384, "dylinkerPath": "/usr/lib/dyld", "installName": "@rpath/x.dylib", "loadDylibs": ["/usr/lib/libSystem.B.dylib"] },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294983680}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED: this fixture is exec-flavored (filetype "execute") but
    // declares neither `processExit` nor `entryCallingConvention`, so
    // D-LK10-ENTRY 2.13's entry-cluster requirement fires ALONGSIDE the
    // installName-on-exec rule this test pins -- two independent
    // defects, not a knock-on of one. UCRT-P4 adds a THIRD, for the same
    // single fixture property: an exec-flavored format must also declare
    // `entryVerbs` (D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE), the paired twin of the
    // processExit requirement. Each firing is named below rather than absorbed
    // into a looser bound, so a FOURTH unrelated rule still reds this line.
    EXPECT_EQ(errorCount(r), 3u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/image/installName"), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/entryVerbs"), 1u) << rejectSummary(r);
}

TEST(MachoDylibFormatJsonValidate, MissingInstallNameRejected) {
    // Config-driven + honest: the walker never derives the identity
    // from the output file name, so an unset installName fails HERE.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
      "dataModel": "LP64",
      "headerNameMatching": "case-sensitive",
      "format": {"name":"t-dylib-noid","kind":"macho"},
      "macho": { "cputype": 16777228, "cpusubtype": 0, "filetype": "dylib", "flags": 1048709 },
      "image": { "segmentPageSize": 16384, "loadDylibs": ["/usr/lib/libSystem.B.dylib"] },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":16384}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: pageZeroSize/dylinkerPath default to the
    // values a dylib requires, loadDylibs is non-empty, and __text's
    // virtualAddress (16384) already equals segmentPageSize, so the
    // empty installName is the only diagnostic.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/image/installName"), 1u) << rejectSummary(r);
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

// ── D-LINK-MACHO-IMAGE-SYMBOL-NAMES-REPLACED-BY-SYNTHETIC-IDS ───────────────
//
// THE red-on-disable pin: a FINAL Mach-O IMAGE's nlist must carry each
// function's DECLARED name, and must keep `_sym_<id>` for exactly the symbols
// that have no declared name.
//
// ✔MEASURED ON REAL APPLE SILICON (macOS 26.5.2, arm64), the defect this
// closes. The macOS crash reporter -- Apple's own unwinder, symbolicating a
// REAL faulting stack from the image's nlist -- printed
//   #0 sym_83 + 52 / #1 sym_88 + 28 / #2 sym_92 + 44 / #3 sym_147 + 4
// where the source says `static_helper` / `global_helper` / `main` (+ the
// linker-injected trampoline). After the fix the SAME program at the SAME
// offsets printed
//   #0 static_helper + 52 / #1 global_helper + 28 / #2 main + 44 / #3 sym_147 + 4
// -- every offset identical, so this is a NAME-only change, and the trampoline
// correctly keeps its synthetic spelling. lldb agreed both times: `br set -n
// main` reported "no locations (pending)" before, and resolves to
// names_exec_dbg`main after.
//
// ★ WHY THIS LOOPS OVER THREE IMAGE ARMS AND NOT ONE. The Mach-O walker has
// TWO image nlist builders and BOTH were wrong, in DIFFERENT ways:
//   * `encodeExec` (the static, zero-extern arm) hardcoded `_sym_<id>`;
//   * `encodeExecDynamic` carried an `isDylib ? definedName : "_sym_" + id`
//     ternary -- so the EXEC branch hardcoded the synthetic id, AND the DYLIB
//     branch used `definedName`, whose `isExternallyVisible` gate belongs to
//     the RELOCATABLE tier and dropped a `static`'s name from a dylib's nlist
//     too (✔MEASURED: `nm` on a DSS dylib showed `_sym_83` beside a correctly
//     named `_dss_lib_entry`).
// The LIVE arm is the dynamic one -- every Darwin exec schema declares
// `processExit`, so `linker::link` injects a trampoline importing `_exit` and
// every REAL executable carries at least one extern. A pin on the "minimal
// static exec" alone would have tested the arm no shipped build reaches.
//
// ★ THE DYLIB CELL IS THE DECISIVE CONTROL, and it is why the image-tier
// relaxation is safe: in ONE binary the `static` gains its name in the nlist
// while staying ABSENT from the EXPORT TRIE, which is the surface dyld's
// dlsym actually walks. The trie has its OWN `isExternallyVisible` gate over
// the same `module.symbols`, so the two surfaces cannot drift.
// ✔MEASURED BY EXECUTION on the Mac with a probe built by APPLE's clang (a
// neutral third party, not DSS reading its own output): against the fixed
// dylib, `dlsym(h, "lib_static_helper")` is still NULL while
// `dlsym(h, "dss_lib_entry")` resolves AND calls correctly (returned 43).
//
// ★ EVERY CELL RUNS THROUGH A `void` CALLABLE so a failed ASSERT returns from
// the BODY instead of aborting the whole matrix -- a safe arm masking a
// dangerous one has been measured in this project twice.
namespace {

// Every nlist name in an emitted image, in emission order. Returned as a LIST
// and asserted by MEMBERSHIP so the pin survives a legitimate reordering of the
// symbol table (not what it exists to police) while still failing on a changed
// SPELLING (which is). nlist_64 is 16 bytes: n_strx(4) n_type(1) n_sect(1)
// n_desc(2) n_value(8).
[[nodiscard]] std::vector<std::string>
imageNlistNames(std::vector<std::uint8_t> const& bytes) {
    std::vector<std::string> out;
    auto const lc = findLoadCommand(bytes, /*LC_SYMTAB=*/0x02u);
    if (!lc) return out;
    std::uint32_t const symOff = readU32LE(bytes, *lc + 8);
    std::uint32_t const nsyms  = readU32LE(bytes, *lc + 12);
    std::uint32_t const strOff = readU32LE(bytes, *lc + 16);
    for (std::uint32_t i = 0; i < nsyms; ++i) {
        std::size_t const rec = static_cast<std::size_t>(symOff) + i * 16u;
        if (rec + 16 > bytes.size()) break;
        std::size_t p =
            static_cast<std::size_t>(strOff) + readU32LE(bytes, rec);
        std::string name;
        while (p < bytes.size() && bytes[p] != 0)
            name.push_back(static_cast<char>(bytes[p++]));
        out.push_back(std::move(name));
    }
    return out;
}

// The three FINAL-IMAGE arms this pin must cover. Named by the walker each
// selects, so a cell that lands in the wrong one says so by name.
enum class ImageArm { StaticExec, DynamicExec, Dylib };

struct MachoPortSpec {
    char const*               label;
    char const*               targetName;
    char const*               execFormat;
    char const*               dylibFormat;
    std::vector<std::uint8_t> retBytes;
};

} // namespace

TEST(MachoImageSymbolNames,
     ImageNlistNamesEveryDeclaredFunctionOnBothPortsAndAllThreeImageArms) {
    // The ONLY per-port data here is the return instruction. Every assertion
    // below is identical for both ports, because the naming decision is made in
    // format-neutral substrate (`ObjectSymbolNames::imageName`) that no target
    // or format can reach into.
    std::vector<MachoPortSpec> const ports{
        {"arm64", "arm64",
         "macho64-arm64-darwin-exec", "macho64-arm64-darwin-dylib",
         {0xC0, 0x03, 0x5F, 0xD6}},                     // RET
        {"x86_64", "x86_64",
         "macho64-x86_64-darwin-exec", "macho64-x86_64-darwin-dylib",
         {0xC3}},                                       // ret
    };

    // ONE cell = one (port, image arm) pair. `void` on purpose -- see above.
    auto runCell = [](MachoPortSpec const& port, ImageArm arm) -> void {
        char const* const armLabel =
            arm == ImageArm::StaticExec  ? " [static exec arm]"
          : arm == ImageArm::DynamicExec ? " [dynamic exec arm]"
                                         : " [dylib arm]";
        std::string const label = std::string{port.label} + armLabel;
        bool const isDylibCell = arm == ImageArm::Dylib;

        auto target = TargetSchema::loadShipped(port.targetName);
        ASSERT_TRUE(target.has_value()) << label;
        auto fmt = ObjectFormatSchema::loadShipped(
            isDylibCell ? port.dylibFormat : port.execFormat);
        ASSERT_TRUE(fmt.has_value()) << label;

        AssembledModule mod;
        mod.expectedFuncCount = 3;

        // fn #7 -- a `static` (Local binding) function WITH a declared name.
        // THE discriminating case: a final image wants this name (it is the
        // frame a debugger and a crash reporter print), while a relocatable
        // `.o` must NOT expose it (a foreign linker keys by name, and two TUs'
        // `helper`s would collide -- D-LK-INTERNAL-LINKAGE-FN-EMITTED-GLOBAL-
        // FOREIGN-COLLISION). Pre-fix, every arm spelled it `_sym_7`.
        AssembledFunction f7;
        f7.symbol = SymbolId{7};
        f7.bytes  = port.retBytes;
        mod.functions.push_back(std::move(f7));

        // fn #8 -- an externally-visible function with a declared name. Pre-fix
        // the two EXEC arms lost this one too; only the dylib arm kept it.
        AssembledFunction f8;
        f8.symbol = SymbolId{8};
        f8.bytes  = port.retBytes;
        mod.functions.push_back(std::move(f8));

        // fn #9 -- NO `ModuleSymbol` row at all: the shape of the linker-
        // injected `_start` trampoline (minted SymbolId, no declared name).
        // This one MUST keep `_sym_9`; the fallback is the deliberate answer
        // for a symbol that genuinely has no name, not an oversight.
        AssembledFunction f9;
        f9.symbol = SymbolId{9};
        f9.bytes  = port.retBytes;
        mod.functions.push_back(std::move(f9));

        mod.symbols.push_back(ModuleSymbol{SymbolId{7}, "_img_static_fn",
                                           SymbolBinding::Local,
                                           SymbolVisibility::Default});
        mod.symbols.push_back(ModuleSymbol{SymbolId{8}, "_img_global_fn",
                                           SymbolBinding::Global,
                                           SymbolVisibility::Default});

        // The EXTERN is what routes an exec to `encodeExecDynamic` -- exactly
        // how every shipped build gets there (the injected trampoline imports
        // `_exit`). Its ABSENCE is what keeps the static cell on `encodeExec`.
        if (arm == ImageArm::DynamicExec) {
            ExternImport imp;
            imp.symbol      = SymbolId{99};
            imp.mangledName = "_puts";
            imp.libraryPath = "/usr/lib/libSystem.B.dylib";
            mod.externImports.push_back(std::move(imp));
        }
        // A dylib is entry-less; only the exec arms need the D-LK10-ENTRY
        // override that stands in for the trampoline `linker::link` would have
        // injected.
        if (!isDylibCell) mod.imageEntryOverride = std::size_t{0};

        // ★ THE STATIC ARM IS NOT REACHABLE ON EVERY SHIPPED EXEC FORMAT, and
        // this cell ASSERTS that boundary rather than skipping past it. The
        // arm64 exec schema declares `image.buildVersion` (modern dyld rejects
        // an Apple Silicon main executable without LC_BUILD_VERSION), and
        // `encodeExec` REFUSES such a schema loud
        // (D-LK10-ENTRY-MACHO-STATIC-BUILD-VERSION) because only the dynamic
        // arm emits that load command. The x86_64 exec schema deliberately
        // OMITS the key for exactly this reason -- its own
        // `$remainingDeliberateOmissionsComment` says so -- which is what
        // keeps the static walker reachable from a test at all.
        // So: on a format that declares it, this cell pins the LOUD REFUSAL
        // (silently emitting an unloadable image is the failure that gate
        // exists to prevent); on one that does not, it pins the NAMES. Either
        // way the cell asserts something that can go red, and the naming fix
        // is still witnessed on BOTH ports by the dynamic exec + dylib cells
        // -- which is where every shipped build actually lands.
        bool const staticArmRefusedByBuildVersion =
            arm == ImageArm::StaticExec
            && (*fmt)->machoImage().buildVersion.has_value();

        DiagnosticReporter rep;
        auto const bytes = dss::macho::encode(mod, **target, **fmt, rep);
        std::string diags;
        for (auto const& d : rep.all()) diags += d.actual + "\n";

        if (staticArmRefusedByBuildVersion) {
            EXPECT_TRUE(bytes.empty())
                << label
                << ": a schema declaring image.buildVersion must NOT encode "
                   "down the static arm -- that arm emits no LC_BUILD_VERSION, "
                   "so the image would be silently unloadable";
            bool sawAnchor = false;
            for (auto const& d : rep.all()) {
                if (d.actual.find("D-LK10-ENTRY-MACHO-STATIC-BUILD-VERSION")
                    != std::string::npos) {
                    sawAnchor = true;
                }
            }
            EXPECT_TRUE(sawAnchor)
                << label
                << ": the refusal must NAME its anchor so the boundary stays "
                   "findable\n" << diags;
            return;
        }

        ASSERT_EQ(rep.errorCount(), 0u) << label << "\n" << diags;
        ASSERT_FALSE(bytes.empty()) << label << "\n" << diags;

        // Each cell must really reach the arm it NAMES -- otherwise cells could
        // silently share one builder and most of the matrix would assert
        // nothing about the arm it claims to cover. filetype separates dylib
        // from exec; __LINKEDIT separates the two exec walkers (the static arm
        // emits none, which is also why it can carry no code signature).
        std::uint32_t const filetype = readU32LE(bytes, 12);
        EXPECT_EQ(filetype, isDylibCell ? 6u : 2u)
            << label << ": wrong MH_ filetype for the arm this cell names";
        if (!isDylibCell) {
            EXPECT_EQ(findSegment(bytes, "__LINKEDIT").has_value(),
                      arm == ImageArm::DynamicExec)
                << label
                << ": this cell did not reach the exec walker it names, so its "
                   "result says nothing about that arm";
        }

        auto const names = imageNlistNames(bytes);
        ASSERT_FALSE(names.empty()) << label << ": no nlist names read";
        auto has = [&](std::string_view want) {
            return std::find(names.begin(), names.end(), want) != names.end();
        };

        // THE FIX, both directions: the declared names are PRESENT...
        EXPECT_TRUE(has("_img_static_fn"))
            << label
            << ": a `static` function must appear under its DECLARED name in a "
               "final image -- this is the frame a debugger and the macOS crash "
               "reporter print";
        EXPECT_TRUE(has("_img_global_fn"))
            << label << ": an externally-visible function must keep its name";
        // ...and the pre-fix synthetic spellings are GONE. These are the
        // red-on-disable discriminators: restore either hardcoded
        // `"_sym_" + id`, or re-gate `imageName` on visibility, and they fail.
        EXPECT_FALSE(has("_sym_7"))
            << label
            << ": `_sym_7` is the PRE-FIX spelling of `_img_static_fn`; "
               "emitting it means the image threw the declared name away again";
        EXPECT_FALSE(has("_sym_8"))
            << label
            << ": `_sym_8` is the PRE-FIX spelling of `_img_global_fn`";

        // The deliberate fallback survives, and is asserted rather than
        // assumed: if this ever fails, a nameless symbol has acquired a name
        // from somewhere it should not have.
        EXPECT_TRUE(has("_sym_9"))
            << label
            << ": a symbol with NO declared name (the injected entry "
               "trampoline's shape) must keep the `_sym_<id>` fallback";

        // ── THE ABI CONTROL, in the SAME binary as the relaxation ──
        // Naming a `static` in the nlist must NOT put it on the export
        // surface. The trie is what dlsym walks; the nlist is not.
        if (isDylibCell) {
            auto const di = readDyldInfo(bytes);
            ASSERT_TRUE(di.found) << label;
            ASSERT_GT(di.exportSize, 0u) << label;
            ASSERT_LE(static_cast<std::size_t>(di.exportOff) + di.exportSize,
                      bytes.size()) << label;
            std::span<std::uint8_t const> const trie{
                bytes.data() + di.exportOff, di.exportSize};
            EXPECT_TRUE(trieWalk(trie, "_img_global_fn").has_value())
                << label
                << ": the externally-visible function must still be EXPORTED";
            EXPECT_FALSE(trieWalk(trie, "_img_static_fn").has_value())
                << label
                << ": THE CONTROL -- a `static` gained its name in the nlist, "
                   "but it must NOT have leaked onto the export trie, which is "
                   "the ABI surface dlsym walks";
            EXPECT_FALSE(trieWalk(trie, "_sym_9").has_value())
                << label << ": a nameless symbol must never be exported";
        }
    };

    for (auto const& port : ports) {
        runCell(port, ImageArm::StaticExec);
        runCell(port, ImageArm::DynamicExec);
        runCell(port, ImageArm::Dylib);
    }
}

// ── D-LINK-MACHO-LINKEDIT-SYMTAB-MISALIGNED ──────────────────────
// A final image's __LINKEDIT is a chain of blobs whose file offsets the load
// commands PUBLISH. Apple's image validator (dyld's
// `Header::validStructureLinkedit`, which Apple's `ld` runs on every link
// INPUT) requires each blob to START on a pointer-sized boundary -- 8 here --
// and refuses the WHOLE image on the first one that fails:
//     ld: mis-aligned LINKEDIT content 'symbol table' in '<image>'
// ★★ WHY THIS PIN EXISTS, and why a green suite could not see the defect it
// covers: the images LOADED and RAN perfectly while misaligned (dyld's own
// mapping does not care, and `nm` / `otool` / the crash reporter read the
// table fine), so every runtime witness this project owns stayed green while
// a DSS-built dylib could not be handed to the Apple toolchain as a LINK
// INPUT at all. ✔MEASURED on real Apple Silicon (macOS 26.5.2, ld-1267): the
// same `ld` invocation goes rc=1 with that exact sentence before the fix and
// rc=0 after, on the arm64 dylib, the arm64 exec (via `-bundle_loader`) and
// the x86_64 dylib.
// ★ THE ALIGNMENT USED TO LIVE IN THE PRODUCERS, which is why it held for
// three blobs and not the fourth: the rebase, bind and export-trie builders
// each pad their OWN tail "so the next payload starts aligned". The indirect
// symbol table has no such producer step -- it is `count * 4` -- so an ODD
// number of indirect symbols left the nlist table at (prev + 4). That is the
// shape this test builds ON PURPOSE (see the odd-count assertion below);
// without it every cell would pass pre-fix and assert nothing.
namespace {

// One __LINKEDIT blob exactly as the IMAGE publishes it, plus the alignment
// Apple's validator requires of its START. Pointer-sized (8) for everything
// except the indirect symbol table (4) and the string table (1) -- and the
// two lax ones are emitted 8-aligned anyway, so `requiredAlign` carries the
// SPEC's number and not ours (a test asserting our stricter rule would fail
// the day the writer legitimately relaxed it).
struct LinkeditBlobView {
    char const*   name;
    std::uint64_t requiredAlign;
    std::uint32_t off;
    std::uint32_t size;
};

[[nodiscard]] std::vector<LinkeditBlobView>
readLinkeditBlobs(std::span<std::uint8_t const> b) {
    std::vector<LinkeditBlobView> out;
    if (auto lc = findLoadCommand(b, 0x80000022u)) {   // LC_DYLD_INFO_ONLY
        out.push_back({"rebase opcodes", 8, readU32LE(b, *lc + 8),
                       readU32LE(b, *lc + 12)});
        out.push_back({"bind opcodes", 8, readU32LE(b, *lc + 16),
                       readU32LE(b, *lc + 20)});
        out.push_back({"weak bind opcodes", 8, readU32LE(b, *lc + 24),
                       readU32LE(b, *lc + 28)});
        out.push_back({"lazy bind opcodes", 8, readU32LE(b, *lc + 32),
                       readU32LE(b, *lc + 36)});
        out.push_back({"exports trie", 8, readU32LE(b, *lc + 40),
                       readU32LE(b, *lc + 44)});
    }
    if (auto lc = findLoadCommand(b, 0x80000034u)) {   // LC_DYLD_CHAINED_FIXUPS
        out.push_back({"chained fixups", 8, readU32LE(b, *lc + 8),
                       readU32LE(b, *lc + 12)});
    }
    if (auto lc = findLoadCommand(b, 0x0Bu)) {         // LC_DYSYMTAB
        out.push_back({"indirect symbol table", 4, readU32LE(b, *lc + 56),
                       readU32LE(b, *lc + 60) * 4u});
    }
    if (auto lc = findLoadCommand(b, 0x02u)) {         // LC_SYMTAB
        out.push_back({"symbol table", 8, readU32LE(b, *lc + 8),
                       readU32LE(b, *lc + 12) * 16u});
        out.push_back({"symbol table strings", 1, readU32LE(b, *lc + 16),
                       readU32LE(b, *lc + 20)});
    }
    if (auto lc = findLoadCommand(b, 0x1Du)) {         // LC_CODE_SIGNATURE
        out.push_back({"code signature", 8, readU32LE(b, *lc + 8),
                       readU32LE(b, *lc + 12)});
    }
    return out;
}

// The indirect symbol COUNT the image published -- the DRIVER of the defect
// (odd count => `count * 4` is 4 mod 8 => the packed nlist cursor lands
// misaligned). Read back from the image rather than assumed from the module,
// so a writer change that stopped emitting the data extern's __got slot makes
// the cells fail loudly instead of going quietly vacuous.
[[nodiscard]] std::uint32_t indirectSymbolCount(std::span<std::uint8_t const> b) {
    auto lc = findLoadCommand(b, 0x0Bu);
    return lc ? readU32LE(b, *lc + 60) : 0u;
}

}  // namespace

TEST(MachoImageLinkeditAlignment,
     EveryLinkeditBlobStartsOnApplesBoundaryOnBothPortsAndBothDynamicArms) {
    struct PortSpec {
        char const*               label;
        char const*               targetName;
        char const*               execFormat;
        char const*               dylibFormat;
        std::vector<std::uint8_t> retBytes;
    };
    std::vector<PortSpec> const ports{
        {"arm64", "arm64",
         "macho64-arm64-darwin-exec", "macho64-arm64-darwin-dylib",
         {0xC0, 0x03, 0x5F, 0xD6}},                     // RET
        {"x86_64", "x86_64",
         "macho64-x86_64-darwin-exec", "macho64-x86_64-darwin-dylib",
         {0xC3}},                                       // ret
    };

    // ONE cell = one (port, arm) pair, run through a `void` callable so a
    // failed ASSERT in one cell cannot cancel the other three.
    auto runCell = [](PortSpec const& port, bool isDylibCell) -> void {
        std::string const label =
            std::string{port.label} + (isDylibCell ? " [dylib arm]"
                                                   : " [dynamic exec arm]");

        auto target = TargetSchema::loadShipped(port.targetName);
        ASSERT_TRUE(target.has_value()) << label;
        auto fmt = ObjectFormatSchema::loadShipped(
            isDylibCell ? port.dylibFormat : port.execFormat);
        ASSERT_TRUE(fmt.has_value()) << label;

        AssembledModule mod;
        mod.expectedFuncCount = 2;
        for (std::uint32_t id : {1u, 2u}) {
            AssembledFunction fn;
            fn.symbol = SymbolId{id};
            fn.bytes  = port.retBytes;
            mod.functions.push_back(std::move(fn));
        }
        mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "_dss_align_a",
                                           SymbolBinding::Global,
                                           SymbolVisibility::Default});
        mod.symbols.push_back(ModuleSymbol{SymbolId{2}, "_dss_align_b",
                                           SymbolBinding::Global,
                                           SymbolVisibility::Default});

        // ★ THE SHAPE THAT MAKES THIS CELL ABLE TO GO RED. Indirect symbols =
        // one per STUB (function externs only) + one per __got slot (ALL
        // externs). An all-function extern set is therefore always EVEN and
        // leaves the nlist table 8-aligned by luck; ONE DATA extern makes it
        // ODD, `count * 4` lands at 4 mod 8, and the packed cursor put the
        // nlist table right there. This is exactly what a real `#include
        // <stdio.h>` + `fprintf(stderr, ...)` translation unit produces --
        // that CLI probe measured symoff = 50076 pre-fix.
        ExternImport fnImp;
        fnImp.symbol      = SymbolId{98};
        fnImp.mangledName = "_puts";
        fnImp.libraryPath = "/usr/lib/libSystem.B.dylib";
        mod.externImports.push_back(std::move(fnImp));
        ExternImport dataImp;
        dataImp.symbol      = SymbolId{99};
        dataImp.mangledName = "___stdoutp";
        dataImp.libraryPath = "/usr/lib/libSystem.B.dylib";
        dataImp.isData      = true;
        mod.externImports.push_back(std::move(dataImp));

        // A data slot whose abs64 reloc targets the DATA extern, so the __got
        // slot is genuinely referenced rather than dead.
        // ⚠ THE KIND TAG IS RESOLVED BY NAME, NOT TYPED AS A LITERAL. The
        // vocabulary is per-TARGET and the two ports disagree: `abs64` is row
        // 4 on arm64 and row 2 on x86_64, where 4 is `tls-tpoff32`. A literal
        // `RelocationKind{4}` copied from an arm64-only test compiles fine and
        // makes the x86_64 cells fail on a TLS width check instead of testing
        // anything -- MEASURED, it happened while writing this test.
        auto const* abs64 = (*target)->relocationByName("abs64");
        ASSERT_NE(abs64, nullptr)
            << label << ": this target declares no `abs64` relocation";
        AssembledData slot;
        slot.symbol    = SymbolId{5};
        slot.section   = DataSectionKind::Data;
        slot.bytes     = std::vector<std::uint8_t>(8, 0);
        slot.alignment = Alignment::of<8>();
        Relocation rel;
        rel.offset = 0;
        rel.target = SymbolId{99};
        rel.kind   = abs64->kind;
        rel.addend = 0;
        slot.relocations.push_back(rel);
        mod.dataItems.push_back(std::move(slot));
        mod.symbols.push_back(ModuleSymbol{SymbolId{5}, "_dss_align_slot",
                                           SymbolBinding::Global,
                                           SymbolVisibility::Default});

        // The exec arm needs the D-LK10-ENTRY override that stands in for the
        // trampoline `linker::link` would have injected; a dylib is entryless.
        if (!isDylibCell) mod.imageEntryOverride = std::size_t{0};

        DiagnosticReporter rep;
        auto const bytes = dss::macho::encode(mod, **target, **fmt, rep);
        std::string diags;
        for (auto const& d : rep.all()) diags += d.actual + "\n";
        ASSERT_EQ(rep.errorCount(), 0u) << label << "\n" << diags;
        ASSERT_FALSE(bytes.empty()) << label << "\n" << diags;

        // Each cell must really reach the arm it NAMES.
        EXPECT_EQ(readU32LE(bytes, 12), isDylibCell ? 6u : 2u)
            << label << ": wrong MH_ filetype for the arm this cell names";
        auto const linkeditSeg = findSegment(bytes, "__LINKEDIT");
        ASSERT_TRUE(linkeditSeg.has_value())
            << label
            << ": this cell did not reach a DYNAMIC image walker -- only "
               "those emit __LINKEDIT, so its result would say nothing about "
               "the cursor chain it claims to cover";
        std::uint64_t const leOff  = readU64LE(bytes, *linkeditSeg + 40);
        std::uint64_t const leSize = readU64LE(bytes, *linkeditSeg + 48);

        // ★ THE REACHABILITY WITNESS. If this count were EVEN, every
        // alignment assertion below would hold PRE-FIX too and this test
        // would assert nothing -- so the odd count is pinned, not assumed.
        std::uint32_t const nIndirect = indirectSymbolCount(bytes);
        ASSERT_NE(nIndirect, 0u)
            << label << ": no indirect symbol table -- the cell lost the "
                        "extern set that drives this defect";
        ASSERT_EQ(nIndirect % 2u, 1u)
            << label << ": the indirect symbol count is " << nIndirect
            << ", which is EVEN. `count * 4` is then already 8-aligned and the "
               "packed cursor would have landed the nlist table correctly by "
               "luck -- this cell must carry an ODD count or it proves nothing";

        auto const blobs = readLinkeditBlobs(bytes);
        ASSERT_FALSE(blobs.empty()) << label;
        bool sawSymtab = false;
        for (auto const& blob : blobs) {
            if (blob.off == 0 && blob.size == 0) continue;   // absent
            EXPECT_EQ(blob.off % blob.requiredAlign, 0u)
                << label << ": __LINKEDIT blob '" << blob.name << "' starts at "
                << blob.off << " (" << (blob.off % 8u)
                << " mod 8), but Apple's image validator requires a multiple "
                   "of " << blob.requiredAlign
                << " and refuses the WHOLE image with `ld: mis-aligned "
                   "LINKEDIT content '" << blob.name
                << "'` -- D-LINK-MACHO-LINKEDIT-SYMTAB-MISALIGNED";
            EXPECT_GE(blob.off, leOff)
                << label << ": blob '" << blob.name
                << "' starts before __LINKEDIT";
            EXPECT_LE(static_cast<std::uint64_t>(blob.off) + blob.size,
                      leOff + leSize)
                << label << ": blob '" << blob.name
                << "' runs past the end of __LINKEDIT";
            EXPECT_LE(static_cast<std::uint64_t>(blob.off) + blob.size,
                      bytes.size())
                << label << ": blob '" << blob.name
                << "' runs past the end of the image";
            if (std::string_view{blob.name} == "symbol table") {
                sawSymtab = true;
                // Named separately from the loop's generic assertion because
                // this is the blob the row was opened on, and a future reader
                // grepping the anchor must land on an assertion, not a list.
                EXPECT_EQ(blob.off % 8u, 0u)
                    << label
                    << ": THE ANCHOR CASE -- an nlist_64 carries an 8-byte "
                       "n_value and `symoff` is " << blob.off
                    << ". Pre-fix this measured 50076 (4 mod 8) on a real "
                       "build and Apple's `ld` refused the image outright";
            }
        }
        EXPECT_TRUE(sawSymtab)
            << label << ": no LC_SYMTAB -- the anchor's own blob is missing";

        // ★ CONTENT, not just the published offset. The cursor arithmetic and
        // the byte emission are two SEPARATE views of this chain (one says
        // where a blob goes, the other appends it), and a fix that padded only
        // the cursors would publish a correct `symoff` over the WRONG BYTES --
        // a silent miscompile that every offset assertion above would pass.
        // So the nlist table is read back THROUGH the published offsets and
        // required to yield the names this module declared.
        auto const names = imageNlistNames(bytes);
        auto hasName = [&](std::string_view want) {
            return std::find(names.begin(), names.end(), want) != names.end();
        };
        EXPECT_TRUE(hasName("_dss_align_a"))
            << label
            << ": the nlist table read THROUGH the published symoff/stroff "
               "does not contain this module's own symbol -- the load "
               "commands and the emitted bytes disagree about where the "
               "symbol table is";
        EXPECT_TRUE(hasName("_dss_align_b")) << label;

        // The blobs must also be ASCENDING and non-overlapping: the fix works
        // by padding each cursor FORWARD, and a padding bug that pushed one
        // blob over its neighbour would still satisfy every alignment
        // assertion above.
        std::uint64_t prevEnd  = leOff;
        char const*   prevName = "__LINKEDIT start";
        for (auto const& blob : blobs) {
            if (blob.off == 0 && blob.size == 0) continue;
            EXPECT_GE(blob.off, prevEnd)
                << label << ": blob '" << blob.name << "' starts at "
                << blob.off << ", inside '" << prevName << "' which ends at "
                << prevEnd;
            prevEnd  = static_cast<std::uint64_t>(blob.off) + blob.size;
            prevName = blob.name;
        }
    };

    for (auto const& port : ports) {
        runCell(port, /*isDylibCell=*/false);
        runCell(port, /*isDylibCell=*/true);
    }
}
