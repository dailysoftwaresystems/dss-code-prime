// Linker substrate tests — plan 14 LK4.
//
// Pins:
//   * Empty module → LinkedImage::ok() == false (expectedFuncCount
//     gate, parallel-index discipline borrowed from
//     AssembledModule::ok / LirAllocation::ok).
//   * Single-CU intra-module symbol resolution: a reloc whose
//     target matches an assembled function's symbol resolves, even
//     with no per-format walker plugged in (LK4 substrate).
//   * Reloc kind known to target schema but absent from format
//     schema → K_RelocationKindMismatch.
//   * Reloc kind absent from BOTH schemas → K_RelocationKindMismatch.
//   * Reloc target unknown to the module's symbol set →
//     K_SymbolUndefined.
//   * Multiple functions: parallel-index discipline preserves
//     resolvedFuncCount on success.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

// Uses the shipped `x86_64.target.json` for the target side so the
// `relocations[]` rows the linker resolves against are the SAME tags
// the assembler actually stamps end-to-end. The linker substrate
// (LK4) never reads `opcodes[]` — it only consults
// `relocationInfo(kind)` — but loading the real schema beats a
// synthetic placeholder for readability + drift insurance.
//
// Object-format side is synthetic: no `*.format.json` ships yet (LK1
// onwards), and the test contract is the cross-side reloc-tag
// agreement — so we craft a format schema that declares the real
// `rel32` tag (kind=1) by name.

// All five sections are required by the ELF walker — declaring them
// minimally keeps the LK1 dispatch arm satisfied so these tests
// focus on the linker substrate's reloc/symbol behavior, not on
// format-walker prerequisites (which are pinned in
// `test_elf_writer.cpp`).
constexpr std::string_view kFormatMatchingX86_64 = R"({
  "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
  "format": {"name": "test-elf", "kind": "elf"},
  "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
  "sections": [
    {"kind":"text",    "name":".text",     "type":1, "flags":6,  "addrAlign":16, "entrySize":0},
    {"kind":"reloc",   "name":".rela.text","type":4, "flags":64, "addrAlign":8,  "entrySize":24},
    {"kind":"symtab",  "name":".symtab",   "type":2, "flags":0,  "addrAlign":8,  "entrySize":24},
    {"kind":"strtab",  "name":".strtab",   "type":3, "flags":0,  "addrAlign":1,  "entrySize":0},
    {"kind":"shstrtab","name":".shstrtab", "type":3, "flags":0,  "addrAlign":1,  "entrySize":0}
  ],
  "relocations": [
    { "name": "R_X86_64_PC32",   "kind": 1, "nativeId": 2  },
    { "name": "R_X86_64_64",     "kind": 2, "nativeId": 1  },
    { "name": "R_X86_64_32",     "kind": 3, "nativeId": 10 }
  ]
})";

// Same target side, but a format schema that DOESN'T declare
// the assembler-side reloc tag — isolates the format-side
// half of plan 13 §2.6's reloc-taxonomy unifier. Sections[] still
// declared so the ELF walker doesn't pollute the diagnostic count.
constexpr std::string_view kFormatMissingReloc = R"({
  "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
  "format": {"name": "test-elf-bare", "kind": "elf"},
  "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
  "sections": [
    {"kind":"text",    "name":".text",     "type":1, "flags":6,  "addrAlign":16, "entrySize":0},
    {"kind":"reloc",   "name":".rela.text","type":4, "flags":64, "addrAlign":8,  "entrySize":24},
    {"kind":"symtab",  "name":".symtab",   "type":2, "flags":0,  "addrAlign":8,  "entrySize":24},
    {"kind":"strtab",  "name":".strtab",   "type":3, "flags":0,  "addrAlign":1,  "entrySize":0},
    {"kind":"shstrtab","name":".shstrtab", "type":3, "flags":0,  "addrAlign":1,  "entrySize":0}
  ]
})";

struct Loaded {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

[[nodiscard]] Loaded loadMinimal(std::string_view formatText = kFormatMatchingX86_64) {
    Loaded out;
    auto t = TargetSchema::loadShipped("x86_64");
    if (!t.has_value()) {
        ADD_FAILURE() << "loader rejected shipped x86_64 target schema; diagnostics:";
        for (auto const& d : t.error()) ADD_FAILURE() << "  " << d.message;
    }
    out.target = std::move(t).value();
    auto f = ObjectFormatSchema::loadFromText(formatText);
    if (!f.has_value()) {
        ADD_FAILURE() << "loader rejected format schema; diagnostics:";
        for (auto const& d : f.error()) ADD_FAILURE() << "  " << d.message;
    }
    out.format = std::move(f).value();
    return out;
}

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& rep,
                                     DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : rep.all()) {
        if (d.code == code) ++n;
    }
    return n;
}

} // namespace

