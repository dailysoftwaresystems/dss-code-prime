// P69 fold 3c, lane p69/xa — a COFF section with more relocations than its u16 NumberOfRelocations holds.
//
// PE/COFF's convention (IMAGE_SCN_LNK_NRELOC_OVFL): the section sets the flag, its NumberOfRelocations reads 0xFFFF,
// and its FIRST relocation record is not a relocation — its VirtualAddress is the record count, itself included —
// as LLVM's COFFObjectFile reads it and GNU ld and LLVM's writer write it.
//
//   D-LK-COFF-READER-IGNORES-THE-RELOCATION-COUNT-OVERFLOW — the reader took the count record for a relocation:
//     ✔MEASURED 2026-10-20, the lane's dsscp on x86_64-w64-mingw32-gcc -O0's object of
//     examples/c/long_branch_imm19_escape (main renamed; `.text` NumberOfRelocations 0xFFFF, first record
//     VirtualAddress 98307) refused it, F_CorruptedBinary "relocation Type 0 in section '.text' is not declared by PE
//     format 'pe64-x86_64-windows-staticlib'", where GNU ld links it and the program exits 42.
//   D-LK2-3 — the writer refused such a section as a capacity it lacked (K_NoMatchingObjectFormat).
//
// The object is not a fixture (1.6 MB): each test SYNTHESIZES its table through DSS's own PE object writer, and reads
// the header with a reader of its own, so the writer is checked against what the bytes say and the reader against
// bytes the writer did not choose for it.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

constexpr std::uint32_t kNrelocOvfl = 0x01000000u;

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  <no diagnostic>"} : s;
}

[[nodiscard]] bool said(DiagnosticReporter const& rep, std::string_view text) {
    return std::any_of(rep.all().begin(), rep.all().end(),
                       [&](auto const& d) { return d.actual.find(text) != std::string::npos; });
}

struct Loaded {
    std::shared_ptr<TargetSchema const>       target;
    std::shared_ptr<ObjectFormatSchema const> object;
};

[[nodiscard]] Loaded load() {
    Loaded l;
    if (auto t = TargetSchema::loadShipped("x86_64"); t.has_value()) l.target = *t;
    if (auto f = ObjectFormatSchema::loadShipped("pe64-x86_64-windows"); f.has_value()) l.object = *f;
    EXPECT_TRUE(l.target && l.object) << "the shipped x86_64 target and pe64 object documents";
    return l;
}

// One function of `calls` `call ext_fn` instructions, each a rel32 relocation against the import.
[[nodiscard]] AssembledModule callsToAnImport(TargetSchema const& t, std::uint32_t calls) {
    AssembledModule m;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes.reserve(static_cast<std::size_t>(calls) * 5u + 1u);
    auto const* rel32 = t.relocationByName("rel32");
    EXPECT_NE(rel32, nullptr);
    for (std::uint32_t i = 0; i < calls; ++i) {
        Relocation r;
        r.offset = i * 5u + 1u;
        r.target = SymbolId{2};
        r.kind   = rel32 != nullptr ? rel32->kind : RelocationKind{};
        fn.relocations.push_back(r);
        fn.bytes.insert(fn.bytes.end(), {0xE8, 0x00, 0x00, 0x00, 0x00});
    }
    fn.bytes.push_back(0xC3);
    m.functions.push_back(std::move(fn));
    m.expectedFuncCount = 1;
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "calls", SymbolBinding::Global, SymbolVisibility::Default});
    ExternImport imp;
    imp.symbol      = SymbolId{2};
    imp.mangledName = "ext_fn";
    m.externImports.push_back(imp);
    return m;
}

[[nodiscard]] std::vector<std::uint8_t> writeObject(Loaded const& l, std::uint32_t calls, DiagnosticReporter& rep) {
    std::vector<AssembledModule> one{callsToAnImport(*l.target, calls)};
    LinkedImage const img = linker::link(std::span<AssembledModule const>{one}, *l.target, *l.object, rep);
    return img.ok() ? img.bytes : std::vector<std::uint8_t>{};
}

// The `.text` section header of a COFF object: where its fields sit, read by this test alone.
struct TextHeader {
    std::size_t   at = 0;               // the header's file offset
    std::uint32_t relocPointer = 0;
    std::uint16_t numberOfRelocations = 0;
    std::uint32_t characteristics = 0;
};

[[nodiscard]] std::uint32_t u32(std::vector<std::uint8_t> const& b, std::size_t o) {
    return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8)
         | (static_cast<std::uint32_t>(b[o + 2]) << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}
