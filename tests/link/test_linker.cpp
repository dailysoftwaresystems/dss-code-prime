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

TEST(Linker, EmptyModuleIsOk) {
    // D-CSUBSET-TESTTU-SILENT-EXIT1: an empty module (a declaration-only /
    // all-preprocessed-out TU) links cleanly to a valid empty relocatable
    // object — resolvedFuncCount == expectedFuncCount == 0 is a VALID success.
    // RED-ON-DISABLE: restoring the `expectedFuncCount > 0` clause in
    // LinkedImage::ok() flips this back to false and silently rejects the
    // whole compile of any declaration-only TU.
    auto loaded = loadMinimal();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule empty{};
    DiagnosticReporter rep;
    auto image = linker::link(empty, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(image.ok());
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

// ── D-LK-OBJECT-NOLIB-EXTERN-RELOCATABLE ─────────────────────────

TEST(Linker, RelocatableKeepsReferencedNoLibraryExternAsUndefined) {
    // A bare-prototype extern (the `SQLITE_API` shape: `int sqlite3_foo(...);`,
    // no `extern`, no import library) that is REFERENCED must NOT be rejected
    // when the output is a RELOCATABLE object — it is a legal SHN_UNDEF symbol
    // the final (foreign) linker resolves. RED if the reject re-fires for a .o.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(fmt.has_value());
    ASSERT_FALSE((*fmt)->isImageFlavor()) << "elf64-x86_64-linux is relocatable";
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0x00, 0x00, 0x00, 0x00};   // call rel32 → the extern
    fn.relocations.push_back(
        Relocation{1u, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    ExternImport imp;
    imp.symbol      = SymbolId{99};
    imp.mangledName = "sqlite3_foo";              // libraryPath stays EMPTY
    mod.externImports.push_back(std::move(imp));
    DiagnosticReporter rep;
    auto image = linker::link(mod, **target, **fmt, rep);
    EXPECT_EQ(rep.errorCount(), 0u)
        << "a referenced no-library extern must NOT be rejected for a "
           "relocatable object (D-LK-OBJECT-NOLIB-EXTERN-RELOCATABLE)";
    EXPECT_TRUE(image.ok());
    EXPECT_FALSE(image.bytes.empty());
}

TEST(Linker, RelocatableKeepsReferencedDataExternAsUndefined) {
    // D-LK-OBJECT-DATA-EXTERN-RELOCATABLE (c144): a library DATA extern (the
    // `stdout` shape — `extern FILE *stdout;` from libc, `isData`) that is
    // REFERENCED must NOT be rejected when the output is a RELOCATABLE object.
    // The relocatable format declares no `dataImportBinding` — but a `.o` does
    // not bind imports; it emits the data extern as an SHN_UNDEF symbol the
    // final (foreign) linker resolves by copy-relocation. Only an IMAGE with no
    // binding rejects. RED if the reject re-fires for a .o (revert the
    // isImageFlavor gate on the data-import reject).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(fmt.has_value());
    ASSERT_FALSE((*fmt)->isImageFlavor()) << "elf64-x86_64-linux is relocatable";
    ASSERT_FALSE((*fmt)->dataImportBinding().has_value())
        << "the relocatable ELF format declares no dataImportBinding — the "
           "exact condition the reject used to fire on";
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x48, 0x8B, 0x05, 0, 0, 0, 0};   // mov rax,[rip+stdout]
    fn.relocations.push_back(
        Relocation{3u, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    ExternImport imp;
    imp.symbol      = SymbolId{99};
    imp.mangledName = "stdout";
    imp.libraryPath = "libc.so.6";
    imp.isData      = true;                        // the DATA extern
    mod.externImports.push_back(std::move(imp));
    DiagnosticReporter rep;
    auto image = linker::link(mod, **target, **fmt, rep);
    EXPECT_EQ(rep.errorCount(), 0u)
        << "a referenced data extern must NOT be rejected for a relocatable "
           "object (D-LK-OBJECT-DATA-EXTERN-RELOCATABLE)";
    EXPECT_TRUE(image.ok());
    EXPECT_FALSE(image.bytes.empty());
}

TEST(Linker, ImageWithNoDataBindingStillRejectsReferencedDataExtern) {
    // The IMAGE side of the c144 gate (positive pin, mirrors c143's image-side
    // reject test): an IMAGE format that declares no `dataImportBinding` MUST
    // still reject a surviving data extern with K_FormatLacksImportSupport. An
    // image is load-time-bound with no later linker to resolve the object, so
    // an unbindable data import is a load-time silent-failure. RED if the
    // reject is deleted outright — RelocatableKeepsReferencedDataExternAsUndefined
    // alone would not catch that (it only pins the relocatable branch).
    //
    // c149 (D-LK-EXTERN-DATA-IMPORT, the PE half): the shipped PE exec format
    // NOW declares `dataImportBinding: "got-indirect"` (the IAT-slot `__imp_`
    // model — the last missing image binding), so this pin runs against a
    // loadFromText PE-exec schema with the declaration REMOVED — the exact
    // "revert the JSON declaration" red-on-disable shape: the gate keys on the
    // schema field, never on the format name, so the reverted schema must
    // reject exactly as the pre-c149 shipped one did.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "dataModel": "LLP64",
      "format": {"name":"pe-exec-no-data-binding","kind":"pe"},
      "externCallDispatch": "direct-plt",
      "pe": { "machine": 34404, "characteristics": 34, "type": "exec" },
      "optionalHeader": { "magic": 523, "imageBase": 5368709120, "sectionAlignment": 4096, "fileAlignment": 512, "subsystem": 3, "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096, "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096 },
      "sections":[{"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}],
      "relocations":[{"name":"IMAGE_REL_AMD64_REL32","kind":1,"nativeId":4}]
    })");
    ASSERT_TRUE(fmt.has_value());
    ASSERT_TRUE((*fmt)->isImageFlavor()) << "the no-binding schema is an image";
    ASSERT_FALSE((*fmt)->dataImportBinding().has_value())
        << "the schema declares no dataImportBinding -- the reject condition";
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x48, 0x8B, 0x05, 0, 0, 0, 0};   // mov rax,[rip+stdout]
    fn.relocations.push_back(
        Relocation{3u, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    ExternImport imp;
    imp.symbol      = SymbolId{99};
    imp.mangledName = "stdout";
    imp.libraryPath = "msvcrt.dll";
    imp.isData      = true;                        // the DATA extern
    mod.externImports.push_back(std::move(imp));
    DiagnosticReporter rep;
    auto image = linker::link(mod, **target, **fmt, rep);
    EXPECT_FALSE(image.ok())
        << "an image with no dataImportBinding must reject a data extern";
    EXPECT_GE(countCode(rep, DiagnosticCode::K_FormatLacksImportSupport), 1u);
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

// D-LINK-DATA-RELOC-TARGET-VALIDATE: a DATA-item relocation (an abs64 pointer slot
// in a symbol-address global / function-pointer table / switch jump table) whose
// target is declared by NO function / data item / extern in its CU must fail LOUD at
// the RESOLUTION tier — symmetric with the function-relocation check above. Before
// this the resolver inspected ONLY `m.functions[].relocations`, so a dangling /
// mis-targeted DATA reloc slipped past resolution and surfaced only as a silent
// wrong / NULL pointer at runtime (the class the aSyscall miscompile belonged to).
// RED-ON-DISABLE: remove the `m.dataItems` loop in `resolveCrossCuSymbols` → the
// resolution-tier diagnostic (message "data-item relocation in CU …") never fires.
TEST(Linker, DataItemUndefinedRelocationFailsLoud) {
    auto loaded = loadMinimal();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;             // one clean function (no relocations)
    fn.symbol = SymbolId{1};
    mod.functions.push_back(std::move(fn));
    // A data item (a function-pointer table analogue) whose abs64 relocation targets
    // SymbolId #99 — declared by nothing in this CU.
    AssembledData di;
    di.symbol  = SymbolId{2};
    di.section = DataSectionKind::Data;
    di.bytes.assign(8, std::uint8_t{0});
    di.relocations.push_back(Relocation{/*offset=*/0u, /*target=*/SymbolId{99},
                                        /*kind=*/RelocationKind{2}, /*addend=*/0});
    mod.dataItems.push_back(std::move(di));
    DiagnosticReporter rep;
    auto image = linker::link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_GE(countCode(rep, DiagnosticCode::K_SymbolUndefined), 1u)
        << "a DATA-item relocation to an undeclared symbol must fail loud";
    bool namedByResolutionCheck = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_SymbolUndefined
            && d.actual.find("data-item relocation") != std::string::npos) {
            namedByResolutionCheck = true;
        }
    }
    EXPECT_TRUE(namedByResolutionCheck)
        << "the resolution-tier data-item reloc check must NAME the dangling target "
           "(RED-on-disable if the dataItems loop is removed).";
    EXPECT_FALSE(image.ok());
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

// c86 (D-CSUBSET-BARE-PROTO-EXTERN-SYNTHESIS): a NO-LIBRARY extern (empty
// libraryPath — the bare-prototype cross-TU reference) that a SIBLING CU
// defines resolves exactly like a library-bound one: the reference binds to
// the definition, the import is stripped, and NO undefined-symbol reject
// fires. Pins that `rejectUnboundExterns` runs on the POST-merge module (a
// pre-resolution empty-library reject would falsely fail this legal C shape).
TEST(Linker, CrossCuNoLibraryExternResolvesToSiblingDefinition) {
    auto loaded = loadMinimal();
    ASSERT_TRUE(loaded.target && loaded.format);
    std::vector<AssembledModule> mods;
    {
        AssembledModule m;  // CU #1 references "foo" via a NO-LIBRARY extern
        m.cuId = CompilationUnitId{1};
        ExternImport ext;
        ext.symbol      = SymbolId{5};
        ext.mangledName = "foo";
        ext.libraryPath = "";   // bare-proto cross-TU reference: no library ON PURPOSE
        m.externImports.push_back(std::move(ext));
        mods.push_back(std::move(m));
    }
    mods.push_back(lkCu(2, {lkSym(2, "foo", SymbolBinding::Global)}));  // CU #2 DEFINES "foo"
    DiagnosticReporter rep;
    auto image = lkLink(mods, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(image.resolvedCrossCuRefs.size(), 1u);
    EXPECT_EQ(image.resolvedCrossCuRefs[0].reference.cuId.v, 1u);
    EXPECT_EQ(image.resolvedCrossCuRefs[0].definition.cuId.v, 2u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolUndefined), 0u)
        << "a sibling-defined no-library extern is NOT an undefined symbol";
    EXPECT_EQ(std::count(image.externImportNames.begin(), image.externImportNames.end(),
                         std::string{"foo"}), 0)
        << "the resolved reference must be stripped from the emitted imports";
}

// c86 (D-CSUBSET-BARE-PROTO-EXTERN-SYNTHESIS): a NO-LIBRARY extern that NO
// linked CU defines is an UNDEFINED SYMBOL — the linker rejects LOUD, NAMING
// the symbol (ld's behavior; C 6.2.2p5), before any walker emission. Pre-c86
// the empty libraryPath produced an internal "empty libraryPath" wording that
// never named the symbol; this pins the user-facing surface. RED-ON-DISABLE:
// drop the `rejectUnboundExterns` call in link() → the module flows to the
// format walker with a library-less import (a null IAT slot / zero-DT_NEEDED
// image — the silent-failure class this reject closes).
TEST(Linker, NoLibraryExternWithoutDefinitionIsUndefinedSymbol) {
    // The reject keeps a library-less import out of an IMAGE (a null IAT slot /
    // zero-DT_NEEDED image — the silent-failure class it closes). Since
    // D-LK-OBJECT-NOLIB-EXTERN-RELOCATABLE it is IMAGE-scoped: a relocatable
    // object legitimately keeps such an extern as an SHN_UNDEF the final linker
    // resolves (see Linker.RelocatableKeepsReferencedNoLibraryExternAsUndefined).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(fmt.has_value());
    ASSERT_TRUE((*fmt)->isImageFlavor());
    // Single module (N==1): nothing to resolve against — definitely undefined.
    AssembledModule m;
    m.cuId = CompilationUnitId{1};
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    Relocation rel;
    rel.offset = 4;
    rel.target = SymbolId{5};       // the extern's symbol — declared, so the
    rel.kind   = RelocationKind{1}; // reloc RESOLVES; undefinedness is the
    fn.relocations.push_back(rel);  // no-library reject's job, not the reloc's
    m.functions.push_back(std::move(fn));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "f1",
                                     SymbolBinding::Global, SymbolVisibility::Default});
    ExternImport ext;
    ext.symbol      = SymbolId{5};
    ext.mangledName = "neverDefined";
    ext.libraryPath = "";           // no library, no sibling definition
    m.externImports.push_back(std::move(ext));
    DiagnosticReporter rep;
    auto image = linker::link(m, **target, **fmt, rep);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolUndefined), 1u)
        << "an unresolved no-library extern must be rejected exactly once (image)";
    bool namedTheSymbol = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_SymbolUndefined
            && d.actual.find("neverDefined") != std::string::npos) {
            namedTheSymbol = true;
        }
    }
    EXPECT_TRUE(namedTheSymbol)
        << "the undefined-symbol diagnostic must NAME the symbol";
    EXPECT_FALSE(image.ok())
        << "an undefined symbol must block image emission";
    EXPECT_EQ(image.resolvedFuncCount, 0u);
}