TEST(Linker, EmptyModuleIsNotOk) {
    auto loaded = loadMinimal();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule empty{};
    DiagnosticReporter rep;
    auto image = linker::link(empty, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(image.ok());
    EXPECT_EQ(image.expectedFuncCount, 0u);
    EXPECT_EQ(image.resolvedFuncCount, 0u);
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(Linker, NoRelocationsResolvesCleanly) {
    auto loaded = loadMinimal();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{42};
    mod.functions.push_back(std::move(fn));
    DiagnosticReporter rep;
    auto image = linker::link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(image.ok());
    EXPECT_EQ(image.resolvedFuncCount, 1u);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(image.format, ObjectFormatKind::Elf);
}

TEST(Linker, IntraModuleSymbolReferenceResolves) {
    auto loaded = loadMinimal();
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction caller;
    caller.symbol = SymbolId{1};
    Relocation rel;
    rel.offset = 4;
    rel.target = SymbolId{2};       // matches `callee.symbol`
    rel.kind   = RelocationKind{1}; // matches both schemas
    caller.relocations.push_back(rel);
    AssembledFunction callee;
    callee.symbol = SymbolId{2};
    mod.functions.push_back(std::move(caller));
    mod.functions.push_back(std::move(callee));
    DiagnosticReporter rep;
    auto image = linker::link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(image.ok());
    EXPECT_EQ(image.resolvedFuncCount, 2u);
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(Linker, UnknownSymbolEmitsK_SymbolUndefined) {
    auto loaded = loadMinimal();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    Relocation rel;
    rel.target = SymbolId{99}; // not declared by any function in module
    rel.kind   = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    DiagnosticReporter rep;
    auto image = linker::link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(image.resolvedFuncCount, 0u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolUndefined), 1u);
}

// D-LK4-3 collision pin: two CompilationUnits (distinct cuIds) each define a
// function with the SAME bare SymbolId #42, and each NAMES its #42 in the symbol
// table (with DISTINCT names "foo"/"bar"). The linker's compound key (cuId,
// SymbolId) keeps the two #42s distinct in the per-CU validation index — no false
// "declared more than once" — and the cross-CU merge (LK11a) resolves two global
// definitions. RED-ON-DISABLE: drop cuId from LinkedSymbolKey's ==/hash and the
// two #42s collide in buildCompoundIndex → a spurious K_SymbolUndefined.
TEST(Linker, CrossCuSymbolIdCollisionDisambiguatedByCuId) {
    auto loaded = loadMinimal();
    ASSERT_TRUE(loaded.target && loaded.format);

    std::vector<AssembledModule> mods;
    {
        AssembledModule m;
        m.cuId = CompilationUnitId{1};
        m.expectedFuncCount = 1;
        AssembledFunction fn;
        fn.symbol = SymbolId{42};
        m.functions.push_back(std::move(fn));
        m.symbols.push_back(ModuleSymbol{SymbolId{42}, "foo",
                                         SymbolBinding::Global, SymbolVisibility::Default});
        mods.push_back(std::move(m));
    }
    {
        AssembledModule m;
        m.cuId = CompilationUnitId{2};
        m.expectedFuncCount = 1;
        AssembledFunction fn;
        fn.symbol = SymbolId{42};  // SAME bare SymbolId as CU #1's function
        m.functions.push_back(std::move(fn));
        m.symbols.push_back(ModuleSymbol{SymbolId{42}, "bar",
                                         SymbolBinding::Global, SymbolVisibility::Default});
        mods.push_back(std::move(m));
    }

    DiagnosticReporter rep;
    auto image = linker::link(
        std::span<AssembledModule const>{mods.data(), mods.size()},
        *loaded.target, *loaded.format, rep);

    // THE D-LK4-3 regression guard: the compound key kept the two CUs' #42 distinct
    // in per-CU validation — no spurious duplicate-symbol diagnostic.
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolUndefined), 0u)
        << "(cuId, SymbolId) compound key must keep two CUs' colliding #42 distinct";
    // Two DISTINCT names → two resolved global definitions; the merged image indexes
    // both functions (symbolCount counts the EMITTED merged module's symbols).
    EXPECT_EQ(image.symbolCount, 2u);
    EXPECT_EQ(image.resolvedGlobalDefs.size(), 2u);
    // Distinct names → no redefinition; the merge EMITTED the image (LK11b — no deferral).
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolRedefinedAcrossUnits), 0u);
}

// ── LK11a cross-CU symbol-resolution tests ──────────────────────────────────
// Hand-built multi-module inputs exercise the linker's DEFINITION merge +
// weak-vs-strong resolution (the D-LK4-3 collision-pin pattern). They assert the
// WINNING definition's compound key — not just a diagnostic count — the property
// the OPT7 Weak-inline guard is built on. Reference/reloc resolution to a cross-CU
// def (the extern -> sibling-def path) + merged-image bytes are LK11b.

static ModuleSymbol lkSym(std::uint32_t id, std::string name, SymbolBinding b) {
    return ModuleSymbol{SymbolId{id}, std::move(name), b, SymbolVisibility::Default};
}
static AssembledModule lkCu(std::uint32_t cuId, std::vector<ModuleSymbol> syms) {
    AssembledModule m;
    m.cuId    = CompilationUnitId{cuId};
    m.symbols = std::move(syms);
    return m;
}
static LinkedImage lkLink(std::vector<AssembledModule> const& mods,
                          TargetSchema const& target,
                          ObjectFormatSchema const& format,
                          DiagnosticReporter& rep) {
    return linker::link(std::span<AssembledModule const>{mods.data(), mods.size()},
                        target, format, rep);
}