[[nodiscard]] std::uint16_t u16(std::vector<std::uint8_t> const& b, std::size_t o) {
    return static_cast<std::uint16_t>(b[o] | (b[o + 1] << 8));
}
void put32(std::vector<std::uint8_t>& b, std::size_t o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[o + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
}

[[nodiscard]] std::optional<TextHeader> textHeaderOf(std::vector<std::uint8_t> const& b) {
    if (b.size() < 20) return std::nullopt;
    std::uint16_t const sections = u16(b, 2);
    std::size_t const   first    = 20u + u16(b, 16);
    for (std::uint16_t i = 0; i < sections; ++i) {
        std::size_t const h = first + 40u * i;
        if (std::string_view{reinterpret_cast<char const*>(b.data() + h), 5} != ".text") continue;
        return TextHeader{h, u32(b, h + 24), u16(b, h + 32), u32(b, h + 36)};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<AssembledModule> read(Loaded const& l, std::vector<std::uint8_t> const& b,
                                                  DiagnosticReporter& rep) {
    return pe::readRelocatableObject(b, *l.target, *l.object, rep, CompilationUnitId{1});
}

}  // namespace

// 65534 relocations fit the field: no flag, no count record. 65535 do not: the flag, 0xFFFF, and a first record whose
// VirtualAddress counts the 65535 relocations and itself (symbol 0, type 0).
TEST(CoffRelocationCountOverflow, TheWriterStatesATablePastTheFieldInItsFirstRecord) {
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    {
        DiagnosticReporter rep;
        auto const         b = writeObject(l, 65534u, rep);
        ASSERT_FALSE(b.empty()) << diagnosticsOf(rep);
        auto const h = textHeaderOf(b);
        ASSERT_TRUE(h.has_value());
        EXPECT_EQ(h->numberOfRelocations, 65534u);
        EXPECT_EQ(h->characteristics & kNrelocOvfl, 0u) << "a count the field holds sets no flag";
    }
    DiagnosticReporter rep;
    auto const         b = writeObject(l, 65535u, rep);
    ASSERT_FALSE(b.empty()) << "a table past the u16 field is written, not refused:" << diagnosticsOf(rep);
    auto const h = textHeaderOf(b);
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(h->numberOfRelocations, 0xFFFFu);
    EXPECT_NE(h->characteristics & kNrelocOvfl, 0u) << "IMAGE_SCN_LNK_NRELOC_OVFL";
    EXPECT_EQ(u32(b, h->relocPointer), 65536u) << "the count record counts the relocations and itself";
    EXPECT_EQ(u32(b, h->relocPointer + 4), 0u) << "symbol 0";
    EXPECT_EQ(u16(b, h->relocPointer + 8), 0u) << "type 0, IMAGE_REL_AMD64_ABSOLUTE";
    EXPECT_EQ(u32(b, h->relocPointer + 10), 1u) << "the first RELOCATION follows the count record: the first call's field";
}

// The reader reads every relocation of such a table, the count record skipped — the writer's output read back.
TEST(CoffRelocationCountOverflow, TheReaderReadsEveryRelocationPastTheCountRecord) {
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    for (std::uint32_t const calls : {65535u, 65536u + 70000u}) {
        SCOPED_TRACE(calls);
        DiagnosticReporter wrep;
        auto const         b = writeObject(l, calls, wrep);
        ASSERT_FALSE(b.empty()) << diagnosticsOf(wrep);
        DiagnosticReporter rep;
        auto const         m = read(l, b, rep);
        ASSERT_TRUE(m.has_value()) << "the reader takes the count from the first record:" << diagnosticsOf(rep);
        ASSERT_EQ(m->functions.size(), 1u);
        auto const& rels = m->functions[0].relocations;
        ASSERT_EQ(rels.size(), calls) << "every relocation, none dropped at 65535, the count record not among them";
        EXPECT_EQ(rels.front().offset, 1u);
        EXPECT_EQ(rels.back().offset, (calls - 1u) * 5u + 1u);
    }
}

// The two halves of the convention must agree, and a count record must count at least itself.
TEST(CoffRelocationCountOverflow, AFlagTheCountDoesNotAgreeWithIsRefusedByName) {
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    {
        DiagnosticReporter wrep;
        auto               b = writeObject(l, 1u, wrep);
        ASSERT_FALSE(b.empty()) << diagnosticsOf(wrep);
        auto const h = textHeaderOf(b);
        ASSERT_TRUE(h.has_value());
        put32(b, h->at + 36, h->characteristics | kNrelocOvfl);
        DiagnosticReporter rep;
        EXPECT_FALSE(read(l, b, rep).has_value());
        EXPECT_TRUE(said(rep, "sets IMAGE_SCN_LNK_NRELOC_OVFL beside a NumberOfRelocations of 1")) << diagnosticsOf(rep);
    }
    DiagnosticReporter wrep;
    auto               b = writeObject(l, 65535u, wrep);
    ASSERT_FALSE(b.empty()) << diagnosticsOf(wrep);
    auto const h = textHeaderOf(b);
    ASSERT_TRUE(h.has_value());
    put32(b, h->relocPointer, 0u);
    DiagnosticReporter rep;
    EXPECT_FALSE(read(l, b, rep).has_value());
    EXPECT_TRUE(said(rep, "its count record says 0")) << diagnosticsOf(rep);
}
