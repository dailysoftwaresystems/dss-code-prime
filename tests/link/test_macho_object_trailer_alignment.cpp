// D-LINK-MACHO-OBJECT-SYMTAB-MISALIGNED — the MH_OBJECT trailer tables start
// on an address-sized boundary.
//
// The relocatable writer laid its trailer — the `__text` / `__data` / relro
// relocation tables and the nlist_64 symbol table — hard against the end of
// the file-backed section span, so the first table started wherever the
// section bytes happened to end. ✔MEASURED at the base commit over 21 DSS
// objects (10 corpus sources × 2 ports, plus one probe): 12 were misaligned,
// `symoff` ≡ 3, 4, 6 or 7 mod 8 (`examples/c/attributes_syntax` arm64:
// `reloff 292`, `symoff 308`). Apple's own toolchain starts every one of those
// tables 8-aligned (✔MEASURED 2026-09-04, Apple clang 21.0.0: `symoff` 696
// and 624 on two arm64 objects), and relocation_info is an 8-byte record while
// nlist_64 carries an 8-byte `n_value`.
//
// ⚠ The misalignment was ✔MEASURED HARMLESS before the fix — an MH_OBJECT has
// no __LINKEDIT, so dyld's `validStructureLinkedit` never runs on it, and
// Apple `cc` linked and RAN a DSS `.o` with `symoff 308` (rc 42) — which is
// exactly why the fix travels with its own witness rather than by analogy with
// the image tier: the `.o` tier's byte identity across the image-tier fix was
// a CONTROL that fix did not leak, and only a change with its own pins may
// spend it. The Apple `cc` link + RUN of the changed objects and the byte-diff
// (every moved byte a load-command offset field or zero padding) are recorded
// in the row; this file pins the STRUCTURE on every host.
//
// Every module here is built so the raw span end is NOT 8-aligned: a pin over
// a module whose text happens to end on a boundary would stay green with the
// alignment step deleted — the vacuous-green shape this project has paid for.
//
// RED-ON-DISABLE (REMOVE-direction): delete the `alignUp` steps on the trailer
// cursors in `macho::encode` (MH_OBJECT) — `symoff` lands on the raw span end,
// ≡ 4 mod 8 here, and every alignment assertion below fails.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/macho.hpp"
#include "link/format/macho_object_reader.hpp"
#include "link/object_format_schema.hpp"
#include "macho_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using dss::macho::test::findLoadCommand;
using dss::macho::test::readU32LE;
using dss::macho::test::readU64LE;

namespace {

constexpr std::uint64_t kTrailerAlign = 8;

struct Loaded {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

[[nodiscard]] Loaded loadShippedPair(std::string_view targetName,
                                     std::string_view formatName) {
    Loaded out;
    auto t = TargetSchema::loadShipped(targetName);
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(" << targetName << ") failed";
    } else {
        out.target = std::move(t).value();
    }
    auto f = ObjectFormatSchema::loadShipped(formatName);
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(" << formatName << ") failed";
    } else {
        out.format = std::move(f).value();
    }
    return out;
}

// The trailer geometry as the load commands PUBLISH it.
struct Trailer {
    std::uint64_t spanEnd = 0;   // LC_SEGMENT_64 fileoff + filesize
    std::uint32_t symoff  = 0;
    std::uint32_t nsyms   = 0;
    std::uint32_t stroff  = 0;
    std::uint32_t strsize = 0;
    // ("SEGNAME,SECTNAME", reloff, nreloc) for every section carrying
    // relocations. ⚠ THE SEGMENT IS PART OF THE KEY, NOT DECORATION: a
    // section name alone is ambiguous here, and this file holds the ambiguous
    // pair — `macho64-arm64-darwin.format.json` maps rodata to `__TEXT,__const`
    // and relro to `__DATA,__const`, so a bare `__const` names either. It
    // resolves today only because this fixture's rodata carries no relocation;
    // a fixture that relocated its rodata would silently pin the wrong table.
    std::vector<std::pair<std::string, std::pair<std::uint32_t, std::uint32_t>>>
        relocTables;
};

[[nodiscard]] Trailer readTrailer(std::vector<std::uint8_t> const& bytes) {
    Trailer t;
    auto const seg = findLoadCommand(bytes, /*LC_SEGMENT_64=*/0x19u);
    if (seg) {
        t.spanEnd = readU64LE(bytes, *seg + 40) + readU64LE(bytes, *seg + 48);
        std::uint32_t const nsects = readU32LE(bytes, *seg + 64);
        std::size_t sec = *seg + 72;
        for (std::uint32_t i = 0; i < nsects; ++i, sec += 80) {
            // section_64: sectname at +0, segname at +16, both 16-byte
            // NUL-padded fixed fields.
            auto const field16 = [&bytes](std::size_t at) {
                auto const* p = reinterpret_cast<char const*>(&bytes[at]);
                return std::string(p, strnlen(p, 16));
            };
            std::string const qualified =
                field16(sec + 16) + "," + field16(sec);
            std::uint32_t const reloff = readU32LE(bytes, sec + 56);
            std::uint32_t const nreloc = readU32LE(bytes, sec + 60);
            if (nreloc > 0) {
                t.relocTables.push_back({qualified, {reloff, nreloc}});
            }
        }
    }
    auto const sym = findLoadCommand(bytes, /*LC_SYMTAB=*/0x02u);
    if (sym) {
        t.symoff  = readU32LE(bytes, *sym + 8);
        t.nsyms   = readU32LE(bytes, *sym + 12);
        t.stroff  = readU32LE(bytes, *sym + 16);
        t.strsize = readU32LE(bytes, *sym + 20);
    }
    return t;
}