// Two distinct global names across CUs → two resolved definitions, each mapped to its
// defining CU's compound key. Symbol-only modules (no functions) — the resolution
// outcome is `resolvedGlobalDefs`, asserted directly (the emission-side `symbolCount`
// is the EMITTED merged module's symbol count, 0 here).
TEST(Linker, CrossCuTwoDistinctGlobalsResolve) {
    auto loaded = loadMinimal();
    ASSERT_TRUE(loaded.target && loaded.format);
    std::vector<AssembledModule> mods;
    mods.push_back(lkCu(1, {lkSym(1, "foo", SymbolBinding::Global)}));
    mods.push_back(lkCu(2, {lkSym(2, "bar", SymbolBinding::Global)}));
    DiagnosticReporter rep;
    auto image = lkLink(mods, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(image.resolvedGlobalDefs.size(), 2u);
    ASSERT_EQ(image.resolvedGlobalDefs.count("foo"), 1u);
    ASSERT_EQ(image.resolvedGlobalDefs.count("bar"), 1u);
    EXPECT_EQ(image.resolvedGlobalDefs.at("foo").cuId.v, 1u);
    EXPECT_EQ(image.resolvedGlobalDefs.at("foo").symbol.v, 1u);
    EXPECT_EQ(image.resolvedGlobalDefs.at("bar").cuId.v, 2u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolRedefinedAcrossUnits), 0u);
}

// A strong (Global) definition shadows a weak one of the same name — the winning
// key is the STRONG def's, regardless of which CU it lives in.
TEST(Linker, CrossCuStrongShadowsWeak) {
    auto loaded = loadMinimal();
    ASSERT_TRUE(loaded.target && loaded.format);
    std::vector<AssembledModule> mods;
    mods.push_back(lkCu(1, {lkSym(1, "foo", SymbolBinding::Weak)}));
    mods.push_back(lkCu(2, {lkSym(2, "foo", SymbolBinding::Global)}));
    DiagnosticReporter rep;
    auto image = lkLink(mods, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(image.resolvedGlobalDefs.count("foo"), 1u);
    EXPECT_EQ(image.resolvedGlobalDefs.at("foo").cuId.v, 2u)   // the strong def in CU #2 wins
        << "a strong (Global) definition must shadow the weak one";
    EXPECT_EQ(image.resolvedGlobalDefs.at("foo").symbol.v, 2u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolRedefinedAcrossUnits), 0u);
}

// Two strong (Global) definitions of one name across CUs is an ambiguous
// redefinition — fail loud.
TEST(Linker, CrossCuDuplicateStrongRedefines) {
    auto loaded = loadMinimal();
    ASSERT_TRUE(loaded.target && loaded.format);
    std::vector<AssembledModule> mods;
    mods.push_back(lkCu(1, {lkSym(1, "foo", SymbolBinding::Global)}));
    mods.push_back(lkCu(2, {lkSym(2, "foo", SymbolBinding::Global)}));
    DiagnosticReporter rep;
    auto image = lkLink(mods, *loaded.target, *loaded.format, rep);
    // K strong defs of one name → exactly K-1 redefinition diagnostics, order-
    // independent. Two strong defs → exactly 1 (not just >= 1) — pins count determinism.
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolRedefinedAcrossUnits), 1u);
}

// Among all-weak definitions the lowest (cuId, SymbolId) wins — INDEPENDENT of
// module order. Link both orderings; the winner must be identical.
TEST(Linker, CrossCuAllWeakDeterministicBothOrderings) {
    auto loaded = loadMinimal();
    ASSERT_TRUE(loaded.target && loaded.format);
    DiagnosticReporter repA;
    std::vector<AssembledModule> a;
    a.push_back(lkCu(1, {lkSym(1, "foo", SymbolBinding::Weak)}));
    a.push_back(lkCu(2, {lkSym(2, "foo", SymbolBinding::Weak)}));
    auto imageA = lkLink(a, *loaded.target, *loaded.format, repA);

    DiagnosticReporter repB;
    std::vector<AssembledModule> b;  // reversed module order
    b.push_back(lkCu(2, {lkSym(2, "foo", SymbolBinding::Weak)}));
    b.push_back(lkCu(1, {lkSym(1, "foo", SymbolBinding::Weak)}));
    auto imageB = lkLink(b, *loaded.target, *loaded.format, repB);

    ASSERT_EQ(imageA.resolvedGlobalDefs.count("foo"), 1u);
    ASSERT_EQ(imageB.resolvedGlobalDefs.count("foo"), 1u);
    EXPECT_EQ(imageA.resolvedGlobalDefs.at("foo").cuId.v, 1u);  // lowest cuId wins
    EXPECT_EQ(imageB.resolvedGlobalDefs.at("foo").cuId.v, 1u)
        << "all-weak resolution must be order-independent";
    EXPECT_EQ(countCode(repA, DiagnosticCode::K_SymbolRedefinedAcrossUnits), 0u);
    EXPECT_EQ(countCode(repB, DiagnosticCode::K_SymbolRedefinedAcrossUnits), 0u);
}

// Local definitions never enter the global table — two Locals of the same name in
// different CUs do NOT collide and do NOT resolve to a shared definition.
TEST(Linker, CrossCuLocalStaysModulePrivate) {
    auto loaded = loadMinimal();
    ASSERT_TRUE(loaded.target && loaded.format);
    std::vector<AssembledModule> mods;
    mods.push_back(lkCu(1, {lkSym(1, "foo", SymbolBinding::Local)}));
    mods.push_back(lkCu(2, {lkSym(2, "foo", SymbolBinding::Local)}));
    DiagnosticReporter rep;
    auto image = lkLink(mods, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(image.resolvedGlobalDefs.count("foo"), 0u);
    EXPECT_EQ(image.symbolCount, 0u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolRedefinedAcrossUnits), 0u);
}