// c86 (D-CSUBSET-BARE-PROTO-EXTERN-SYNTHESIS): an UNREFERENCED no-library
// extern is NOT an error — ld's rule: only a REFERENCED undefined symbol
// fails the link. A bare prototype nobody calls (sqlite3.h declares the whole
// API; a TU calls a subset, and a config-gated symbol may be defined nowhere)
// is dead declaration surface: the row is DROPPED before emission so no
// format walker ever sees a library-less import group (an empty DT_NEEDED /
// IMAGE_IMPORT_DESCRIPTOR name — a broken image). RED-ON-DISABLE: skipping
// the drop leaks the row to the emitted import table (externImportNames
// gains "neverCalled") or, pre-c86, hard-failed the whole link on a symbol
// nothing uses.
TEST(Linker, UnreferencedNoLibraryExternIsDroppedNotRejected) {
    auto loaded = loadMinimal();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule m;
    m.cuId = CompilationUnitId{1};
    m.expectedFuncCount = 1;
    AssembledFunction fn;      // no relocations — nothing references the extern
    fn.symbol = SymbolId{1};
    m.functions.push_back(std::move(fn));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "f1",
                                     SymbolBinding::Global, SymbolVisibility::Default});
    ExternImport ext;
    ext.symbol      = SymbolId{5};
    ext.mangledName = "neverCalled";
    ext.libraryPath = "";      // unbound AND unreferenced
    m.externImports.push_back(std::move(ext));
    DiagnosticReporter rep;
    auto image = linker::link(m, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolUndefined), 0u)
        << "an unreferenced unbound extern must not fail the link";
    EXPECT_TRUE(image.ok()) << "the link must proceed to a clean image";
    EXPECT_EQ(image.resolvedFuncCount, 1u);
    EXPECT_EQ(std::count(image.externImportNames.begin(), image.externImportNames.end(),
                         std::string{"neverCalled"}), 0)
        << "the dropped row must not reach the emitted import table";
}

