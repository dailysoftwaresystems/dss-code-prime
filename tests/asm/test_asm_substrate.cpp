// Assembler substrate tests — plan 13 AS1 cycle 1.
//
// Pins (the cycle-1 substrate-only contract):
//   * `assemble()` walks every LIR function and produces a parallel-
//     indexed `AssembledFunction` per `funcAt(i)`.
//   * Empty input → `AssembledModule::ok() == false`.
//   * Every non-`None` opcode arriving without a registered walker
//     fires `A_NoEncodingShapeWalker`.
//   * Every `None`-shape opcode fires `A_NoEncodingDeclared`.
//   * `forFuncByIndex` is bounds-checked.
//   * `TargetSchema`'s `relocations()` accessor + `relocationInfo`/
//     `relocationByName` lookups round-trip the JSON-declared rows.
//   * `validate()` rejects duplicate `kind`, zero `kind`, empty `name`.
//
// AS2 (`x86-variable` walker) + AS3 (`fixed32` walker) flip the
// non-`None` diagnostic expectations as their arms light up.

#include "asm/asm.hpp"
#include "asm_test_support.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "diagnostic_count.hpp"
#include "lir/lir.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "link/object_format_schema.hpp"  // FLIP-MARKER test loads shipped formats
#include "lowered_lir_fixture.hpp"
#include "mir/mir_node.hpp"
#include "mutate_target_schema.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace dss;
using dss::test_support::lowerCSubsetToLir;
using dss::test_support::asm_::countDiagnostics;

// ── Substrate surface: `assemble()` over an empty module ──────────────

TEST(AsmSubstrate, EmptyLirProducesEmptyAssembledModule) {
    Lir empty{};
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value()) << "loadShipped(x86_64) failed";
    DiagnosticReporter rep;
    auto result = assemble(empty, **schema, {}, rep);
    EXPECT_TRUE(result.functions.empty());
    EXPECT_EQ(result.expectedFuncCount, 0u);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(rep.errorCount(), 0u);
}

// ── LK6 cycle 2d: externs span flows through `assemble()` ─────────────

TEST(AsmSubstrate, ExternsSpanCopiesIntoAssembledModule) {
    // `assemble()` accepts an `std::span<ExternImport const>` and
    // copies it verbatim into the returned module's `externImports`.
    // Cycle-2d thread-through: the HIR→MIR pre-pass builds the
    // vector (`HirToMirResult.externImports`), MIR→LIR propagates
    // it, the assembler bundles it for the linker. (D-LK6-6 closure.)
    Lir empty{};
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    std::vector<ExternImport> externs;
    externs.push_back(ExternImport{SymbolId{99}, "printf", "libc.so.6"});
    externs.push_back(ExternImport{SymbolId{100},
                                   "_objc_msgSend",
                                   "/usr/lib/libobjc.A.dylib"});
    DiagnosticReporter rep;
    auto result = assemble(empty, **schema, {}, rep,
                           std::span<ExternImport const>{externs});
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(result.externImports.size(), 2u);
    EXPECT_EQ(result.externImports[0].symbol, SymbolId{99});
    EXPECT_EQ(result.externImports[0].mangledName, "printf");
    EXPECT_EQ(result.externImports[0].libraryPath, "libc.so.6");
    EXPECT_EQ(result.externImports[1].mangledName, "_objc_msgSend");
}

// ── D-LK4-RODATA-SUBSTRATE: AssembledData on AssembledModule ───────
//
// First slice toward FF6 hello-world (plan 11). The assembler now
// carries `dataItems` parallel to `functions` and `externImports`.
// Each item is a SymbolId-keyed byte blob tagged with a SectionKind
// (typically `Rodata`) for downstream walker emission to `.rdata` /
// `.rodata` / `__cstring`. The per-format walker arms are anchored
// as follow-up cycles (D-LK2-RODATA (PE), D-LK1-RODATA (ELF), D-LK3-RODATA (Mach-O)).
//
// This slice ships the SUBSTRATE only: the struct exists, lives on
// `AssembledModule`, and round-trips through `assemble()` (which
// today is a no-op for `dataItems` — the assembler doesn't yet
// produce them; hand-built tests are the consumer surface). The
// MIR-global → AssembledData producer (from string-literal
// promotion) lands in the next cycle paired with the per-format
// walker emission.

TEST(AsmSubstrate, AssembledDataDefaultIsRodataSectionEmptyBytes) {
    // Default-constructed AssembledData should default to
    // DataSectionKind::Rodata (the typical kind for string-
    // literal-promoted bytes), an invalid SymbolId (sentinel),
    // alignment 1 (byte-aligned), and empty bytes/relocations.
    // A regression that flipped the default to a different kind
    // (e.g. Bss) would silently route producer output to the
    // wrong walker arm. The `DataSectionKind` narrow + `Alignment`
    // newtype prevent the wider failure modes (walker-synthesized
    // sections; non-power-of-two alignment) at compile time.
    AssembledData d;
    EXPECT_EQ(d.section, DataSectionKind::Rodata);
    EXPECT_EQ(d.symbol, SymbolId{});
    EXPECT_EQ(d.alignment.bytes(), 1u);
    EXPECT_TRUE(d.bytes.empty());
    EXPECT_TRUE(d.relocations.empty());
}

TEST(AsmSubstrate, AssembledModuleCarriesDataItemsField) {
    // The field exists, is default-empty, and accepts hand-built
    // items — the substrate surface tests/walker code will rely on.
    AssembledModule m;
    EXPECT_TRUE(m.dataItems.empty());

    AssembledData d;
    d.symbol  = SymbolId{77};
    d.section = DataSectionKind::Rodata;
    d.bytes   = {'h', 'e', 'l', 'l', 'o', '\n', '\0'};
    d.alignment = Alignment::of<1>();
    m.dataItems.push_back(std::move(d));

    ASSERT_EQ(m.dataItems.size(), 1u);
    EXPECT_EQ(m.dataItems[0].symbol, SymbolId{77});
    EXPECT_EQ(m.dataItems[0].section, DataSectionKind::Rodata);
    EXPECT_EQ(m.dataItems[0].bytes.size(), 7u);
    EXPECT_EQ(m.dataItems[0].bytes.back(), '\0')
        << "C-string nul terminator must round-trip verbatim";
}