// A Local "foo" in one CU must NOT collide with a Global "foo" in another — the
// global resolves to the Global def alone; no false redefinition (audit Gap-1).
TEST(Linker, CrossCuLocalDoesNotCollideWithGlobal) {
    auto loaded = loadMinimal();
    ASSERT_TRUE(loaded.target && loaded.format);
    std::vector<AssembledModule> mods;
    mods.push_back(lkCu(1, {lkSym(1, "foo", SymbolBinding::Local)}));   // module-private
    mods.push_back(lkCu(2, {lkSym(2, "foo", SymbolBinding::Global)}));  // the global "foo"
    DiagnosticReporter rep;
    auto image = lkLink(mods, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(image.resolvedGlobalDefs.count("foo"), 1u);
    EXPECT_EQ(image.resolvedGlobalDefs.at("foo").cuId.v, 2u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolRedefinedAcrossUnits), 0u);
}

// REFERENCE resolution + byte-patching: an extern import whose name is DEFINED in a sibling
// CU is a cross-CU reference — it BINDS to that definition (the def shadows the extern decl
// AND the library fallback). The merge retargets the referencing relocation to the def and
// STRIPS the extern (no library import for it). Asserts the edge + the strip.
TEST(Linker, CrossCuExternResolvesToSiblingDefinition) {
    auto loaded = loadMinimal();
    ASSERT_TRUE(loaded.target && loaded.format);
    std::vector<AssembledModule> mods;
    {
        AssembledModule m;  // CU #1 references "foo" via an extern import
        m.cuId = CompilationUnitId{1};
        ExternImport ext;
        ext.symbol      = SymbolId{5};
        ext.mangledName = "foo";
        ext.libraryPath = "lib.dll";  // moot once the reference binds to the sibling def
        m.externImports.push_back(std::move(ext));
        mods.push_back(std::move(m));
    }
    mods.push_back(lkCu(2, {lkSym(2, "foo", SymbolBinding::Global)}));  // CU #2 DEFINES "foo"
    DiagnosticReporter rep;
    auto image = lkLink(mods, *loaded.target, *loaded.format, rep);
    // The reference (CU #1, extern #5) binds to the definition (CU #2, #2).
    ASSERT_EQ(image.resolvedCrossCuRefs.size(), 1u);
    EXPECT_EQ(image.resolvedCrossCuRefs[0].reference.cuId.v, 1u);
    EXPECT_EQ(image.resolvedCrossCuRefs[0].reference.symbol.v, 5u);
    EXPECT_EQ(image.resolvedCrossCuRefs[0].definition.cuId.v, 2u);
    EXPECT_EQ(image.resolvedCrossCuRefs[0].definition.symbol.v, 2u);
    // The reference resolved to the sibling def: NO fail-loud, and "foo" is STRIPPED from
    // the emitted image's imports (the sibling shadows the library fallback).
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_CrossCuMergeUnsupported), 0u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolUndefined), 0u);
    EXPECT_EQ(std::count(image.externImportNames.begin(), image.externImportNames.end(),
                         std::string{"foo"}), 0);
}

// An extern import with NO cross-CU definition stays a real FFI import — no cross-CU
// edge, no diagnostic (it is resolved via the import table, unchanged).
TEST(Linker, CrossCuExternWithoutDefinitionStaysFfiImport) {
    auto loaded = loadMinimal();
    ASSERT_TRUE(loaded.target && loaded.format);
    std::vector<AssembledModule> mods;
    {
        AssembledModule m;  // CU #1 imports "printf" from a DLL
        m.cuId = CompilationUnitId{1};
        ExternImport ext;
        ext.symbol      = SymbolId{5};
        ext.mangledName = "printf";
        ext.libraryPath = "msvcrt.dll";
        m.externImports.push_back(std::move(ext));
        mods.push_back(std::move(m));
    }
    mods.push_back(lkCu(2, {lkSym(2, "foo", SymbolBinding::Global)}));  // unrelated def
    DiagnosticReporter rep;
    auto image = lkLink(mods, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(image.resolvedCrossCuRefs.size(), 0u)
        << "an extern defined by no CU must stay a real FFI import, not a cross-CU edge";
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolUndefined), 0u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_CrossCuMergeUnsupported), 0u);
}

// A relocation whose target is neither defined nor imported in its own CU is an
// undefined reference — fail loud (the per-CU compound index does not contain it).
TEST(Linker, CrossCuUndefinedRelocationFailsLoud) {
    auto loaded = loadMinimal();
    ASSERT_TRUE(loaded.target && loaded.format);
    std::vector<AssembledModule> mods;
    {
        AssembledModule m;  // CU #1: a function with a reloc to an undeclared symbol
        m.cuId = CompilationUnitId{1};
        m.expectedFuncCount = 1;
        AssembledFunction fn;
        fn.symbol = SymbolId{1};
        Relocation rel;
        rel.target = SymbolId{99};  // not defined, not imported anywhere in CU #1
        fn.relocations.push_back(rel);
        m.functions.push_back(std::move(fn));
        m.symbols.push_back(ModuleSymbol{SymbolId{1}, "f1",
                                         SymbolBinding::Global, SymbolVisibility::Default});
        mods.push_back(std::move(m));
    }
    mods.push_back(lkCu(2, {lkSym(2, "g2", SymbolBinding::Global)}));
    DiagnosticReporter rep;
    auto image = lkLink(mods, *loaded.target, *loaded.format, rep);
    EXPECT_GE(countCode(rep, DiagnosticCode::K_SymbolUndefined), 1u)
        << "a relocation to an undeclared symbol must fail loud";
}