// ── D-LINK-EXTERN-IMPORT-REFERENCE-GATE ──────────────────────────
// (c) The 3-row reference-gate pin. Three LIBRARY-BOUND extern imports exercise
// all three survival outcomes in one link:
//   (i)   unreferenced + EAGER      → KEPT (the shipped-descriptor eager law —
//         D-FFI-DESCRIPTOR-EAGER-IMPORT is untouched);
//   (ii)  unreferenced + non-eager  → DROPPED (gcc's unused-decl rule; the
//         Sqlitetestsse_Init shape — a LIBRARY-BOUND row the pre-fix gate skipped
//         because it only touched zero-library rows → the load-time exit-127);
//   (iii) referenced   + non-eager  → KEPT (the ordinary libc FFI import).
// Runs on the shipped RELOCATABLE ELF so all three library-bound rows emit as
// faithful undefined symbols and `externImportNames` reflects the survivors.
// RED-ON-DISABLE (both ways): flipping (i)'s eager bit to false drops (i) — which
// locks OUT an accidental Design-3 slide (dropping eager imports); clearing the
// gate's `!isEagerImport` erase term for bound rows keeps (ii) — the pre-fix
// load-blocker.
TEST(Linker, ReferenceGateKeepsEagerAndReferencedDropsUnreferencedNonEager) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(fmt.has_value());
    ASSERT_FALSE((*fmt)->isImageFlavor()) << "elf64-x86_64-linux is relocatable";
    AssembledModule mod;
    mod.cuId = CompilationUnitId{1};
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0x00, 0x00, 0x00, 0x00};   // call rel32 → row (iii) only
    fn.relocations.push_back(
        Relocation{1u, SymbolId{30}, RelocationKind{1}, 0});   // references (iii)
    mod.functions.push_back(std::move(fn));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "f1",
                                       SymbolBinding::Global, SymbolVisibility::Default});
    // (i) unreferenced + library-bound + EAGER → kept
    ExternImport eagerUnref;
    eagerUnref.symbol        = SymbolId{10};
    eagerUnref.mangledName   = "eager_unref";
    eagerUnref.libraryPath   = "libc.so.6";
    eagerUnref.isEagerImport = true;
    mod.externImports.push_back(std::move(eagerUnref));
    // (ii) unreferenced + library-bound + non-eager → DROPPED
    ExternImport nonEagerUnref;
    nonEagerUnref.symbol        = SymbolId{20};
    nonEagerUnref.mangledName   = "noneager_unref";
    nonEagerUnref.libraryPath   = "libc.so.6";
    nonEagerUnref.isEagerImport = false;
    mod.externImports.push_back(std::move(nonEagerUnref));
    // (iii) referenced + library-bound + non-eager → kept
    ExternImport nonEagerRef;
    nonEagerRef.symbol        = SymbolId{30};
    nonEagerRef.mangledName   = "noneager_ref";
    nonEagerRef.libraryPath   = "libc.so.6";
    nonEagerRef.isEagerImport = false;
    mod.externImports.push_back(std::move(nonEagerRef));
    DiagnosticReporter rep;
    auto image = linker::link(mod, **target, **fmt, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    auto kept = [&](std::string const& n) {
        return std::count(image.externImportNames.begin(),
                          image.externImportNames.end(), n) > 0;
    };
    EXPECT_TRUE(kept("eager_unref"))
        << "(i) an UNREFERENCED EAGER descriptor import must be KEPT";
    EXPECT_FALSE(kept("noneager_unref"))
        << "(ii) an UNREFERENCED non-eager library-bound import must be DROPPED "
           "(the Sqlitetestsse_Init load-blocker)";
    EXPECT_TRUE(kept("noneager_ref"))
        << "(iii) a REFERENCED non-eager library-bound import must be KEPT";
}