// D-LK2-RODATA CLOSED 2026-06-02: PE walker emits `.rdata`.
// D-LK1-ELF-EXEC-DATA-SECTIONS CLOSED: the ELF ET_EXEC walker emits
// `.rodata` (both x86_64 and aarch64 exec formats declare the row +
// `supportedDataSections: ["rodata"]`). The FLIP-MARKER for PE and
// BOTH ELF exec formats is now NE(nullptr). Mach-O (D-LK3-RODATA)
// remains anchored; its assertion stays EQ(nullptr) until that
// walker arm closes.
TEST(AsmSubstrate, ShippedExecFormatsRodataSectionPerWalkerArm) {
    // PE + ELF (x86_64 + aarch64): walker arms CLOSED — must declare
    // the row. ELF arm closure = D-LK1-ELF-EXEC-DATA-SECTIONS; both
    // arches share the SAME elf.cpp code path (arch differs only via
    // config), so both must declare the rodata section row.
    for (auto const* formatName : {
             "pe64-x86_64-windows-exec",
             "elf64-x86_64-linux-exec",
             "elf64-aarch64-linux-exec"}) {
        auto fmt = ObjectFormatSchema::loadShipped(formatName);
        ASSERT_TRUE(fmt.has_value()) << formatName;
        EXPECT_NE((*fmt)->sectionByKind(SectionKind::Rodata), nullptr)
            << formatName
            << ": its rodata walker arm is CLOSED (D-LK2-RODATA for "
               "PE / D-LK1-ELF-EXEC-DATA-SECTIONS for ELF) — the format "
               "JSON must declare a rodata (`.rdata` / `.rodata`) "
               "section row that the walker reads at emit time. If this "
               "fails, the JSON row was removed without re-anchoring "
               "the walker arm.";
    }
    // Mach-O: walker arm still anchored (D-LK3-RODATA) — must NOT yet
    // declare the row. When the walker arm closes, flip this EQ to NE.
    {
        auto fmt = ObjectFormatSchema::loadShipped(
            "macho64-x86_64-darwin-exec");
        ASSERT_TRUE(fmt.has_value());
        EXPECT_EQ((*fmt)->sectionByKind(SectionKind::Rodata), nullptr)
            << "macho64-x86_64-darwin-exec declares a rodata section "
               "row — the matching walker arm (D-LK3-RODATA for Mach-O) "
               "must land in the SAME slice; flip this assertion from "
               "EQ(nullptr) to NE(nullptr) when that closes.";
    }
}

// D-LK4-RODATA-BSS-INVARIANT (closed by validateAssembledData):
// Bss + non-empty bytes is a substrate-shape violation. Bss is
// zero-fill — the wire format reserves `sh_size` without storing
// bytes; a producer that wrote bytes into a Bss item would either
// silently embed them (defeating BSS's no-file-footprint property)
// or silently drop them. `validateAssembledData()` rejects this
// loud with `K_NoMatchingObjectFormat`.
TEST(AsmSubstrate, ValidateAssembledDataRejectsBssWithNonEmptyBytes) {
    AssembledData d;
    d.symbol  = SymbolId{42};
    d.section = DataSectionKind::Bss;
    d.bytes   = {0xAAu, 0xBBu};  // semantic contradiction
    DiagnosticReporter rep;
    EXPECT_FALSE(validateAssembledData(
        std::span<AssembledData const>{&d, 1}, rep));
    // Pin the EXACT diagnostic code — silent-failure F-4 fold:
    // loose `EXPECT_GT(errorCount, 0)` would silently pass even if
    // the wrong diagnostic fired (e.g. `K_NoMatchingObjectFormat`
    // before the K_BssDataHasBytes split landed).
    EXPECT_EQ(dss::test_support::countCode(
                  rep, DiagnosticCode::K_BssDataHasBytes),
              1u)
        << "validateAssembledData must emit exactly one "
           "K_BssDataHasBytes when Bss carries bytes";
}