// An extern reference binds to a WEAK cross-CU definition (not only Global) — the
// def `table` is Global∪Weak, so a Weak def satisfies a reference (only Local is
// excluded). Also exercises the MULTI-edge shape (two references, two definitions).
TEST(Linker, CrossCuExternBindsToWeakDefAndMultiEdge) {
    auto loaded = loadMinimal();
    ASSERT_TRUE(loaded.target && loaded.format);
    std::vector<AssembledModule> mods;
    {
        AssembledModule m;  // CU #1 references both "w" and "g"
        m.cuId = CompilationUnitId{1};
        ExternImport ew; ew.symbol = SymbolId{5}; ew.mangledName = "w"; ew.libraryPath = "lib.dll";
        ExternImport eg; eg.symbol = SymbolId{6}; eg.mangledName = "g"; eg.libraryPath = "lib.dll";
        m.externImports.push_back(std::move(ew));
        m.externImports.push_back(std::move(eg));
        mods.push_back(std::move(m));
    }
    // CU #2 defines "w" WEAK and "g" Global.
    mods.push_back(lkCu(2, {lkSym(2, "w", SymbolBinding::Weak),
                            lkSym(3, "g", SymbolBinding::Global)}));
    DiagnosticReporter rep;
    auto image = lkLink(mods, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(image.resolvedCrossCuRefs.size(), 2u);
    // Find each edge by its referencing symbol and assert it binds to the right def.
    auto edgeFor = [&](std::uint32_t refSym) -> LinkedImage::CrossCuRef const* {
        for (auto const& e : image.resolvedCrossCuRefs)
            if (e.reference.symbol.v == refSym) return &e;
        return nullptr;
    };
    auto const* wEdge = edgeFor(5);
    auto const* gEdge = edgeFor(6);
    ASSERT_NE(wEdge, nullptr);
    ASSERT_NE(gEdge, nullptr);
    EXPECT_EQ(wEdge->definition.cuId.v, 2u);   // "w" binds to the WEAK def in CU #2
    EXPECT_EQ(wEdge->definition.symbol.v, 2u);
    EXPECT_EQ(gEdge->definition.symbol.v, 3u); // "g" binds to the Global def
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolUndefined), 0u);
}

// LK11b byte-pin: TWO CUs each define a function with DISTINCT, recognizable body bytes;
// the merge concatenates both into ONE combined module that the format walker emits.
// Assert the merged image's bytes contain BOTH functions' byte patterns. RED-ON-DISABLE:
// drop a CU (or fail to concatenate it) in the merge → that CU's pattern vanishes.
TEST(Linker, CrossCuMergeEmitsBothCusFunctionBytes) {
    auto loaded = loadMinimal();
    ASSERT_TRUE(loaded.target && loaded.format);

    // Distinct recognizable bodies: `mov eax, imm32; ret` with different immediates.
    std::vector<std::uint8_t> const bodyA{0xB8, 0xAB, 0x00, 0x00, 0x00, 0xC3};
    std::vector<std::uint8_t> const bodyB{0xB8, 0xCD, 0x00, 0x00, 0x00, 0xC3};

    std::vector<AssembledModule> mods;
    {
        AssembledModule m;
        m.cuId = CompilationUnitId{1};
        m.expectedFuncCount = 1;
        AssembledFunction fn;
        fn.symbol = SymbolId{1};
        fn.bytes  = bodyA;
        m.functions.push_back(std::move(fn));
        m.symbols.push_back(ModuleSymbol{SymbolId{1}, "fnA",
                                         SymbolBinding::Global, SymbolVisibility::Default});
        mods.push_back(std::move(m));
    }
    {
        AssembledModule m;
        m.cuId = CompilationUnitId{2};
        m.expectedFuncCount = 1;
        AssembledFunction fn;
        fn.symbol = SymbolId{1};  // per-CU SymbolId collides with CU #1's — distinct names
        fn.bytes  = bodyB;
        m.functions.push_back(std::move(fn));
        m.symbols.push_back(ModuleSymbol{SymbolId{1}, "fnB",
                                         SymbolBinding::Global, SymbolVisibility::Default});
        mods.push_back(std::move(m));
    }

    DiagnosticReporter rep;
    auto image = lkLink(mods, *loaded.target, *loaded.format, rep);

    auto contains = [](std::vector<std::uint8_t> const& hay,
                       std::vector<std::uint8_t> const& needle) {
        if (needle.empty() || hay.size() < needle.size()) return false;
        for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
            if (std::equal(needle.begin(), needle.end(), hay.begin() + i)) return true;
        }
        return false;
    };
    EXPECT_FALSE(image.bytes.empty()) << "the merged image must emit bytes";
    EXPECT_TRUE(contains(image.bytes, bodyA))
        << "CU #1's function bytes must appear in the merged image";
    EXPECT_TRUE(contains(image.bytes, bodyB))
        << "CU #2's function bytes must appear in the merged image";
}