[[nodiscard]] std::uint64_t alignUp(std::uint64_t v, std::uint64_t a) {
    return (v + a - 1) / a * a;
}

// Every byte of [from, to) is zero: the alignment gaps belong to nobody.
void expectZeroGap(std::vector<std::uint8_t> const& bytes, std::uint64_t from,
                   std::uint64_t to, char const* what) {
    ASSERT_LE(to, bytes.size()) << what;
    for (std::uint64_t i = from; i < to; ++i) {
        EXPECT_EQ(bytes[static_cast<std::size_t>(i)], 0u)
            << what << ": pad byte at " << i << " is not zero";
    }
}

// The whole trailer chain: each table starts aligned, at or after the end of
// what precedes it, with zero padding in between, and the string table sits
// exactly at the end of the nlist (it needs no step of its own).
void expectAlignedChain(std::vector<std::uint8_t> const& bytes,
                        Trailer const& t, char const* label) {
    // The FIXTURE guarantee that makes the pins non-vacuous.
    ASSERT_NE(t.spanEnd % kTrailerAlign, 0u)
        << label << ": the raw section span end must NOT be 8-aligned, or "
                    "deleting the alignment step leaves this pin green";
    std::uint64_t prevEnd = t.spanEnd;
    for (auto const& [name, table] : t.relocTables) {
        auto const [reloff, nreloc] = table;
        EXPECT_EQ(reloff % kTrailerAlign, 0u)
            << label << ": " << name << " reloff " << reloff;
        EXPECT_EQ(reloff, alignUp(prevEnd, kTrailerAlign))
            << label << ": " << name << " relocations must start at the "
                        "first aligned offset after the previous table";
        expectZeroGap(bytes, prevEnd, reloff, name.c_str());
        prevEnd = reloff + static_cast<std::uint64_t>(nreloc) * 8u;
    }
    EXPECT_EQ(t.symoff % kTrailerAlign, 0u) << label << ": symoff " << t.symoff;
    EXPECT_EQ(t.symoff, alignUp(prevEnd, kTrailerAlign))
        << label << ": the symbol table must start at the first aligned "
                    "offset after the last relocation table";
    expectZeroGap(bytes, prevEnd, t.symoff, "symbol table");
    EXPECT_EQ(t.stroff, t.symoff + static_cast<std::uint64_t>(t.nsyms) * 16u)
        << label << ": the string table follows the nlist with no gap";
    EXPECT_EQ(static_cast<std::uint64_t>(t.stroff) + t.strsize, bytes.size())
        << label << ": nothing trails the string table";
}

} // namespace

// ── (1) A leaf module with a 12-byte __text: no relocations, no data ────────

TEST(MachoObjectTrailerAlignment, SymtabStartsAlignedAfterAnUnalignedTextSpan) {
    auto loaded = loadShippedPair("arm64", "macho64-arm64-darwin");
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{7};
    fn.bytes  = {0x00, 0x00, 0x80, 0x52,    // MOV w0, #0
                 0x00, 0x00, 0x80, 0x52,    // MOV w0, #0
                 0xC0, 0x03, 0x5F, 0xD6};   // RET   — 12 bytes, ≡ 4 mod 8
    mod.functions.push_back(std::move(fn));
    mod.symbols.push_back(ModuleSymbol{SymbolId{7}, "_leaf", SymbolBinding::Global,
                                       SymbolVisibility::Default});

    DiagnosticReporter rep;
    auto const bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    Trailer const t = readTrailer(bytes);
    EXPECT_TRUE(t.relocTables.empty());
    EXPECT_EQ(t.nsyms, 1u);
    // 32 (header) + 72 + 80 (one section) + 24 (LC_BUILD_VERSION) + 24
    // (LC_SYMTAB) = 232, plus 12 bytes of __text = 244: the pre-fix `symoff`.
    EXPECT_EQ(t.spanEnd, 244u)
        << "the fixture's raw span end moved — re-derive the pre-fix value";
    EXPECT_EQ(t.symoff, 248u) << "244 aligned up to 8";
    expectAlignedChain(bytes, t, "arm64 leaf");
}

// ── (2) Every trailer table at once, then the reader walks the offsets ──────