TEST(AsmSubstrate, ValidateAssembledDataAcceptsBssWithEmptyBytes) {
    // The Bss-without-bytes case is the well-formed shape — a
    // zero-fill reservation. Validate must accept it.
    AssembledData d;
    d.symbol  = SymbolId{42};
    d.section = DataSectionKind::Bss;
    // bytes intentionally empty
    DiagnosticReporter rep;
    EXPECT_TRUE(validateAssembledData(
        std::span<AssembledData const>{&d, 1}, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(AsmSubstrate, ValidateAssembledDataRejectsDuplicateSymbolIds) {
    // Two items sharing the same non-sentinel SymbolId would
    // silently let "whichever the linker processed last" win
    // the symbol→VA resolution. validate() rejects loud.
    AssembledData a;
    a.symbol = SymbolId{77};
    a.bytes  = {'a'};
    AssembledData b;
    b.symbol = SymbolId{77};  // duplicate
    b.bytes  = {'b'};
    std::array<AssembledData, 2> items{a, b};
    DiagnosticReporter rep;
    EXPECT_FALSE(validateAssembledData(items, rep));
    EXPECT_EQ(dss::test_support::countCode(
                  rep, DiagnosticCode::K_DuplicateDataSymbol),
              1u)
        << "exactly one K_DuplicateDataSymbol — silent-failure F-4 "
           "fold tightens loose EXPECT_GT for the dup arm";
}

TEST(AsmSubstrate, ValidateAssembledDataAcceptsMultipleAnonymousItems) {
    // The sentinel `SymbolId{}` (.v == 0) is the "anonymous data"
    // marker — multiple anonymous items are legitimate (e.g.
    // multiple read-only padding constants with no individual
    // identity).
    AssembledData a;
    a.bytes = {'a'};
    AssembledData b;
    b.bytes = {'b'};
    std::array<AssembledData, 2> items{a, b};
    DiagnosticReporter rep;
    EXPECT_TRUE(validateAssembledData(items, rep))
        << "sentinel SymbolId{} is exempt from duplicate-check — "
           "anonymous items have no identity to clash";
}

TEST(AsmSubstrate, AlignmentNewtypeRejectsZero) {
    // D-LK4-RODATA-WIDE-ALIGNMENT-NEWTYPE: structural rejection
    // of zero / non-power-of-two alignments at construction time.
    EXPECT_FALSE(Alignment::fromBytes(0).has_value());
    EXPECT_FALSE(Alignment::fromBytes(3).has_value());  // not pow2
    EXPECT_FALSE(Alignment::fromBytes(7).has_value());  // not pow2
    EXPECT_FALSE(Alignment::fromBytes(257).has_value());  // > 256
    EXPECT_TRUE(Alignment::fromBytes(1).has_value());
    EXPECT_TRUE(Alignment::fromBytes(8).has_value());
    EXPECT_TRUE(Alignment::fromBytes(16).has_value());
    EXPECT_TRUE(Alignment::fromBytes(256).has_value());
    EXPECT_EQ(Alignment::of<16>().bytes(), 16u);
    EXPECT_EQ(Alignment::of<16>().log2(), 4u);
    // alignUp kernel: rounding via the newtype matches the
    // canonical formula `(n + a - 1) & ~(a - 1)`.
    EXPECT_EQ(Alignment::of<16>().alignUp(0u),  0u);
    EXPECT_EQ(Alignment::of<16>().alignUp(1u),  16u);
    EXPECT_EQ(Alignment::of<16>().alignUp(15u), 16u);
    EXPECT_EQ(Alignment::of<16>().alignUp(16u), 16u);
    EXPECT_EQ(Alignment::of<16>().alignUp(17u), 32u);
}

TEST(AsmSubstrate, DataSectionKindNarrowAdmitsOnlyDataSections) {
    // D-LK4-RODATA-SECTION-NARROW: the conversion from the wider
    // SectionKind to DataSectionKind is partial — only the 3
    // producer-emittable kinds (Rodata, Data, Bss) round-trip.
    // The 9 walker-synthesized kinds map to nullopt.
    EXPECT_EQ(dataSectionKindOf(SectionKind::Rodata),
              DataSectionKind::Rodata);
    EXPECT_EQ(dataSectionKindOf(SectionKind::Data),
              DataSectionKind::Data);
    EXPECT_EQ(dataSectionKindOf(SectionKind::Bss),
              DataSectionKind::Bss);
    // Walker-synthesized kinds — nullopt:
    EXPECT_EQ(dataSectionKindOf(SectionKind::Text),       std::nullopt);
    EXPECT_EQ(dataSectionKindOf(SectionKind::Symtab),     std::nullopt);
    EXPECT_EQ(dataSectionKindOf(SectionKind::Strtab),     std::nullopt);
    EXPECT_EQ(dataSectionKindOf(SectionKind::ShStrtab),   std::nullopt);
    EXPECT_EQ(dataSectionKindOf(SectionKind::RelocTable), std::nullopt);
    EXPECT_EQ(dataSectionKindOf(SectionKind::Dynamic),    std::nullopt);
    EXPECT_EQ(dataSectionKindOf(SectionKind::Note),       std::nullopt);
    EXPECT_EQ(dataSectionKindOf(SectionKind::Debug),      std::nullopt);
    EXPECT_EQ(dataSectionKindOf(SectionKind::Custom),     std::nullopt);
    // toSectionKind is total — every DataSectionKind maps:
    EXPECT_EQ(toSectionKind(DataSectionKind::Rodata),
              SectionKind::Rodata);
    EXPECT_EQ(toSectionKind(DataSectionKind::Data),
              SectionKind::Data);
    EXPECT_EQ(toSectionKind(DataSectionKind::Bss),
              SectionKind::Bss);
}

// Cross-tier canary (test-analyzer MEDIUM-7 fold): the SectionKind
// extract's load-bearing rationale is "asm-tier consumers can
// speak the vocabulary without dragging in link/...". If a future
// refactor accidentally relocated `SectionKind` back into the
// link tier, this test file (which only includes asm/asm.hpp at
// the top of the source) would still compile against the
// transitive include chain — silently re-coupling tiers. Pin
// the constant at the asm-tier surface so a `static_assert` on
// the enum value documents the contract: any reshuffle that
// changes Rodata's underlying value (or moves the enum) would
// fail compilation here.
static_assert(static_cast<int>(SectionKind::Rodata) == 1,
              "D-LK4-RODATA-SUBSTRATE cross-tier vocabulary canary: "
              "SectionKind::Rodata must remain at value 1 and "
              "reachable through asm/asm.hpp's include chain. If "
              "this fails, the SectionKind extract regressed.");

TEST(AsmSubstrate, AssembleProducesEmptyDataItemsByDefault) {
    // The current `assemble()` produces `dataItems`-empty modules:
    // there is no MIR/LIR → data producer yet. The field is reserved
    // for the future producer (HIR→MIR string-literal promotion).
    // This pin proves the substrate doesn't accidentally introduce
    // spurious items when none are configured upstream.
    Lir empty{};
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    DiagnosticReporter rep;
    auto result = assemble(empty, **schema, {}, rep);
    EXPECT_TRUE(result.dataItems.empty())
        << "assemble() must not synthesize unsolicited data items "
           "— the producer thread-through is anchored for a follow-"
           "up cycle";
}

TEST(AsmSubstrate, DefaultExternsSpanProducesEmptyExternImports) {
    // Backward compatibility: the 4-argument call site continues
    // to produce an empty `externImports` vector. Every existing
    // cycle-2a/2b/2c test path is unchanged.
    Lir empty{};
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    DiagnosticReporter rep;
    auto result = assemble(empty, **schema, {}, rep);
    EXPECT_TRUE(result.externImports.empty());
}

TEST(AsmSubstrate, ForFuncByIndexOutOfRangeReturnsNullptr) {
    AssembledModule m;
    m.functions.resize(2);
    m.expectedFuncCount = 2;
    EXPECT_NE(m.forFuncByIndex(0), nullptr);
    EXPECT_NE(m.forFuncByIndex(1), nullptr);
    EXPECT_EQ(m.forFuncByIndex(2), nullptr);
    EXPECT_EQ(m.forFuncByIndex(99), nullptr);
}

TEST(AsmSubstrate, AssembledModuleOkIsParallelIndexShapeCheck) {
    // ok() is the SHAPE check (parallel-index intact), not the
    // SUCCESS check (no encoding errors). Callers that need
    // "every byte encoded successfully" must also check
    // reporter.errorCount() == 0. Pin BOTH the happy-path shape
    // AND the broken-shape — a function-count of 0 (default-
    // constructed module) and a partial run (expectedFuncCount
    // populated but functions vector shorter) must both report
    // not-ok.
    AssembledModule empty;
    EXPECT_FALSE(empty.ok());

    AssembledModule populated;
    populated.functions.resize(2);
    populated.expectedFuncCount = 2;
    EXPECT_TRUE(populated.ok());

    AssembledModule partial;
    partial.functions.resize(1);
    partial.expectedFuncCount = 5;
    EXPECT_FALSE(partial.ok()) << "1/5 functions assembled is NOT ok";

    AssembledModule expectedZero;
    expectedZero.functions.resize(0);
    expectedZero.expectedFuncCount = 0;
    EXPECT_FALSE(expectedZero.ok())
        << "empty input is not 'ok' — caller distinguishes via expectedFuncCount";
}

// ── Substrate surface: cycle-1 fail-loud diagnostics ──────────────────

TEST(AsmSubstrate, EveryUnencodedInstFiresNoEncodingDiagnostic) {
    // Lower a trivial c-subset function all the way to LIR. The
    // shipped x86_64.target.json declares `encoding` for `mov` and
    // `ret` (AS2 cycle 2 scope); the remaining opcodes the LIR uses
    // (`add` / `jmp` / `call` / etc.) still have no encoding row,
    // so the assembler fires `A_NoEncodingDeclared` for them. The
    // parallel-index discipline must remain — every LIR function
    // produces a slot regardless of per-inst failure.
    auto bundle = lowerCSubsetToLir("int f(int x) { return x; }");
    ASSERT_TRUE(bundle.lir.ok);
    Lir const& lir = bundle.lir.lir;

    DiagnosticReporter rep;
    auto result = assemble(lir, *bundle.target, bundle.lir.lirToMir, rep);

    EXPECT_EQ(result.functions.size(), lir.moduleFuncCount());
    EXPECT_EQ(result.expectedFuncCount, lir.moduleFuncCount());
    EXPECT_TRUE(result.ok());

    // The originating function symbol must round-trip through
    // assemble() so the linker doesn't need to consult the upstream
    // `Lir` to know where to place the function's bytes.
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        EXPECT_EQ(result.functions[fi].symbol,
                  lir.funcSymbol(lir.funcAt(fi)));
    }

    // Substrate guarantee: every instruction whose opcode lacks an
    // encoding produces its OWN diagnostic — the parallel-index
    // continuity invariant. Count unencoded insts and assert the
    // diagnostic count matches.
    std::size_t unencodedInsts = 0;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
            LirBlockId const blk = lir.funcBlockAt(fn, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(blk); ++ii) {
                auto const* info = bundle.target->opcodeInfo(
                    lir.instOpcode(lir.blockInstAt(blk, ii)));
                if (info != nullptr
                    && info->encoding.shape == TargetEncodingShape::None) {
                    ++unencodedInsts;
                }
            }
        }
    }
    EXPECT_EQ(countDiagnostics(rep, DiagnosticCode::A_NoEncodingDeclared),
              unencodedInsts)
        << "every unencoded instruction must produce its own diagnostic";
}