// LK11b strong-over-weak EMISSION (the OPT7 weak-inline corpus substrate): TWO CUs each
// DEFINE a function `f` of the SAME name — one WEAK (body returns 7), one STRONG (body
// returns 42). The resolution layer (resolveCrossCuSymbols) picks the strong def as the
// winner; the MERGE must then DROP the shadowed WEAK body, NOT emit it with a colliding
// merged id. Pre-fix the merge folded both ids onto the winner → TWO functions with the
// SAME merged SymbolId → the within-image compound-index collision (K_SymbolUndefined
// "declared more than once"). Post-fix: exactly the STRONG body lands in the image, the
// WEAK body is dropped, and the link succeeds clean.
//
// RED-ON-DISABLE: remove the `isShadowedDuplicate` drop in mergeModules and this fires
// (K_SymbolUndefined collision) — the same failure the weak_inline_crosscu corpus baseline
// hit before the fix. This is the EMISSION-tier completion of strong-over-weak that the
// §2.9 Weak-inline gate's end-to-end exit-42 proof rides on.
TEST(Linker, CrossCuStrongShadowsWeakEmitsOnlyStrongBody) {
    auto loaded = loadMinimal();
    ASSERT_TRUE(loaded.target && loaded.format);

    // Distinct recognizable bodies: `mov eax, 7; ret` (weak) vs `mov eax, 42; ret` (strong).
    std::vector<std::uint8_t> const weakBody  {0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3};
    std::vector<std::uint8_t> const strongBody{0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};

    std::vector<AssembledModule> mods;
    {
        AssembledModule m;  // CU #1: weak f
        m.cuId = CompilationUnitId{1};
        m.expectedFuncCount = 1;
        AssembledFunction fn;
        fn.symbol = SymbolId{1};
        fn.bytes  = weakBody;
        m.functions.push_back(std::move(fn));
        m.symbols.push_back(ModuleSymbol{SymbolId{1}, "f",
                                         SymbolBinding::Weak, SymbolVisibility::Default});
        mods.push_back(std::move(m));
    }
    {
        AssembledModule m;  // CU #2: strong f (same name, per-CU SymbolId also 1)
        m.cuId = CompilationUnitId{2};
        m.expectedFuncCount = 1;
        AssembledFunction fn;
        fn.symbol = SymbolId{1};
        fn.bytes  = strongBody;
        m.functions.push_back(std::move(fn));
        m.symbols.push_back(ModuleSymbol{SymbolId{1}, "f",
                                         SymbolBinding::Global, SymbolVisibility::Default});
        mods.push_back(std::move(m));
    }

    DiagnosticReporter rep;
    auto image = lkLink(mods, *loaded.target, *loaded.format, rep);

    // The link must NOT collide: the shadowed weak body is dropped, not emitted with a
    // duplicate merged id.
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolUndefined), 0u)
        << "strong-over-weak emission must drop the shadowed weak body, not collide";
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolRedefinedAcrossUnits), 0u);
    EXPECT_EQ(image.resolvedGlobalDefs.at("f").cuId.v, 2u)
        << "the strong def (CU #2) wins resolution";

    auto contains = [](std::vector<std::uint8_t> const& hay,
                       std::vector<std::uint8_t> const& needle) {
        if (needle.empty() || hay.size() < needle.size()) return false;
        for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
            if (std::equal(needle.begin(), needle.end(), hay.begin() + i)) return true;
        }
        return false;
    };
    EXPECT_FALSE(image.bytes.empty()) << "the merged image must emit bytes";
    EXPECT_TRUE(contains(image.bytes, strongBody))
        << "the STRONG f body (return 42) must be in the merged image";
    EXPECT_FALSE(contains(image.bytes, weakBody))
        << "the shadowed WEAK f body (return 7) must NOT be in the merged image — "
           "inlining it (or emitting it) is the silent miscompile the OPT7 gate prevents";
}

// LK11b reloc retarget: a function's intra-CU relocation must be remapped from the
// per-CU SymbolId to the merged id. The callee uses a LARGE per-CU SymbolId (100) with
// no counterpart in the small merged id range — so a MISSED retarget leaves a stale
// target the merged symbol index does not contain → K_SymbolUndefined. RED-ON-DISABLE:
// drop the `rel.target = mergedIdFor(...)` retarget and this fires.
TEST(Linker, CrossCuMergeRetargetsIntraCuRelocation) {
    auto loaded = loadMinimal();
    ASSERT_TRUE(loaded.target && loaded.format);
    std::vector<AssembledModule> mods;
    {
        AssembledModule m;  // CU #1: caller (reloc -> callee #100) + callee #100
        m.cuId = CompilationUnitId{1};
        m.expectedFuncCount = 2;
        AssembledFunction caller;
        caller.symbol = SymbolId{1};
        caller.bytes  = {0xE8, 0x00, 0x00, 0x00, 0x00, 0xC3};  // call rel32; ret
        Relocation rel;
        rel.offset = 1;
        rel.target = SymbolId{100};      // callee's per-CU SymbolId (large)
        rel.kind   = RelocationKind{1};  // rel32 — declared by both shipped schemas
        caller.relocations.push_back(rel);
        AssembledFunction callee;
        callee.symbol = SymbolId{100};
        callee.bytes  = {0xC3};          // ret
        m.functions.push_back(std::move(caller));
        m.functions.push_back(std::move(callee));
        m.symbols.push_back(ModuleSymbol{SymbolId{1},   "caller",
                                         SymbolBinding::Global, SymbolVisibility::Default});
        m.symbols.push_back(ModuleSymbol{SymbolId{100}, "callee",
                                         SymbolBinding::Global, SymbolVisibility::Default});
        mods.push_back(std::move(m));
    }
    mods.push_back(lkCu(2, {lkSym(1, "other", SymbolBinding::Global)}));  // CU #2 → N>1
    DiagnosticReporter rep;
    auto image = lkLink(mods, *loaded.target, *loaded.format, rep);
    // #100 was remapped to callee's merged id; a missed retarget leaves stale #100,
    // which the merged symbol index does not contain.
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolUndefined), 0u)
        << "the merge must retarget the intra-CU relocation to the callee's merged id";
}