TEST(MachoObjectTrailerAlignment, EveryTrailerTableStartsAlignedAndTheReaderAgrees) {
    auto loaded = loadShippedPair("arm64", "macho64-arm64-darwin");
    ASSERT_TRUE(loaded.target && loaded.format);

    // 12 bytes of __text (≡ 4 mod 8) carrying a BRANCH26 to an extern and a
    // PAGE21 to a rodata item; a relro table with an abs64 relocation; a
    // writable data item. That is a __text relocation table, a relro
    // relocation table and the nlist — the three cursors the row names, with
    // the data section bytes making the span end land off-boundary again.
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{2};
    fn.bytes.assign(12, 0x1F);
    fn.relocations.push_back(Relocation{0u, SymbolId{21}, RelocationKind{1}, 0});
    fn.relocations.push_back(Relocation{4u, SymbolId{10}, RelocationKind{2}, 0});
    mod.functions.push_back(std::move(fn));

    AssembledData msg;
    msg.symbol    = SymbolId{10};
    msg.section   = DataSectionKind::Rodata;
    msg.bytes     = {'h', 'i', 0};
    msg.alignment = Alignment::of<1>();
    mod.dataItems.push_back(msg);

    AssembledData counter;
    counter.symbol    = SymbolId{11};
    counter.section   = DataSectionKind::Data;
    counter.bytes     = {7, 0, 0, 0};
    counter.alignment = Alignment::of<4>();
    mod.dataItems.push_back(counter);

    AssembledData vtable;
    vtable.symbol    = SymbolId{12};
    vtable.section   = DataSectionKind::RelRoConst;
    vtable.bytes     = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};   // 12: span ≡ 4
    vtable.alignment = Alignment::of<4>();
    vtable.relocations.push_back(Relocation{0u, SymbolId{2}, RelocationKind{4}, 8});
    mod.dataItems.push_back(vtable);

    mod.symbols = {
        ModuleSymbol{SymbolId{2},  "_greet",   SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{10}, "_msg",     SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{11}, "_counter", SymbolBinding::Global, SymbolVisibility::Default},
        ModuleSymbol{SymbolId{12}, "_vtable",  SymbolBinding::Global, SymbolVisibility::Default},
    };
    mod.externImports = {
        ExternImport{SymbolId{21}, "_puts", "/usr/lib/libSystem.B.dylib", /*isData=*/false},
    };

    DiagnosticReporter wrep;
    auto const bytes = macho::encode(mod, *loaded.target, *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    Trailer const t = readTrailer(bytes);
    ASSERT_EQ(t.relocTables.size(), 2u)
        << "a __text table and a relro (__DATA,__const) table";
    EXPECT_EQ(t.relocTables[0].first, "__TEXT,__text");
    EXPECT_EQ(t.relocTables[0].second.second, 2u);
    EXPECT_EQ(t.relocTables[1].first, "__DATA,__const");
    EXPECT_EQ(t.relocTables[1].second.second, 1u);
    EXPECT_EQ(t.nsyms, 5u) << "4 defined + 1 undefined";
    expectAlignedChain(bytes, t, "arm64 full trailer");

    // The offsets the load commands publish are the ones the reader follows:
    // every function, relocation and data item comes back by name, so a
    // published offset that disagreed with the emitted bytes would surface
    // here as a missing or mis-sliced record, not as a silent shift.
    DiagnosticReporter rrep;
    auto const got = macho::readRelocatableObject(bytes, *loaded.target,
                                                  *loaded.format, rrep);
    ASSERT_TRUE(got.has_value()) << "reader errors=" << rrep.errorCount();
    ASSERT_EQ(rrep.errorCount(), 0u);
    ASSERT_EQ(got->functions.size(), 1u);
    EXPECT_EQ(got->functions[0].bytes, std::vector<std::uint8_t>(12, 0x1F));
    EXPECT_EQ(got->functions[0].relocations.size(), 2u);
    ASSERT_EQ(got->dataItems.size(), 3u);
    // The reader's own row ORDER is not the property here (it walks the table
    // its own way); the SET of recovered names is, so both sides are sorted.
    std::vector<std::string> names;
    for (auto const& s : got->symbols) names.push_back(s.name);
    std::sort(names.begin(), names.end());
    std::vector<std::string> expectedNames{"_greet", "_msg", "_counter",
                                           "_vtable"};
    std::sort(expectedNames.begin(), expectedNames.end());
    EXPECT_EQ(names, expectedNames);
    ASSERT_EQ(got->externImports.size(), 1u);
    EXPECT_EQ(got->externImports[0].mangledName, "_puts");
}

// ── (3) The x86_64 port, whose variable-length code lands anywhere ──────────

TEST(MachoObjectTrailerAlignment, X86_64ObjectSymtabStartsAlignedToo) {
    auto loaded = loadShippedPair("x86_64", "macho64-x86_64-darwin");
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{7};
    fn.bytes  = {0xC3};                   // ret — 1 byte, ≡ 1 mod 8
    mod.functions.push_back(std::move(fn));
    mod.symbols.push_back(ModuleSymbol{SymbolId{7}, "_leaf", SymbolBinding::Global,
                                       SymbolVisibility::Default});

    DiagnosticReporter rep;
    auto const bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    Trailer const t = readTrailer(bytes);
    EXPECT_EQ(t.spanEnd, 233u) << "232 of header + commands, 1 byte of __text";
    EXPECT_EQ(t.symoff, 240u);
    expectAlignedChain(bytes, t, "x86_64 leaf");
}