TEST(AsmSubstrate, LirToMirSizeMismatchFailsLoud) {
    // The substrate uses lirToMir[LirInstId.v] (once AS2/AS3 wire it)
    // to stamp SourceMapEntry::mirInst. A span shorter than
    // lir.instCount() would silently UB. Pin the entry-time check:
    // a 1-entry span against an N-instruction module emits
    // A_LirToMirSizeMismatch and returns an empty module with
    // ok() == false.
    auto bundle = lowerCSubsetToLir("int f(int x) { return x; }");
    ASSERT_TRUE(bundle.lir.ok);
    Lir const& lir = bundle.lir.lir;
    ASSERT_GT(lir.instCount(), 1u) << "fixture must have multiple insts";

    std::vector<MirInstId> shortSpan;
    shortSpan.resize(lir.instCount() - 1);  // one short

    DiagnosticReporter rep;
    auto result = assemble(lir, *bundle.target, shortSpan, rep);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.functions.empty())
        << "broken-shape input MUST NOT produce a parallel-index slot";
    EXPECT_EQ(countDiagnostics(rep, DiagnosticCode::A_LirToMirSizeMismatch), 1u);
}

// Test `EncodingShapeWalkerFiresWhenShapeDeclaredWithoutWalker`
// removed AS3 cycle 3: both X86Variable and Fixed32 walkers are
// now registered (cycle 1's no-walker substrate is fully populated).
// The enum-drift fallback path still exists in asm.cpp + walkers'
// switch statements; it's unreachable via valid JSON (the loader
// rejects unknown shape strings) and is exercised only by future
// enum additions where the static_assert / fall-through diagnostic
// surfaces the maintenance gap.

#if 0
TEST(AsmSubstrate, EncodingShapeWalkerFiresWhenShapeDeclaredWithoutWalker) {
    // Synthesize a target schema whose `trap` opcode declares the
    // `fixed32` shape — AS2 cycle 2 wires the `x86-variable` walker,
    // but `fixed32` still has no walker registered (AS3 plugs it in).
    // `A_NoEncodingShapeWalker` is the expected diagnostic until then.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth_arm_like", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "trap", "result": "none",
              "terminatorKind": "unreachable",
              "encoding": {
                "format": "fixed32",
                "variants": [
                  {
                    "guard":    { "operandKinds": [] },
                    "template": { "opcode": [222] }
                  }
                ]
              } }
        ]
    })";
    auto schema = TargetSchema::loadFromText(kJson, "synth.target.json");
    ASSERT_TRUE(schema.has_value());

    // Build a tiny LIR with one function containing the `trap` opcode.
    auto const trapOp = (*schema)->opcodeByMnemonic("trap");
    ASSERT_TRUE(trapOp.has_value());
    LirBuilder b{**schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    (void)b.addUnreachable(*trapOp);
    Lir lir = std::move(b).finish();

    // `lirToMir` size must equal the LIR's instCount() — the
    // substrate's entry-time bounds check rejects shorter spans
    // (LirToMirSizeMismatchFailsLoud pins that path). Use a
    // default-constructed (invalid) MirInstId per slot since this
    // test exercises the dispatch arm, not the source-map stamping.
    std::vector<MirInstId> lirToMir(lir.instCount());

    DiagnosticReporter rep;
    auto result = assemble(lir, **schema, lirToMir, rep);
    EXPECT_EQ(result.functions.size(), 1u);
    EXPECT_GT(countDiagnostics(rep, DiagnosticCode::A_NoEncodingShapeWalker), 0u);
    EXPECT_EQ(countDiagnostics(rep, DiagnosticCode::A_NoEncodingDeclared), 0u);
}
#endif

// ── Schema surface: relocations[] taxonomy ────────────────────────────