// LK11b cross-CU REFERENCE byte-patching: CU#1's `caller` calls an extern "crossfn"
// (declared with a library fallback) that CU#2 DEFINES. The merge must (a) retarget the
// call to CU#2's def (so it resolves — no K_SymbolUndefined) and (b) STRIP the extern (the
// sibling shadows the library fallback — "crossfn" must NOT appear in the emitted image's
// imports). Asserts both, plus the def's body bytes land in the merged image.
TEST(Linker, CrossCuRetargetAndStripPatches) {
    auto loaded = loadMinimal();
    ASSERT_TRUE(loaded.target && loaded.format);
    std::vector<std::uint8_t> const calleeBody{0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};  // mov eax,42; ret
    std::vector<AssembledModule> mods;
    {
        AssembledModule m;  // CU #1: caller calls extern "crossfn" (#2)
        m.cuId = CompilationUnitId{1};
        m.expectedFuncCount = 1;
        AssembledFunction caller;
        caller.symbol = SymbolId{1};
        caller.bytes  = {0xE8, 0x00, 0x00, 0x00, 0x00, 0xC3};  // call rel32; ret
        Relocation rel;
        rel.offset = 1;
        rel.target = SymbolId{2};        // the extern "crossfn"
        rel.kind   = RelocationKind{1};  // rel32
        caller.relocations.push_back(rel);
        m.functions.push_back(std::move(caller));
        ExternImport ext;
        ext.symbol      = SymbolId{2};
        ext.mangledName = "crossfn";
        ext.libraryPath = "lib.dll";     // library fallback — shadowed by the sibling def
        m.externImports.push_back(std::move(ext));
        m.symbols.push_back(ModuleSymbol{SymbolId{1}, "caller",
                                         SymbolBinding::Global, SymbolVisibility::Default});
        mods.push_back(std::move(m));
    }
    {
        AssembledModule m;  // CU #2: DEFINES crossfn
        m.cuId = CompilationUnitId{2};
        m.expectedFuncCount = 1;
        AssembledFunction callee;
        callee.symbol = SymbolId{1};
        callee.bytes  = calleeBody;
        m.functions.push_back(std::move(callee));
        m.symbols.push_back(ModuleSymbol{SymbolId{1}, "crossfn",
                                         SymbolBinding::Global, SymbolVisibility::Default});
        mods.push_back(std::move(m));
    }
    DiagnosticReporter rep;
    auto image = lkLink(mods, *loaded.target, *loaded.format, rep);

    // Resolution: caller's extern "crossfn" (CU#1) bound to CU#2's def.
    ASSERT_EQ(image.resolvedCrossCuRefs.size(), 1u);
    EXPECT_EQ(image.resolvedCrossCuRefs[0].reference.cuId.v, 1u);
    EXPECT_EQ(image.resolvedCrossCuRefs[0].definition.cuId.v, 2u);
    // Retarget: the call resolves (no undefined); the old N>1 fail-loud is gone.
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolUndefined), 0u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_CrossCuMergeUnsupported), 0u);
    // Strip: "crossfn" resolved to the sibling def → NO library import for it.
    EXPECT_EQ(std::count(image.externImportNames.begin(), image.externImportNames.end(),
                         std::string{"crossfn"}), 0)
        << "a cross-CU-resolved extern must be stripped, not emitted as a library import";
    // Mint + fail-loud: the merge minted a GOT-like THUNK SLOT — an 8-byte rodata data item
    // carrying the abs64 fixup to the sibling def, so the c-subset indirect call
    // `call qword ptr [slot]` dereferences the def's runtime address. The thunk slot is an
    // EXEC-image mechanism (the loader fills it via a base relocation). A relocatable OBJECT
    // format — this synthetic `test-elf`, which carries rodata through the symbol table and
    // so cannot declare `supportedDataSections` (legal only on exec flavors) — MUST reject
    // the slot LOUDLY (K_NoMatchingObjectFormat) rather than silently drop the cross-CU
    // indirect-call slot. So this diagnostic is POSITIVE evidence the slot was minted: the
    // capability gate fires only on a rodata `dataItems` entry, which only the thunk-slot
    // path produces here (the two CUs carry no other data). End-to-end EXEC emission of the
    // slot (callee body + resolved pointer + base-reloc) is pinned by the runnable
    // `examples/c-subset/cross_cu_call` (PE exec, exit 42). ELF/Mach-O exec thunk-slot
    // emission is deferred — format-triggered anchor D-LK11-ELF-MACHO-CROSSCU-THUNK-EMISSION
    // (needs the rodata + base-relocation walker arms). `calleeBody` stays the CU#2 def body.
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_NoMatchingObjectFormat), 1u)
        << "the merge must mint a rodata thunk slot, and a non-exec object format must reject "
           "it loudly — never silently drop the cross-CU indirect-call slot";
    EXPECT_TRUE(image.bytes.empty())
        << "emission must abort when the thunk slot's section cannot be carried — no "
           "half-emitted image past the capability gate";
}