// D-LINK-EXTERN-IMPORT-REFERENCE-GATE (c, red-on-disable companion): flipping row
// (i)'s eager bit to FALSE — an UNREFERENCED non-eager library-bound import —
// makes it DROP exactly like row (ii). This is the second red-on-disable
// direction: it proves the KEEP of (i) above is due to the eager bit and locks
// out an accidental Design-3 slide (a gate that dropped eager imports too would
// pass the (i)-kept assertion only because SOMETHING kept it — here the SAME row,
// eager-cleared, must drop).
TEST(Linker, ReferenceGateDropsUnreferencedImportOnceEagerBitCleared) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.cuId = CompilationUnitId{1};
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};       // no relocations — nothing references the import
    mod.functions.push_back(std::move(fn));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "f1",
                                       SymbolBinding::Global, SymbolVisibility::Default});
    ExternImport imp;
    imp.symbol        = SymbolId{10};
    imp.mangledName   = "was_eager";
    imp.libraryPath   = "libc.so.6";
    imp.isEagerImport = false;     // the ONLY change vs (i) — eager bit cleared
    mod.externImports.push_back(std::move(imp));
    DiagnosticReporter rep;
    auto image = linker::link(mod, **target, **fmt, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(std::count(image.externImportNames.begin(),
                         image.externImportNames.end(), std::string{"was_eager"}), 0)
        << "an unreferenced library-bound import with the eager bit CLEARED must "
           "DROP (the same row kept only while eager)";
}