TEST(TargetSchemaRelocations, JsonRoundTripsThroughAccessors) {
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" }
        ],
        "relocations": [
            { "name": "rel32",  "kind": 1, "pcRelative": true,  "addendBias": -4, "widthBytes": 4 },
            { "name": "abs64",  "kind": 2, "pcRelative": false, "addendBias":  0, "widthBytes": 8 }
        ]
    })";
    auto schema = TargetSchema::loadFromText(kJson, "synth.target.json");
    ASSERT_TRUE(schema.has_value());
    EXPECT_EQ((*schema)->relocationCount(), 2u);

    auto const* rel32 = (*schema)->relocationByName("rel32");
    ASSERT_NE(rel32, nullptr);
    EXPECT_EQ(rel32->kind, RelocationKind{1});
    EXPECT_TRUE(rel32->pcRelative);
    EXPECT_EQ(rel32->addendBias, -4);
    EXPECT_EQ(rel32->widthBytes, 4);

    auto const* abs64 = (*schema)->relocationByName("abs64");
    ASSERT_NE(abs64, nullptr);
    EXPECT_EQ(abs64->kind, RelocationKind{2});

    auto const* byKind1 = (*schema)->relocationInfo(RelocationKind{1});
    ASSERT_NE(byKind1, nullptr);
    EXPECT_EQ(byKind1->name, "rel32");

    auto const* byKind2 = (*schema)->relocationInfo(RelocationKind{2});
    ASSERT_NE(byKind2, nullptr);
    EXPECT_EQ(byKind2->name, "abs64");

    // Unknown name / unknown kind → nullptr (fail-loud at the
    // consumer; substrate never invents).
    EXPECT_EQ((*schema)->relocationByName("nope"), nullptr);
    EXPECT_EQ((*schema)->relocationInfo(RelocationKind{0xDEADBEEF}), nullptr);

    // Default-constructed RelocationKind is the slot-0 invalid
    // sentinel — lookup must NEVER match a declared kind, even if
    // the schema had a row with `kind: 0` (which validate() rejects).
    EXPECT_EQ((*schema)->relocationInfo(RelocationKind{}), nullptr);
    EXPECT_FALSE(RelocationKind{}.valid());
}

TEST(TargetSchemaRelocations, DuplicateKindIsLoadTimeFatal) {
    // Two rows declaring the same opaque `kind` would let the
    // assembler+linker disagree on which formula a relocation refers
    // to. validate() rejects the schema at load time.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" }
        ],
        "relocations": [
            { "name": "a", "kind": 7 },
            { "name": "b", "kind": 7 }
        ]
    })";
    auto schema = TargetSchema::loadFromText(kJson, "synth.target.json");
    EXPECT_FALSE(schema.has_value());
}

TEST(TargetSchemaRelocations, ZeroKindIsLoadTimeFatal) {
    // `kind == 0` is reserved as the invalid sentinel.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" }
        ],
        "relocations": [
            { "name": "a", "kind": 0 }
        ]
    })";
    auto schema = TargetSchema::loadFromText(kJson, "synth.target.json");
    EXPECT_FALSE(schema.has_value());
}

TEST(TargetSchemaRelocations, DuplicateNameIsLoadTimeFatal) {
    // Two rows with the same `name` would let the linker's
    // *.format.json cross-reference resolve to whichever row's index
    // entry won the emplace race. validate() / loader rejects.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" }
        ],
        "relocations": [
            { "name": "rel32", "kind": 1 },
            { "name": "rel32", "kind": 2 }
        ]
    })";
    auto schema = TargetSchema::loadFromText(kJson, "synth.target.json");
    EXPECT_FALSE(schema.has_value());
}

TEST(TargetSchemaRelocations, KindOutOfRangeIsLoadTimeFatal) {
    // `kind` must fit in uint32. Negative or > UINT32_MAX rejected.
    constexpr char const* kJsonNeg = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" }
        ],
        "relocations": [{ "name": "a", "kind": -1 }]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJsonNeg, "n.json").has_value());

    constexpr char const* kJsonTooBig = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" }
        ],
        "relocations": [{ "name": "a", "kind": 5000000000 }]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJsonTooBig, "b.json").has_value());
}

TEST(TargetSchemaRelocations, NonStringFormulaIsLoadTimeFatal) {
    // Silent type-coercion would drop a non-string `formula`
    // silently. The substrate mirrors `terminatorKind`'s strict
    // type-check.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" }
        ],
        "relocations": [{ "name": "a", "kind": 1, "formula": 7 }]
    })";
    auto schema = TargetSchema::loadFromText(kJson, "synth.target.json");
    EXPECT_FALSE(schema.has_value());
}

TEST(TargetSchemaRelocations, EmptyNameLookupReturnsNullptr) {
    // Boundary check on the consumer path: looking up by empty name
    // against a valid schema returns nullptr — never accidentally
    // matches an empty-string key that the loader already rejects.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" }
        ],
        "relocations": [{ "name": "rel32", "kind": 1 }]
    })";
    auto schema = TargetSchema::loadFromText(kJson, "synth.target.json");
    ASSERT_TRUE(schema.has_value());
    EXPECT_EQ((*schema)->relocationByName(""), nullptr);
}

TEST(TargetSchemaRelocations, EmptyNameIsLoadTimeFatal) {
    // Empty name would silently collide with another empty-name row
    // in the linker's *.format.json cross-reference.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" }
        ],
        "relocations": [
            { "name": "", "kind": 1 }
        ]
    })";
    auto schema = TargetSchema::loadFromText(kJson, "synth.target.json");
    EXPECT_FALSE(schema.has_value());
}

TEST(TargetSchemaRelocations, AbsentSectionIsLegal) {
    // A target that emits no relocations leaves the section absent.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" }
        ]
    })";
    auto schema = TargetSchema::loadFromText(kJson, "synth.target.json");
    ASSERT_TRUE(schema.has_value());
    EXPECT_EQ((*schema)->relocationCount(), 0u);
    EXPECT_EQ((*schema)->relocationInfo(RelocationKind{1}), nullptr);
}

// ── Schema surface: encoding facet on opcode rows ─────────────────────

TEST(TargetSchemaEncoding, OpcodeWithoutEncodingDefaultsToNoneShape) {
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "nop",     "result": "none" }
        ]
    })";
    auto schema = TargetSchema::loadFromText(kJson, "synth.target.json");
    ASSERT_TRUE(schema.has_value());
    auto const idx = (*schema)->opcodeByMnemonic("nop");
    ASSERT_TRUE(idx.has_value());
    auto const* info = (*schema)->opcodeInfo(*idx);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->encoding.shape, TargetEncodingShape::None);
}

