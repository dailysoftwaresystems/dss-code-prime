// P69 fold f26b (lane `cs`) — A WEAK ALIAS READ FROM A FOREIGN COFF OBJECT IS WRITTEN BACK AS ITS COMPILER WROTE IT.
//
// clang-cl writes `m`, an overridable weak alias of the select-any `w` (`__declspec(selectany) int w = 9; extern int m
// __attribute__((weak, alias("w")));`, unit b of `tests/link/data/coff_carried_alias.source.c` — lane `cs`'s probe
// wk1, cell 3), as a WEAK EXTERNAL whose default is `w`'s own EXTERNAL record in its COMDAT section: a NAME that
// outlives a discarded copy and resolves to the copy the link keeps (✔MEASURED, lane `cs`'s probe wk1, and lane
// `xa`'s run of these very objects beside a cl `main`: link.exe and lld-link read 7 in unit order a b and 9 in b a).
// The PE object writer picks an alias's record by what the alias STATES (`appendAliasEntries`, src/link/format/pe.cpp):
// a weak alias stating a kind the document writes as a weak external — PE's overridable kind — takes the
// weak-external arm beside a select-any body too. Chosen by binding alone (the alias's, weak, equal to the select-any
// body's), it would be a second LABEL of `w`'s COMDAT section — MinGW gcc's shape — which goes with a discarded copy,
// and a DSS relocatable link of clang-cl's object would turn the references' 7 into "undefined" (lane `xa`, INFERRED
// from reading; this file is the measurement).
//
// The two halves, both in process on every leg (the fixture is read, never compiled):
//   * THE PREMISE — what DSS's COFF reader states of clang-cl's `m`: a weak alias of `w`'s body, of the OVERRIDABLE
//     kind, and `w` the select-any definition. The writer's choice rests on it.
//   * THE PIN — clang-cl's object alone through DSS's relocatable link (`-r`), read back as bytes: `m` is a weak
//     external (class 105, UNDEF, value 0, one auxiliary record) whose TagIndex names `w`'s external record in a COMDAT
//     section. The default named is the CANONICAL record; a default other than the canonical (MinGW gcc's
//     `.weak.m.*` label) is lane `xa`'s, after its fold 3d.
//
// The fixture (`tests/link/data/coff_carried_alias_b_clangcl_O2.obj`, clang-cl 19.1.5 /O2 /GS-) is lane `xa`'s, the
// same bytes its fold checks in; its source file says how it was built.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "link_test_support.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using link_format::test::readU16LE;
using link_format::test::readU32LE;