// (d) The exec reject STILL fires. A REFERENCED + UNBOUND + non-eager extern on an
// EXEC image (nothing later binds it) is a genuine undefined symbol: the gate must
// still reject LOUD, naming the symbol (the c143/c150 policy, preserved by the
// reference-gate generalization — the inner reject stays scoped to
// `libraryPath.empty()`). ★ The complementary safety — a REFERENCED
// LIBRARY-BOUND import must NOT reject on an exec (else every referenced libc
// import rejects loud, catastrophic) — is the reason the reject stays gated on the
// empty-library guard; it is witnessed green end-to-end by the referenced-import
// corpus examples (extern_call_elf / hello_printf). RED-ON-DISABLE: widening the
// reject past the empty-library guard, or dropping the referenced-unbound reject,
// silently defers this to a load-time crash.
TEST(Linker, ReferenceGateExecRejectStillFiresForReferencedUnboundExtern) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(fmt.has_value());
    ASSERT_TRUE((*fmt)->isImageFlavor());
    AssembledModule m;
    m.cuId = CompilationUnitId{1};
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.relocations.push_back(
        Relocation{4u, SymbolId{5}, RelocationKind{1}, 0});   // references the extern
    m.functions.push_back(std::move(fn));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "f1",
                                     SymbolBinding::Global, SymbolVisibility::Default});
    ExternImport ext;
    ext.symbol        = SymbolId{5};
    ext.mangledName   = "unresolved_ref";
    ext.libraryPath   = "";          // unbound + referenced → undefined on an exec
    ext.isEagerImport = false;
    m.externImports.push_back(std::move(ext));
    DiagnosticReporter rep;
    auto image = linker::link(m, **target, **fmt, rep);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolUndefined), 1u)
        << "a referenced unbound extern on an EXEC must still reject loud";
    bool named = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_SymbolUndefined
            && d.actual.find("unresolved_ref") != std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named) << "the undefined-symbol diagnostic must NAME the symbol";
    EXPECT_FALSE(image.ok());
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
    // Direct bind (c154, D-LK11-ELF-MACHO-CROSSCU-THUNK-EMISSION closure): this
    // synthetic `test-elf` declares NO `externCallDispatch`, so the merge binds the
    // reference DIRECTLY to the sibling definition's merged id — no thunk slot is
    // minted (the slot arm is scoped to `indirect-slot` formats, whose call sites
    // dereference it; see CrossCuLinkFormats.IndirectSlotDynMintsRelRoThunkSlotWith
    // RelativeRow). The pre-c154 merge minted the slot unconditionally and
    // retargeted this DIRECT `call rel32` into the slot's DATA bytes — the linked
    // exec branched into data (SIGSEGV, witnessed on elf-exec + pe-exec). With no
    // data item minted, this relocatable format emits the merged object CLEAN: no
    // capability-gate rejection, both bodies present, the call resolved to the def.
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_NoMatchingObjectFormat), 0u)
        << "a direct-bind cross-CU merge mints no data item — nothing for the "
           "capability gate to reject";
    EXPECT_FALSE(image.bytes.empty())
        << "the merged relocatable object must emit — the cross-CU reference is an "
           "ordinary intra-module reference after the direct bind";
    auto contains = [](std::vector<std::uint8_t> const& hay,
                       std::vector<std::uint8_t> const& needle) {
        if (needle.empty() || hay.size() < needle.size()) return false;
        for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
            if (std::equal(needle.begin(), needle.end(), hay.begin() + i)) return true;
        }
        return false;
    };
    EXPECT_TRUE(contains(image.bytes, calleeBody))
        << "the sibling definition's body must land in the merged object";
}