TEST(TargetSchemaEncoding, X86VariableAndFixed32RoundTrip) {
    // Validate() requires `variants` non-empty when shape != None,
    // so each opcode gets a minimal placeholder variant. The test's
    // purpose is shape-discriminator JSON round-trip, not encoder
    // correctness — minimal variants suffice.
    // result=none so the new convergence-fix G rule (result-value
    // requires a destination slot) doesn't fire — this test pins
    // shape ROUND-TRIP, not encoder semantics.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "addx",
              "result": "none",
              "terminatorKind": "unreachable",
              "encoding": {
                "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": [] },
                    "template": { "opcode": [1] } }
                ]
              } },
            { "mnemonic": "addr",
              "result": "none",
              "terminatorKind": "unreachable",
              "encoding": {
                "format": "fixed32",
                "variants": [
                  { "guard": { "operandKinds": [] },
                    "template": { "fixedWord": 2 } }
                ]
              } }
        ]
    })";
    auto schema = TargetSchema::loadFromText(kJson, "synth.target.json");
    ASSERT_TRUE(schema.has_value());
    auto const* x = (*schema)->opcodeInfo(*(*schema)->opcodeByMnemonic("addx"));
    auto const* r = (*schema)->opcodeInfo(*(*schema)->opcodeByMnemonic("addr"));
    ASSERT_NE(x, nullptr);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(x->encoding.shape, TargetEncodingShape::X86Variable);
    EXPECT_EQ(r->encoding.shape, TargetEncodingShape::Fixed32);
}

TEST(TargetSchemaEncoding, UnknownFormatIsLoadTimeFatal) {
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "bogus", "result": "none",
              "encoding": { "format": "made-up-shape" } }
        ]
    })";
    auto schema = TargetSchema::loadFromText(kJson, "synth.target.json");
    EXPECT_FALSE(schema.has_value());
}

TEST(TargetSchemaEncoding, EncodingBlockWithoutFormatIsLoadTimeFatal) {
    // A typo like `"encoding": { "format2": "..." }` (or a bare
    // `"encoding": {}`) would silently leave the opcode at None;
    // the loader now requires `format` when the block is present.
    constexpr char const* kJsonMissing = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "none", "encoding": {} }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJsonMissing, "m.json").has_value());

    constexpr char const* kJsonTypo = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "none",
              "encoding": { "format2": "x86-variable" } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJsonTypo, "t.json").has_value());
}

TEST(TargetSchemaEncoding, NonObjectEncodingIsLoadTimeFatal) {
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "none",
              "encoding": "x86-variable" }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "s.json").has_value());
}

TEST(TargetSchemaEncoding, NonStringFormatIsLoadTimeFatal) {
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "none",
              "encoding": { "format": 7 } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "f.json").has_value());
}

// ── FC2 Part B: mandatoryPrefix (SSE legacy-prefix bytes) ─────────────

TEST(TargetSchemaEncoding, MandatoryPrefixParsesOnX86Variant) {
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "addsdx",
              "result": "none",
              "terminatorKind": "unreachable",
              "encoding": {
                "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": [] },
                    "template": { "mandatoryPrefix": [242], "opcode": [15, 88] } }
                ]
              } }
        ]
    })";
    auto schema = TargetSchema::loadFromText(kJson, "synth.target.json");
    ASSERT_TRUE(schema.has_value());
    auto const* info =
        (*schema)->opcodeInfo(*(*schema)->opcodeByMnemonic("addsdx"));
    ASSERT_NE(info, nullptr);
    ASSERT_EQ(info->encoding.variants.size(), 1u);
    auto const& tmpl = info->encoding.variants[0].tmpl;
    ASSERT_EQ(tmpl.mandatoryPrefix.size(), 1u);
    EXPECT_EQ(tmpl.mandatoryPrefix[0], 0xF2);
    ASSERT_EQ(tmpl.opcodeBytes.size(), 2u);
    EXPECT_EQ(tmpl.opcodeBytes[0], 0x0F);
    EXPECT_EQ(tmpl.opcodeBytes[1], 0x58);
}

TEST(TargetSchemaEncoding, MandatoryPrefixOnFixed32VariantIsLoadTimeFatal) {
    // The fixed32 walker has no legacy-prefix concept — silently
    // accepting the field would let a misauthored ARM64-style row
    // believe its prefix is emitted. Mirrors the opcodeBytes /
    // modrmRegExt fixed32 rejections.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "wordop",
              "result": "none",
              "terminatorKind": "unreachable",
              "encoding": {
                "format": "fixed32",
                "variants": [
                  { "guard": { "operandKinds": [] },
                    "template": { "fixedWord": 2, "mandatoryPrefix": [242] } }
                ]
              } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "p.json").has_value());
}

// ── FC2 Part B: registerClassOps (per-class move/load/store) ──────────

TEST(TargetSchemaRegisterClassOps, ShippedX64ResolvesDeclaredAndDefaultOps) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const& s = **schema;

    // fpr: declared row — move=movaps, load=movsd_load, store=
    // movsd_store (the store landed with its first consumer — the
    // ms_x64 callee-saved-xmm prologue spill; resolving to the GPR
    // `store` here would mis-encode an XMM hwEncoding).
    EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::FPR, RegClassOp::Move),
              s.opcodeByMnemonic("movaps"));
    EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::FPR, RegClassOp::Load),
              s.opcodeByMnemonic("movsd_load"));
    EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::FPR, RegClassOp::Store),
              s.opcodeByMnemonic("movsd_store"))
        << "fpr store must resolve to MOVSD's store form, never the "
           "GPR store";

    // gpr: no row — the universal default bindings.
    EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::GPR, RegClassOp::Move),
              s.opcodeByMnemonic("mov"));
    EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::GPR, RegClassOp::Load),
              s.opcodeByMnemonic("load"));
    EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::GPR, RegClassOp::Store),
              s.opcodeByMnemonic("store"));

    // vr: no row + not the default class → nothing (a future vector
    // class must declare its ops, never inherit the GPR forms).
    EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::VR, RegClassOp::Move),
              std::nullopt);
}

TEST(TargetSchemaRegisterClassOps, StrippedTableLosesFprOpsButKeepsGprDefaults) {
    // Strip the whole registerClassOps section (the red-on-disable
    // lever for every consumer test): fpr ops vanish, gpr defaults
    // survive (they come from the universal bindings, not the table).
    auto mutated = dss::test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) { doc.erase("registerClassOps"); });
    ASSERT_TRUE(mutated.has_value());
    auto const& s = **mutated;
    EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::FPR, RegClassOp::Move),
              std::nullopt);
    EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::FPR, RegClassOp::Load),
              std::nullopt);
    EXPECT_EQ(s.regClassOpOpcode(TargetRegClass::GPR, RegClassOp::Move),
              s.opcodeByMnemonic("mov"));
}

TEST(TargetSchemaRegisterClassOps, UnresolvableMnemonicIsLoadTimeFatal) {
    // A declared per-class mnemonic that names no opcode row is a
    // schema typo — load-time fatal (at the consumer it would be
    // indistinguishable from trigger-disciplined omission).
    auto mutated = dss::test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            doc["registerClassOps"][0]["move"] = "no_such_opcode";
        });
    EXPECT_FALSE(mutated.has_value());
}