namespace {

constexpr char const* kFixture = "coff_carried_alias_b_clangcl_O2.obj";

constexpr std::uint8_t  kClassExternal      = 2;
constexpr std::uint8_t  kClassWeakExternal  = 105;
constexpr std::uint32_t kScnLnkComdat       = 0x00001000u;
constexpr std::uint32_t kWeakExternSearchAlias = 3;

struct Loaded {
    std::shared_ptr<TargetSchema const>       target;
    std::shared_ptr<ObjectFormatSchema const> format;
};

[[nodiscard]] Loaded loadPe() {
    Loaded l;
    auto   t = TargetSchema::loadShipped("x86_64");
    auto   f = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    EXPECT_TRUE(t.has_value() && f.has_value()) << "the shipped x86_64 target and pe64 relocatable document load";
    if (t.has_value()) l.target = *t;
    if (f.has_value()) l.format = *f;
    return l;
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s;
}

[[nodiscard]] std::vector<std::uint8_t> fixture() {
    auto const    path = dss::test::repoRoot() / "tests" / "link" / "data" / kFixture;
    std::ifstream in{path, std::ios::binary};
    if (!in.good()) {
        ADD_FAILURE() << "cannot open " << path.string();
        return {};
    }
    std::string const raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    return std::vector<std::uint8_t>(raw.begin(), raw.end());
}

// ── One COFF object's symbol table, read off the bytes (PE/COFF 5.4) ──────────────────────────────────────────────
struct CoffRecord {
    std::uint32_t index         = 0;   // the record's symbol-table index
    std::string   name;
    std::uint32_t value         = 0;
    std::int16_t  sectionNumber = 0;
    std::uint8_t  storageClass  = 0;
    std::uint8_t  auxCount      = 0;
    std::size_t   offset        = 0;   // of the record in the object
};

[[nodiscard]] std::vector<CoffRecord> coffRecords(std::span<std::uint8_t const> b) {
    std::vector<CoffRecord> out;
    if (b.size() < 20) return out;
    std::uint32_t const table = readU32LE(b, 8);
    std::uint32_t const count = readU32LE(b, 12);
    std::size_t const   strings = static_cast<std::size_t>(table) + static_cast<std::size_t>(count) * 18u;
    if (strings > b.size()) return out;
    for (std::uint32_t i = 0; i < count;) {
        std::size_t const at = table + static_cast<std::size_t>(i) * 18u;
        CoffRecord        r;
        r.index  = i;
        r.offset = at;
        if (readU32LE(b, at) == 0u) {
            std::size_t p = strings + readU32LE(b, at + 4);
            while (p < b.size() && b[p] != 0) r.name.push_back(static_cast<char>(b[p++]));
        } else {
            for (std::size_t k = 0; k < 8 && b[at + k] != 0; ++k) r.name.push_back(static_cast<char>(b[at + k]));
        }
        r.value         = readU32LE(b, at + 8);
        r.sectionNumber = static_cast<std::int16_t>(readU16LE(b, at + 12));
        r.storageClass  = b[at + 16];
        r.auxCount      = b[at + 17];
        out.push_back(r);
        i += 1u + r.auxCount;
    }
    return out;
}

[[nodiscard]] std::uint32_t sectionCharacteristics(std::span<std::uint8_t const> b, std::int16_t sectionNumber) {
    std::size_t const headers = 20u + readU16LE(b, 16);
    return readU32LE(b, headers + (static_cast<std::size_t>(sectionNumber) - 1u) * 40u + 36u);
}

// `name` in `b` is a weak external whose default is the record named `defaultName`, an external definition in a
// COMDAT section: every clause of PE/COFF 5.5.3's shape, and what its TagIndex names.
void expectAWeakExternalDefaultingTo(std::span<std::uint8_t const> b, std::string const& name,
                                     std::string const& defaultName, char const* whose) {
    SCOPED_TRACE(whose);
    auto const              records = coffRecords(b);
    CoffRecord const*       weak    = nullptr;
    for (auto const& r : records) {
        if (r.name == name) {
            ASSERT_EQ(weak, nullptr) << "'" << name << "' is ONE record";
            weak = &r;
        }
    }
    ASSERT_NE(weak, nullptr) << "'" << name << "' is a record of the object";
    EXPECT_EQ(weak->storageClass, kClassWeakExternal)
        << "'" << name << "' is a WEAK EXTERNAL: a label of the copy's section here goes with a discarded copy";
    EXPECT_EQ(weak->sectionNumber, 0) << "5.5.3: a weak external is UNDEF -- it defines nothing itself";
    EXPECT_EQ(weak->value, 0u) << "5.5.3: and its value is zero";
    ASSERT_EQ(weak->auxCount, 1u) << "without its auxiliary record the name defers to nothing";
    std::uint32_t const tag = readU32LE(b, weak->offset + 18u);
    CoffRecord const*   def = nullptr;
    for (auto const& r : records) {
        if (r.index == tag) def = &r;
    }
    ASSERT_NE(def, nullptr) << "the TagIndex " << tag << " names a record of the object";
    EXPECT_EQ(def->name, defaultName) << "the weak external's default is the select-any body's own record";
    EXPECT_EQ(def->storageClass, kClassExternal) << "the default is an EXTERNAL name (link.exe requires one: LNK1235)";
    ASSERT_GT(def->sectionNumber, 0) << "the default is a definition";
    EXPECT_EQ(sectionCharacteristics(b, def->sectionNumber) & kScnLnkComdat, kScnLnkComdat)
        << "in the select-any definition's COMDAT section";
}

[[nodiscard]] ModuleSymbol const* rowNamed(AssembledModule const& m, std::string_view name) {
    for (auto const& s : m.symbols) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

}  // namespace

// THE PREMISE: what DSS's COFF reader states of clang-cl's `m` — the statement the writer's choice reads.
TEST(ForeignPeWeakAlias, ClangClsWeakAliasOfASelectAnyCopyReadsAsAnOverridableNameOfItsBody) {
    auto const L = loadPe();
    ASSERT_TRUE(L.target && L.format);
    auto const bytes = fixture();
    ASSERT_FALSE(bytes.empty());
    // The control: clang-cl's own object IS the shape the pin asks DSS to keep.
    expectAWeakExternalDefaultingTo(bytes, "m", "w", "clang-cl's object");
    DiagnosticReporter rep;
    auto const         read = pe::readRelocatableObject(bytes, *L.target, *L.format, rep, CompilationUnitId{1});
    ASSERT_TRUE(read.has_value()) << diagnosticsOf(rep);
    EXPECT_EQ(rep.errorCount(), 0u) << diagnosticsOf(rep);
    ModuleSymbol const* const m = rowNamed(*read, "m");
    ModuleSymbol const* const w = rowNamed(*read, "w");
    ASSERT_TRUE(m != nullptr && w != nullptr) << "both names are rows of the read object";
    EXPECT_EQ(m->symbol, w->symbol) << "two names of ONE body";
    EXPECT_EQ(w->binding, SymbolBinding::Weak);
    EXPECT_EQ(w->weakKind, std::optional<WeakDefinitionKind>{WeakDefinitionKind::SelectAny})
        << "`w` is the select-any definition (its COMDAT section)";
    EXPECT_EQ(m->binding, SymbolBinding::Weak);
    EXPECT_EQ(m->weakKind, std::optional<WeakDefinitionKind>{WeakDefinitionKind::Overridable})
        << "`m` is a weak external whose default is a body of its object: the OVERRIDABLE kind";
}

// THE PIN: clang-cl's object alone through DSS's relocatable link, read back as bytes.
TEST(ForeignPeWeakAlias, ARelocatableLinkWritesItBackAWeakExternalOfItsSelectAnyBody) {
    auto const L = loadPe();
    ASSERT_TRUE(L.target && L.format);
    auto const bytes = fixture();
    ASSERT_FALSE(bytes.empty());
    DiagnosticReporter rep;
    auto read = pe::readRelocatableObject(bytes, *L.target, *L.format, rep, CompilationUnitId{1});
    ASSERT_TRUE(read.has_value()) << diagnosticsOf(rep);
    std::vector<AssembledModule> const mods{std::move(*read)};
    DiagnosticReporter                 linkRep;
    auto const out = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format, linkRep);
    ASSERT_TRUE(out.ok()) << "DSS's relocatable link of clang-cl's object" << diagnosticsOf(linkRep);
    EXPECT_EQ(linkRep.errorCount(), 0u) << diagnosticsOf(linkRep);
    expectAWeakExternalDefaultingTo(out.bytes, "m", "w", "DSS's relocatable link of it");
    // DSS's writer states SEARCH_ALIAS, the only value under which a foreign linker resolves the weak name from
    // another object (the measurement at the writer's constant).
    for (auto const& r : coffRecords(out.bytes)) {
        if (r.name == "m" && r.auxCount == 1u) {
            EXPECT_EQ(readU32LE(out.bytes, r.offset + 18u + 4u), kWeakExternSearchAlias);
        }
    }
    // And the artifact reads back as the object it was read from: `m` a weak name of the overridable kind on `w`'s
    // body.
    DiagnosticReporter backRep;
    auto const         back = pe::readRelocatableObject(out.bytes, *L.target, *L.format, backRep, CompilationUnitId{1});
    ASSERT_TRUE(back.has_value()) << diagnosticsOf(backRep);
    ModuleSymbol const* const m = rowNamed(*back, "m");
    ModuleSymbol const* const w = rowNamed(*back, "w");
    ASSERT_TRUE(m != nullptr && w != nullptr) << "both names survive the relocatable link";
    EXPECT_EQ(m->symbol, w->symbol) << "two names of ONE body";
    EXPECT_EQ(m->binding, SymbolBinding::Weak);
    EXPECT_EQ(m->weakKind, std::optional<WeakDefinitionKind>{WeakDefinitionKind::Overridable});
}