// The strip must NOT over-strip: a real FFI extern (no sibling definition) that is
// REFERENCED survives the merge as a genuine library import (the FF11 library tier
// owns it, untouched). D-LINK-EXTERN-IMPORT-REFERENCE-GATE: the import must be
// REFERENCED — an unreferenced non-eager library import is now dropped (gcc's
// unused-decl rule), so CU #1 CALLS "realffi" (the realistic real-FFI shape) to
// isolate the merge's not-over-stripping behavior from the reference gate's drop.
TEST(Linker, CrossCuRealFfiExternSurvivesMerge) {
    auto loaded = loadMinimal();
    ASSERT_TRUE(loaded.target && loaded.format);
    std::vector<AssembledModule> mods;
    {
        AssembledModule m;  // CU #1: imports + CALLS "realffi" (no sibling def)
        m.cuId = CompilationUnitId{1};
        m.expectedFuncCount = 1;
        AssembledFunction fn;
        fn.symbol = SymbolId{1};
        fn.bytes  = {0xE8, 0x00, 0x00, 0x00, 0x00, 0xC3};   // call rel32 realffi; ret
        fn.relocations.push_back(
            Relocation{1u, SymbolId{2}, RelocationKind{1}, 0});  // references realffi
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

// ═════════════════════════════════════════════════════════════════
// D-CSUBSET-THREAD-LOCAL (TLS C1): linker-tier gates.
// ═════════════════════════════════════════════════════════════════

TEST(Linker, SurvivingThreadLocalExternImportRejectsLoud) {
    // D-CSUBSET-THREAD-LOCAL-INITIAL-EXEC: a thread-local extern that
    // SURVIVED the cross-CU merge is a true LIBRARY thread-local
    // (glibc-errno-class). No shipped binding model can carry it —
    // copy-relocation would collapse it to ONE process-shared slot,
    // and local-exec tpoffs only cover THIS exec's own PT_TLS block.
    // The gate is UNCONDITIONAL and format-agnostic (it sits before
    // any walker dispatch): storage-model capability, not format
    // capability. An INTRA-program `extern thread_local` never gets
    // here (the LK11 merge strips the row when a sibling CU defines
    // it — the ordinary extern-resolution path).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(fmt.has_value());

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    // D-LINK-EXTERN-IMPORT-REFERENCE-GATE: the TLS import must be REFERENCED to
    // survive the reference gate and REACH this (later) TLS gate — an unreferenced
    // non-eager library import is now dropped (gcc's unused-decl rule). A true
    // library thread-local you actually read IS referenced, so the function loads
    // it (mov rax,[rip+lib_tls_object]) via a relocation targeting the import.
    fn.bytes  = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0xC3};   // mov rax,[rip+tls]; ret
    fn.relocations.push_back(
        Relocation{3u, SymbolId{77}, RelocationKind{1}, 0});   // references the TLS import
    mod.functions.push_back(std::move(fn));
    ExternImport tlsImp;
    tlsImp.symbol        = SymbolId{77};
    tlsImp.mangledName   = "lib_tls_object";
    tlsImp.libraryPath   = "libc.so.6";
    tlsImp.isData        = true;
    tlsImp.dataSizeBytes = 4;
    tlsImp.dataAlignBytes = 4;
    tlsImp.isThreadLocal = true;
    mod.externImports.push_back(std::move(tlsImp));

    DiagnosticReporter rep;
    auto image = linker::link(mod, **target, **fmt, rep);
    EXPECT_FALSE(image.ok());
    bool saw = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_FormatLacksThreadLocalSupport
            && d.actual.find("D-CSUBSET-THREAD-LOCAL-INITIAL-EXEC")
                   != std::string::npos) {
            saw = true;
        }
    }
    EXPECT_TRUE(saw)
        << "a surviving thread-local extern import must fail loud "
           "citing the initial-exec deferral, never bind through "
           "copy-relocation";
}