TEST(TargetSchemaRegisterClassOps, UnknownClassNameIsLoadTimeFatal) {
    auto mutated = dss::test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            doc["registerClassOps"][0]["class"] = "made-up-class";
        });
    EXPECT_FALSE(mutated.has_value());
}

TEST(TargetSchemaRegisterClassOps, DuplicateClassRowIsLoadTimeFatal) {
    auto mutated = dss::test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            doc["registerClassOps"].push_back(
                doc["registerClassOps"][0]);  // second fpr row
        });
    EXPECT_FALSE(mutated.has_value());
}

TEST(TargetSchemaEncoding, MandatoryPrefixByteOutOfRangeIsLoadTimeFatal) {
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op",
              "result": "none",
              "terminatorKind": "unreachable",
              "encoding": {
                "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": [] },
                    "template": { "mandatoryPrefix": [256], "opcode": [1] } }
                ]
              } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "r.json").has_value());
}

// ── Diagnostic surface: A_* renders with the `A` prefix ───────────────

TEST(AsmDiagnostics, AnNibbleRendersAsLetterA) {
    // The 0x1xxx high-nibble allocation maps to the letter 'A' via
    // diagnosticCodePrefix's switch. Pinning this here keeps plan 00
    // §0.3 + parse_diagnostic.cpp + this test triangulated.
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::A_NoEncodingDeclared),     "A0001");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::A_NoEncodingShapeWalker),  "A0002");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::A_LirToMirSizeMismatch),   "A0003");
}

// ── D-LK4-RODATA-PRODUCER-AGGREGATE-GLOBAL: encoder unit pins ──────────
//
// `lowerMirGlobalsToDataItems` encodes const-init aggregate globals to
// `.rodata` bytes via the recursive `encodeAggregateValue`. The c-subset
// corpus `struct_body_top_level` is the end-to-end RUNTIME witness for the
// encoder shapes a C source can REACH; these pins cover the shapes it cannot:
// the short-init zero-fill (HIR pre-normalizes omitted slots into a FULL
// positional field list, so the encoder never receives a short aggregate from
// c-subset) and the fail-loud arms (over-long init, an exotic-float leaf, an
// absent aggregate-layout). They build the `MirAggregateValue` shape DIRECTLY
// — §A.5(b): an unconsumed substrate path is latent unless the test constructs
// the consuming shape itself, not waits for a real consumer. Each pin is
// red-on-disable; the comment on each names the guard it watches fail.

namespace {

// Build a Mir with ONE constant-init module global of `type` initialized by
// `init`, lower it through `lowerMirGlobalsToDataItems`, return the emitted
// data items + the reporter's error count.
struct LoweredAgg {
    std::vector<AssembledData> items;
    std::size_t                errors;
};
[[nodiscard]] LoweredAgg lowerOneAggGlobal(
        TypeInterner const& ti, TypeId type, MirLiteralValue init,
        std::optional<AggregateLayoutParams> lp, DataModel dm) {
    MirBuilder b;
    std::uint32_t const lit = b.literalPoolAdd(std::move(init));
    b.addGlobal(type, SymbolId{1}, lit);
    Mir const m = std::move(b).finish();
    DiagnosticReporter rep;
    auto items = lowerMirGlobalsToDataItems(m, ti, lp, dm, rep);
    return {std::move(items), rep.errorCount()};
}

// A scalar field literal of `kind` carrying integer bits `v`.
[[nodiscard]] MirLiteralValue intField(std::int64_t v, TypeKind kind) {
    MirLiteralValue f;
    f.value = v;
    f.core  = kind;
    return f;
}

// Wrap `fields` into a struct/array aggregate literal tagged `core`.
[[nodiscard]] MirLiteralValue aggOf(std::vector<MirLiteralValue> fields, TypeKind core) {
    MirAggregateValue agg;
    agg.fields = std::move(fields);
    MirLiteralValue v;
    v.value = std::move(agg);
    v.core  = core;
    return v;
}

// The shipped-target params (natural alignment, 16-byte ISA cap).
constexpr AggregateLayoutParams kNatural16{ScalarAlignmentRule::Natural, 16};

} // namespace

// POSITIVE control: a {char,int} struct (the padding classic) FULLY
// initialized encodes char@0 + pad[1..3] + int@4 (LE), size 8 — at the unit
// tier, complementing the runtime corpus. Red-on-disable: a wrong field offset
// (or dropping the layout-driven pre-zero) changes the bytes.
TEST(AsmAggregateGlobal, PaddedStructFullInitEncodesByteExact) {
    TypeInterner ti{CompilationUnitId{1}};
    std::array<TypeId, 2> const f{ti.primitive(TypeKind::Char),
                                  ti.primitive(TypeKind::I32)};
    TypeId const s = ti.structType("Padded", f);
    auto const r = lowerOneAggGlobal(
        ti, s,
        aggOf({intField(7, TypeKind::Char), intField(0x11223344, TypeKind::I32)},
              TypeKind::Struct),
        kNatural16, DataModel::Lp64);
    ASSERT_EQ(r.errors, 0u);
    ASSERT_EQ(r.items.size(), 1u);
    std::vector<std::uint8_t> const expect{0x07, 0, 0, 0, 0x44, 0x33, 0x22, 0x11};
    EXPECT_EQ(r.items[0].bytes, expect);
}

// FC8 D-CSUBSET-BITFIELD-INIT: a GLOBAL bit-field struct initializer packs each
// field into its allocation UNIT in the static-data byte buffer (the path the
// `bitfield_init` corpus drives end-to-end; this is the byte-exact unit pin).
// `struct {unsigned a:3; unsigned b:5;}` (one 4-byte unit) initialized {5,20}:
// a=5 at bitOffset 0, b=20 at bitOffset 3 → 5 | (20<<3) = 0xA5 in byte 0; the
// other 3 unit bytes stay zero (pre-zeroed buffer). Red-on-disable: revert the
// encoder's bit-field arm to the `scalars(ty) empty` fail-loud and this errors
// instead of packing; or to a full-width per-field store and b clobbers a.
TEST(AsmAggregateGlobal, BitFieldStructInitPacksIntoUnitByteExact) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const u32 = ti.primitive(TypeKind::U32);
    std::array<TypeId, 2> const f{u32, u32};
    std::array<std::int64_t, 2> const widths{3, 5};
    TypeId const s = ti.structType("Flags", f, widths);
    AggregateLayoutParams gnuPacked{ScalarAlignmentRule::Natural, 16};
    gnuPacked.bitFieldStrategy = BitFieldStrategy::GnuPacked;
    auto const r = lowerOneAggGlobal(
        ti, s,
        aggOf({intField(5, TypeKind::U32), intField(20, TypeKind::U32)},
              TypeKind::Struct),
        gnuPacked, DataModel::Lp64);
    ASSERT_EQ(r.errors, 0u);
    ASSERT_EQ(r.items.size(), 1u);
    std::vector<std::uint8_t> const expect{0xA5, 0, 0, 0};   // 5 | (20<<3)
    EXPECT_EQ(r.items[0].bytes, expect);
}