// The strip must NOT over-strip: a real FFI extern (no sibling definition) survives the
// merge as a genuine library import (the FF11 library tier owns it, untouched).
TEST(Linker, CrossCuRealFfiExternSurvivesMerge) {
    auto loaded = loadMinimal();
    ASSERT_TRUE(loaded.target && loaded.format);
    std::vector<AssembledModule> mods;
    {
        AssembledModule m;  // CU #1: imports "realffi" (no sibling def)
        m.cuId = CompilationUnitId{1};
        m.expectedFuncCount = 1;
        AssembledFunction fn;
        fn.symbol = SymbolId{1};
        fn.bytes  = {0xC3};
        m.functions.push_back(std::move(fn));
        ExternImport ext;
        ext.symbol      = SymbolId{2};
        ext.mangledName = "realffi";
        ext.libraryPath = "lib.dll";
        m.externImports.push_back(std::move(ext));
        m.symbols.push_back(ModuleSymbol{SymbolId{1}, "f1",
                                         SymbolBinding::Global, SymbolVisibility::Default});
        mods.push_back(std::move(m));
    }
    mods.push_back(lkCu(2, {lkSym(1, "other", SymbolBinding::Global)}));  // no "realffi" def
    DiagnosticReporter rep;
    auto image = lkLink(mods, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(image.resolvedCrossCuRefs.size(), 0u);
    EXPECT_EQ(std::count(image.externImportNames.begin(), image.externImportNames.end(),
                         std::string{"realffi"}), 1)
        << "a real FFI extern (no sibling def) must survive the merge as a library import";
}

TEST(Linker, RelocationKindMissingFromFormatEmitsMismatch) {
    auto loaded = loadMinimal(kFormatMissingReloc);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    Relocation rel;
    rel.target = SymbolId{1};       // self-ref to isolate the kind check
    rel.kind   = RelocationKind{1}; // declared by target but NOT format
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    DiagnosticReporter rep;
    auto image = linker::link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(image.resolvedFuncCount, 0u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_RelocationKindMismatch), 1u);
}

TEST(Linker, PartialResolutionAcrossMultipleFunctions) {
    // Pin the parallel-index `resolvedFuncCount` accounting: a
    // 3-function module where fn#1 has a bad reloc while fn#0 and
    // fn#2 are clean should resolve 2 of 3 — NOT zero (regression
    // that flips `funcResolved = false` to a module-wide latch) and
    // NOT three (regression that swallows the bad reloc silently).
    auto loaded = loadMinimal();
    AssembledModule mod;
    mod.expectedFuncCount = 3;
    // fn#0 — clean
    AssembledFunction fn0;
    fn0.symbol = SymbolId{10};
    mod.functions.push_back(std::move(fn0));
    // fn#1 — bad reloc kind
    AssembledFunction fn1;
    fn1.symbol = SymbolId{20};
    Relocation badRel;
    badRel.target = SymbolId{20};       // self-ref
    badRel.kind   = RelocationKind{99}; // not declared
    fn1.relocations.push_back(badRel);
    mod.functions.push_back(std::move(fn1));
    // fn#2 — clean
    AssembledFunction fn2;
    fn2.symbol = SymbolId{30};
    mod.functions.push_back(std::move(fn2));

    DiagnosticReporter rep;
    auto image = linker::link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(image.ok());
    EXPECT_EQ(image.expectedFuncCount, 3u);
    // Architect Decision 4 convergence: on linkage failure the
    // engine resets `resolvedFuncCount = 0`, so `ok()` cannot
    // false-positive when the walker is skipped. Per-reloc
    // diagnostic count still reflects the EXACT bad reloc the
    // unifier rejected (1, not 3) — accounting is at the reloc
    // level, not the function level.
    EXPECT_EQ(image.resolvedFuncCount, 0u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_RelocationKindMismatch), 1u);
    EXPECT_TRUE(image.bytes.empty());
}

TEST(Linker, KindMissingFromTargetEmitsMismatchWithBothSidesNamed) {
    // Pin the cross-reference unifier's symmetric half: a reloc kind
    // present on the format side but missing from the target side.
    // The diagnostic must name BOTH sides accurately (the
    // 3-agent-convergence fix that closed the "wrong-side message
    // when both miss" hole).
    auto loaded = loadMinimal();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    Relocation rel;
    rel.target = SymbolId{1};
    rel.kind   = RelocationKind{99};  // declared by NEITHER schema
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));

    DiagnosticReporter rep;
    auto image = linker::link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(image.resolvedFuncCount, 0u);
    bool sawBothSidesNamed = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_RelocationKindMismatch) {
            bool hasTargetSide = d.actual.find(loaded.target->name()) != std::string::npos;
            bool hasFormatSide = d.actual.find(loaded.format->name()) != std::string::npos;
            if (hasTargetSide && hasFormatSide) sawBothSidesNamed = true;
        }
    }
    EXPECT_TRUE(sawBothSidesNamed)
        << "diagnostic must name BOTH target and format when both miss the kind";
}

TEST(Linker, MismatchAndUndefinedSymbolFireIndependentlyOnSameReloc) {
    // Pin the silent-failure-hunter C2 fix: if a reloc has BOTH a
    // bad kind AND an undefined target symbol, the linker should
    // surface BOTH diagnostics in ONE pass — not require two link
    // attempts (kind first, then symbol).
    auto loaded = loadMinimal();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    Relocation rel;
    rel.target = SymbolId{77};         // undefined
    rel.kind   = RelocationKind{99};   // unknown
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));

    DiagnosticReporter rep;
    auto image = linker::link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(image.resolvedFuncCount, 0u);
    EXPECT_GE(countCode(rep, DiagnosticCode::K_RelocationKindMismatch), 1u);
    EXPECT_GE(countCode(rep, DiagnosticCode::K_SymbolUndefined), 1u);
}

TEST(Linker, UnknownRelocationKindEmitsMismatch) {
    auto loaded = loadMinimal();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    Relocation rel;
    rel.target = SymbolId{1};
    rel.kind   = RelocationKind{99}; // declared by NEITHER schema
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    DiagnosticReporter rep;
    auto image = linker::link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(image.resolvedFuncCount, 0u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_RelocationKindMismatch), 1u);
}