TEST(Linker, TdataItemOnNonOptedInFormatsRejectsAtAcceptsGate) {
    // Layering pin (audit LOW-b): for formats whose JSON does NOT
    // advertise tdata/tbss (Mach-O until C4 — pe64 opted in at TLS C3,
    // aarch64-ELF at C2, x86_64-ELF at C1), the GENERIC schema-declared
    // acceptsDataSection gate fires FIRST — K_NoMatchingObjectFormat
    // naming the section — before any walker runs. The walkers' own
    // K_FormatLacksThreadLocalSupport (0x8015) belts sit BEHIND this
    // gate and fire only on a direct walker call or a format JSON opting
    // in prematurely (pinned in the per-walker test files). Zero format-
    // name branches: the same set-membership check serves every format.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    for (char const* fmtName :
         {"macho64-x86_64-darwin-exec"}) {
        auto fmt = ObjectFormatSchema::loadShipped(fmtName);
        ASSERT_TRUE(fmt.has_value()) << fmtName;
        ASSERT_FALSE((*fmt)->acceptsDataSection(DataSectionKind::Tdata))
            << fmtName << " must not opt into tdata before its TLS "
                          "cycle lands";
        ASSERT_FALSE((*fmt)->acceptsDataSection(DataSectionKind::Tbss))
            << fmtName;

        AssembledModule mod;
        mod.expectedFuncCount = 1;
        AssembledFunction fn;
        fn.symbol = SymbolId{1};
        fn.bytes  = {0xC3};
        mod.functions.push_back(std::move(fn));
        AssembledData d;
        d.symbol    = SymbolId{42};
        d.section   = DataSectionKind::Tdata;
        d.bytes     = {7, 0, 0, 0};
        d.alignment = Alignment::of<4>();
        mod.dataItems.push_back(std::move(d));

        DiagnosticReporter rep;
        auto image = linker::link(mod, **target, **fmt, rep);
        EXPECT_FALSE(image.ok()) << fmtName;
        bool saw = false;
        for (auto const& diag : rep.all()) {
            if (diag.code == DiagnosticCode::K_NoMatchingObjectFormat
                && diag.actual.find("tdata") != std::string::npos
                && diag.actual.find("supportedDataSections")
                       != std::string::npos) {
                saw = true;
            }
        }
        EXPECT_TRUE(saw)
            << fmtName << ": the schema-declared acceptsDataSection "
                          "gate must reject a tdata item loud";
    }
}