// F1 (review-caught): an ORDINARY field that shares a bit-field's allocation
// unit must survive the pack. `struct { char x; unsigned a:3; }` puts x at byte
// 0 and a's u32 unit at bytes [0,4) — overlapping. The static-data encoder
// pre-zeroes the whole buffer ONCE, then writes x (byte 0) and ORs a in at
// bitOffset 8 (byte 1) → x is preserved. This is the GLOBAL side of the
// global/local agreement the MIR two-pass fix restores (see the MIR pin
// BitFieldUnitZeroPrecedesOrdinaryFieldStoreInSharedUnit). Red-on-disable: were
// the encoder to write the bit-field unit full-width (clobbering x) or skip the
// pre-zero, byte 0 would not read back 7.
TEST(AsmAggregateGlobal, BitFieldUnitSharingOrdinaryFieldByteExact) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i8  = ti.primitive(TypeKind::I8);
    TypeId const u32 = ti.primitive(TypeKind::U32);
    std::array<TypeId, 2> const f{i8, u32};
    std::array<std::int64_t, 2> const widths{kNotBitfield, 3};   // x ordinary, a:3
    TypeId const s = ti.structType("Tag", f, widths);
    AggregateLayoutParams gnuPacked{ScalarAlignmentRule::Natural, 16};
    gnuPacked.bitFieldStrategy = BitFieldStrategy::GnuPacked;
    auto const r = lowerOneAggGlobal(
        ti, s,
        aggOf({intField(7, TypeKind::I8), intField(5, TypeKind::U32)},
              TypeKind::Struct),
        gnuPacked, DataModel::Lp64);
    ASSERT_EQ(r.errors, 0u);
    ASSERT_EQ(r.items.size(), 1u);
    std::vector<std::uint8_t> const expect{7, 5, 0, 0};   // x=7 @byte0, a=5<<8 @byte1
    EXPECT_EQ(r.items[0].bytes, expect);
}

// SHORT-init zero-fill — the path c-subset cannot reach (HIR delivers a full
// field list). A {I32,I32,I32} with ONLY field 0 provided must encode field 0
// then ZERO the trailing 8 bytes (the layout-sized, pre-zeroed buffer).
// Red-on-disable: the trailing zeros come from the layout-sized pre-zero, so the
// assertion depends on it — remove `d.bytes.assign(lay->size, 0)` and the buffer
// is empty, so even field 0's write falls outside it → the leaf bounds-check fails
// loud (errors != 0) instead of yielding 12 clean bytes.
TEST(AsmAggregateGlobal, ShortInitZeroFillsTrailingFields) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32 = ti.primitive(TypeKind::I32);
    std::array<TypeId, 3> const f{i32, i32, i32};
    TypeId const s = ti.structType("Triple", f);
    auto const r = lowerOneAggGlobal(
        ti, s,
        aggOf({intField(0x11223344, TypeKind::I32)}, TypeKind::Struct),  // field 0 only
        kNatural16, DataModel::Lp64);
    ASSERT_EQ(r.errors, 0u);
    ASSERT_EQ(r.items.size(), 1u);
    std::vector<std::uint8_t> const expect{
        0x44, 0x33, 0x22, 0x11, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(r.items[0].bytes, expect);
}

// FAIL-LOUD: MORE init values than the struct has fields → reject, no data.
// Red-on-disable: drop the `agg.fields.size() > ops.size()` guard and the loop
// indexes `ops[i]` past the field count instead of failing loud.
TEST(AsmAggregateGlobal, OverLongInitFailsLoud) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32 = ti.primitive(TypeKind::I32);
    std::array<TypeId, 2> const f{i32, i32};
    TypeId const s = ti.structType("Pair", f);
    auto const r = lowerOneAggGlobal(
        ti, s,
        aggOf({intField(1, TypeKind::I32), intField(2, TypeKind::I32),
               intField(3, TypeKind::I32)}, TypeKind::Struct),  // 3 > 2 fields
        kNatural16, DataModel::Lp64);
    EXPECT_EQ(r.errors, 1u);
    EXPECT_TRUE(r.items.empty());
}

// FAIL-LOUD: an F16 leaf — the literal pool's `double` arm cannot represent
// f16/f128 losslessly. Reject, no data. Red-on-disable: make
// `decodeScalarLiteralBits` encode f16 from the double and this emits SILENT
// wrong bytes (0 errors, 1 item) instead of failing loud.
TEST(AsmAggregateGlobal, ExoticFloatLeafFailsLoud) {
    TypeInterner ti{CompilationUnitId{1}};
    std::array<TypeId, 1> const f{ti.primitive(TypeKind::F16)};
    TypeId const s = ti.structType("H", f);
    MirLiteralValue leaf;
    leaf.value = double{1.5};
    leaf.core  = TypeKind::F16;
    auto const r = lowerOneAggGlobal(
        ti, s, aggOf({leaf}, TypeKind::Struct), kNatural16, DataModel::Lp64);
    EXPECT_EQ(r.errors, 1u);
    EXPECT_TRUE(r.items.empty());
}

// FAIL-LOUD: an aggregate global, but the target declared NO `aggregateLayout`
// block (nullopt) → no sound layout → fail loud, never a guessed one.
// Red-on-disable: drop the `!aggregateLayout.has_value()` check and
// `computeLayout(…, *aggregateLayout, …)` dereferences a disengaged optional.
TEST(AsmAggregateGlobal, MissingAggregateLayoutFailsLoud) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32 = ti.primitive(TypeKind::I32);
    std::array<TypeId, 2> const f{i32, i32};
    TypeId const s = ti.structType("Pair", f);
    auto const r = lowerOneAggGlobal(
        ti, s,
        aggOf({intField(1, TypeKind::I32), intField(2, TypeKind::I32)},
              TypeKind::Struct),
        std::nullopt, DataModel::Lp64);   // no layout params declared
    EXPECT_EQ(r.errors, 1u);
    EXPECT_TRUE(r.items.empty());
}
