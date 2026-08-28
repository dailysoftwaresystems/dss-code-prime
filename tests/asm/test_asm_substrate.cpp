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
#include "core/types/wide_float_value.hpp"   // LD-3: folded F80/F128 global bytes
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
// D-MIR-OVERLAP-STRUCT-ZERO-INIT: `computeLayout` (the DECLARED size/offsets a
// byte pin must be measured against) + `compositeFieldsOverlap` (the single
// overlap authority, so a fixture's precondition is asserted, never assumed).
#include "core/types/type_lattice/type_layout.hpp"
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
using dss::test_support::lowerCToLir;
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
    // D-CSUBSET-TESTTU-SILENT-EXIT1: an empty module is a VALID success
    // (0 == 0) — a declaration-only TU lowers to a valid empty relocatable
    // object. RED-ON-DISABLE: restoring the `expectedFuncCount > 0` clause in
    // AssembledModule::ok() makes this false again (and silently rejects the
    // whole compile of any declaration-only TU).
    EXPECT_TRUE(result.ok());
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
// `supportedDataSections: ["rodata"]`).
// D-LK3-RODATA (Mach-O) CLOSED: the MH_EXECUTE walker emits a loadable
// `__TEXT,__const` from `AssembledModule.dataItems` — the SAME
// `exec_data_section.hpp` substrate (`buildExecDataSection` /
// `addDataSymbolVas`) the ELF arm uses, read through `sectionByKind`
// with zero format-name branches. The arm landed with
// `macho64-arm64-darwin-exec` (runtime-witnessed on Apple Silicon);
// `macho64-x86_64-darwin-exec` joined it on 2026-08-05 — MEASURED, its
// emitted image carries `__TEXT,__const addr=0x1000010e0 size=0x2` for
// a `const char msg[]="x"` TU. So the flip this test's old EQ(nullptr)
// branch instructed ("flip this EQ to NE when that closes") is now
// done, and the macho execs join the sweep rather than sitting in a
// hand-written negative branch: ONE list, every shipped exec format,
// every walker arm closed.
TEST(AsmSubstrate, ShippedExecFormatsRodataSectionPerWalkerArm) {
    // Every shipped EXEC format: its rodata walker arm is CLOSED, so
    // each must declare the row the walker reads at emit time. Per-arch
    // pairs are listed BOTH times on purpose — each pair shares one
    // writer code path (arch differs only via config), so a missing row
    // on one of a pair is a config regression the other would hide.
    for (auto const* formatName : {
             "pe64-x86_64-windows-exec",
             "elf64-x86_64-linux-exec",
             "elf64-aarch64-linux-exec",
             "macho64-x86_64-darwin-exec",
             "macho64-arm64-darwin-exec"}) {
        auto fmt = ObjectFormatSchema::loadShipped(formatName);
        ASSERT_TRUE(fmt.has_value()) << formatName;
        EXPECT_NE((*fmt)->sectionByKind(SectionKind::Rodata), nullptr)
            << formatName
            << ": its rodata walker arm is CLOSED (D-LK2-RODATA for "
               "PE / D-LK1-ELF-EXEC-DATA-SECTIONS for ELF / "
               "D-LK3-RODATA for Mach-O) — the format JSON must declare "
               "a rodata (`.rdata` / `.rodata` / `__TEXT,__const`) "
               "section row that the walker reads at emit time. If this "
               "fails, the JSON row was removed without re-anchoring "
               "the walker arm.";
    }
    // ★ The row alone is not the contract — a format may declare a
    // `sections[]` row and still not ADMIT the items (the linker's
    // pre-walker gate reads `supportedDataSections`, a SEPARATE key).
    // Pin both halves together: a format carrying the row but not the
    // opt-in accepts nothing, and one carrying the opt-in but not the
    // row is worse still — MEASURED 2026-08-05 on
    // macho64-x86_64-darwin-exec, that combination returns from
    // `encodeExecDynamic` with NO diagnostic and the caller reports
    // the generic `K_ImageEmpty` ("the walker returned success with no
    // output"), because the `hasConst && secConst == nullptr` guard at
    // src/link/format/macho.cpp is a bare `return {}` while its
    // `hasData`/`hasBss` siblings ten lines below each emit a
    // `K_NoMatchingObjectFormat` naming the missing row. This
    // assertion cannot fix that walker asymmetry (it is a src/link
    // defect, anchored separately) but it does stop THIS pair of keys
    // from drifting apart in the shipped configs, which is the only
    // way the shipped pipeline reaches it.
    for (auto const* formatName : {
             "pe64-x86_64-windows-exec",
             "elf64-x86_64-linux-exec",
             "elf64-aarch64-linux-exec",
             "macho64-x86_64-darwin-exec",
             "macho64-arm64-darwin-exec"}) {
        auto fmt = ObjectFormatSchema::loadShipped(formatName);
        ASSERT_TRUE(fmt.has_value()) << formatName;
        EXPECT_TRUE((*fmt)->acceptsDataSection(DataSectionKind::Rodata))
            << formatName
            << ": it declares a rodata `sections[]` row but does NOT "
               "list `rodata` in `supportedDataSections`, so the "
               "linker's pre-walker gate rejects every rodata item and "
               "the row is dead config. The two keys must move "
               "together.";
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
    // reporter.errorCount() == 0. Pin the happy-path shape, the
    // valid-EMPTY shape, AND the broken shape: a MISMATCH
    // (expectedFuncCount populated but functions vector shorter)
    // reports not-ok, while a genuinely EMPTY module (0 == 0) is a
    // VALID success (D-CSUBSET-TESTTU-SILENT-EXIT1).
    AssembledModule empty;
    // RED-ON-DISABLE: restoring `expectedFuncCount > 0` in
    // AssembledModule::ok() flips this back to false (the silent-reject bug).
    EXPECT_TRUE(empty.ok()) << "empty (0 == 0) is a valid empty relocatable object";

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
    EXPECT_TRUE(expectedZero.ok())
        << "0 == 0 is a valid empty module (a declaration-only TU)";
}

// ── Substrate surface: cycle-1 fail-loud diagnostics ──────────────────

TEST(AsmSubstrate, EveryUnencodedInstFiresNoEncodingDiagnostic) {
    // Lower a trivial c function all the way to LIR. The
    // shipped x86_64.target.json declares `encoding` for `mov` and
    // `ret` (AS2 cycle 2 scope); the remaining opcodes the LIR uses
    // (`add` / `jmp` / `call` / etc.) still have no encoding row,
    // so the assembler fires `A_NoEncodingDeclared` for them. The
    // parallel-index discipline must remain — every LIR function
    // produces a slot regardless of per-inst failure.
    auto bundle = lowerCToLir("int f(int x) { return x; }");
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
    auto bundle = lowerCToLir("int f(int x) { return x; }");
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
// `.rodata` bytes via the recursive `encodeAggregateValue`. The c
// corpus `struct_body_top_level` is the end-to-end RUNTIME witness for the
// encoder shapes a C source can REACH; these pins cover the shapes it cannot:
// the short-init zero-fill (HIR pre-normalizes omitted slots into a FULL
// positional field list, so the encoder never receives a short aggregate from
// c) and the fail-loud arms (over-long init, an exotic-float leaf, an
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
    std::string                messages;   // C4b (I2): concatenated diag text for substring pins
    // A1 (audit fold): the CODE half, so a refusal pin can assert code AND text.
    // Text alone would go green on a diagnostic that says the right words under a
    // wrong code; a code alone cannot tell two arms of this function apart (every
    // arm of `lowerMirGlobalsToDataItems` shares `K_NoMatchingObjectFormat`).
    std::vector<DiagnosticCode> codes;
};
[[nodiscard]] LoweredAgg lowerOneAggGlobal(
        TypeInterner const& ti, TypeId type, MirLiteralValue init,
        std::optional<AggregateLayoutParams> lp, DataModel dm) {
    MirBuilder b;
    std::uint32_t const lit = b.literalPoolAdd(std::move(init));
    b.addGlobal(type, SymbolId{1}, lit, MirFuncId{}, SymbolBinding::Global,
                SymbolVisibility::Default, /*isConst=*/false,
                MirThreadStorage::Shared);
    Mir const m = std::move(b).finish();
    DiagnosticReporter rep;
    auto items = lowerMirGlobalsToDataItems(m, ti, lp, dm, rep);
    std::string                 msgs;
    std::vector<DiagnosticCode> codes;
    for (auto const& d : rep.all()) {
        msgs += d.actual;
        codes.push_back(d.code);
    }
    return {std::move(items), rep.errorCount(), std::move(msgs), std::move(codes)};
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

// Lower ONE scalar (I32) module global through `lowerMirGlobalsToDataItems`
// with a chosen mutability + init shape, returning the single emitted item +
// the error count. `init` set ⇒ constant-init; `init` nullopt ⇒ tentative
// (zero-init). `isConst` threads `MirGlobal.isConst` (the const-vs-mutable
// signal the section selection keys on). D-LK4-DATA-PRODUCER-MUTABLE-GLOBAL.
struct LoweredScalar {
    std::vector<AssembledData> items;
    std::size_t                errors;
};
[[nodiscard]] LoweredScalar lowerOneScalarGlobal(
        TypeInterner& ti, std::optional<std::int64_t> init, bool isConst) {
    MirBuilder b;
    TypeId const i32 = ti.primitive(TypeKind::I32);
    if (init.has_value()) {
        MirLiteralValue v;
        v.value = *init;
        v.core  = TypeKind::I32;
        std::uint32_t const lit = b.literalPoolAdd(std::move(v));
        b.addGlobal(i32, SymbolId{1}, lit, {},
                    SymbolBinding::Global, SymbolVisibility::Default, isConst,
                    MirThreadStorage::Shared);
    } else {
        b.addGlobal(i32, SymbolId{1}, UINT32_MAX, {},
                    SymbolBinding::Global, SymbolVisibility::Default, isConst,
                    MirThreadStorage::Shared);
    }
    Mir const m = std::move(b).finish();
    DiagnosticReporter rep;
    auto items = lowerMirGlobalsToDataItems(m, ti, kNatural16,
                                            DataModel::Lp64, rep);
    return {std::move(items), rep.errorCount()};
}

// c145 (D-LK-RELRO-CONST-DATA-RELOCATABLE): lower ONE scalar POINTER global
// initialized to a link-time SYMBOL ADDRESS (`int *p = &target;` — the F5
// reloc-bearing shape) with a chosen mutability. Passes a synthetic abs64
// `absPtrRelocKind` so the symbol-address arm fires; the const-vs-mutable
// ROUTING under test is independent of the reloc kind's numeric value.
[[nodiscard]] LoweredScalar lowerOneSymAddrScalarGlobal(TypeInterner& ti,
                                                        bool isConst) {
    MirBuilder b;
    TypeId const i32 = ti.primitive(TypeKind::I32);
    TypeId const ptr = ti.pointer(i32);
    MirLiteralValue v;
    v.value = MirSymbolAddrValue{/*symbol=*/2u, /*addend=*/0};
    v.core  = TypeKind::Ptr;
    std::uint32_t const lit = b.literalPoolAdd(std::move(v));
    b.addGlobal(ptr, SymbolId{1}, lit, MirFuncId{},
                SymbolBinding::Global, SymbolVisibility::Default, isConst,
                MirThreadStorage::Shared);
    Mir const m = std::move(b).finish();
    DiagnosticReporter rep;
    auto items = lowerMirGlobalsToDataItems(m, ti, kNatural16, DataModel::Lp64,
                                            rep, RelocationKind{2});
    return {std::move(items), rep.errorCount()};
}

} // namespace

// ── D-LK4-DATA-PRODUCER-MUTABLE-GLOBAL: section-selection pins ─────────
//
// `lowerMirGlobalsToDataItems` routes an INITIALIZED global to read-only
// `.rodata` (const) vs writable `.data` (mutable), and a TENTATIVE zero-init
// global to `.bss` (zero-fill). These are the structural pins the writable-data-
// sections cycle adds; each is RED on the exact regression named in its comment.

// A MUTABLE initialized global lands in writable `.data` (NOT read-only
// `.rodata` — a store into `.rodata` faults: the bug this cycle fixed). RED if
// asm.cpp reverts line ~737 to unconditional `Rodata`.
TEST(AsmDataSection, MutableInitializedGlobalLowersToData) {
    TypeInterner ti{CompilationUnitId{1}};
    auto const r = lowerOneScalarGlobal(ti, /*init=*/std::int64_t{5},
                                        /*isConst=*/false);
    ASSERT_EQ(r.errors, 0u);
    ASSERT_EQ(r.items.size(), 1u);
    EXPECT_EQ(r.items[0].section, DataSectionKind::Data)
        << "a mutable initialized global must route to writable .data";
    std::vector<std::uint8_t> const expect{5, 0, 0, 0};
    EXPECT_EQ(r.items[0].bytes, expect);
}

// ── D-CSUBSET-BITINT-DATA-GLOBAL: a `_BitInt(N)` global emits its container image ──
// ⚠ THIS TEST USED TO ASSERT THE OPPOSITE (`BitIntGlobalFailsLoud`), AND THE
// INVERSION IS DELIBERATE, NOT AN OVERSIGHT. C4b walled the arm fail-loud because
// the encoder could not carry the image: `appendLE` takes a `std::uint64_t` (so any
// width past 8 is a >>64 UB) and `scalarByteSize` takes a KIND (so it cannot know N).
// P42 answered both — a wider SOURCE (the wrapped limbs) and the TypeId-aware
// `sizeOfScalarOrBitInt` ladder — so the arm is a PRODUCER and the assertions turn
// over with it. Leaving the old refusal pin standing would be the P36 mistake
// [[D-CSUBSET-INT128-DATA-GLOBAL]] recorded: a gate that contradicts the shipped
// binary.
//
// ★★ THE PADDING BITS ARE THE DECISION THIS PIN EXISTS TO FREEZE. C23 6.2.6.1p6
// leaves padding-bit values UNSPECIFIED, so more than one image conforms. ✔MEASURED:
// clang 18.1.3 `-std=c23` ZERO-fills a static image (`_BitInt(17) = -3wb` → `fd ff 01
// 00`), but at -O0 its own RUNTIME padding is GARBAGE (`fd ff 01 a1`), so clang's
// padding is not a stable target. ✔MEASURED by EXECUTION on DSS: a runtime
// `_BitInt(17) = -3` reads byte 2 == 0xff and a runtime `_BitInt(65) = -1` reads byte
// 8 == 0xff — DSS SIGN-EXTENDS, because that is the `bitIntMask`/`maskTopLimb` wrap
// invariant the whole `_BitInt` tier is built on. The static image must agree with
// the RUNTIME THAT READS IT: zero-filling would make `_BitInt(17) g = -3wb; g < 0`
// answer false (the container would hold +131069) — a silent miscompile of the
// compiler's own initializer. Every `0xff` padding byte below is that decision.
//
// ★ THE FOUR LITERAL ARMS ARE ALL EXERCISED AND NONE IMPLIES ANOTHER — the dispatch
// lesson [[D-CSUBSET-INT128-DATA-GLOBAL]] paid for. A `_BitInt` initializer folds
// into `BitIntValue` OR a plain `std::int64_t`/`std::uint64_t`/`bool`; only the
// declared KIND is present in all four, so the arm keys on the kind. RED-ON-DISABLE:
// re-key the arm on `holds_alternative<BitIntValue>` and cases (d)-(f) refuse; drop
// the sign extension and (a)/(c) redden; emit `toLimbBytes()` untrimmed and (a)/(b)
// get 8-byte and 8-byte images where 4 and 1 are reserved; reverse the limb order and
// (c)/(g) redden while the single-limb cases stay green.
TEST(AsmDataSection, BitIntGlobalEmitsContainerSizedLittleEndianImage) {
    TypeInterner ti{CompilationUnitId{1}};
    auto const bytesOf = [](LoweredAgg const& r) {
        return r.items.empty() ? std::vector<std::uint8_t>{} : r.items[0].bytes;
    };
    auto const lowerBitInt = [&](std::int64_t n, bool signd, MirLiteralValue v) {
        return lowerOneAggGlobal(ti, ti.bitInt(n, signd), std::move(v), kNatural16,
                                 DataModel::Lp64);
    };

    // (a) NARROW SIGNED NEGATIVE — `_BitInt(17) g = -3wb;`. The container is FOUR
    //     bytes and bits 17..31 are padding: sign-extended here, `01 00` in clang.
    MirLiteralValue a;
    a.core  = TypeKind::BitInt;
    a.value = BitIntValue::fromI64(-3, 17, /*isSigned=*/true);
    auto const ra = lowerBitInt(17, true, std::move(a));
    EXPECT_EQ(ra.errors, 0u) << ra.messages;
    ASSERT_EQ(ra.items.size(), 1u);
    EXPECT_EQ(bytesOf(ra), (std::vector<std::uint8_t>{0xfd, 0xff, 0xff, 0xff}))
        << "four container bytes, padding SIGN-EXTENDED (clang would say fd ff 01 00)";
    EXPECT_EQ(ra.items[0].alignment.bytes(), 4u);

    // (b) NARROW UNSIGNED — `unsigned _BitInt(8) g = 200uwb;`. ONE byte, not four:
    //     an over-wide image would overrun the item the layout reserves.
    MirLiteralValue b;
    b.core  = TypeKind::BitInt;
    b.value = BitIntValue::fromU64(200, 8, /*isSigned=*/false);
    auto const rb = lowerBitInt(8, false, std::move(b));
    EXPECT_EQ(rb.errors, 0u) << rb.messages;
    ASSERT_EQ(rb.items.size(), 1u);
    EXPECT_EQ(bytesOf(rb), (std::vector<std::uint8_t>{0xc8}));
    EXPECT_EQ(rb.items[0].alignment.bytes(), 1u);

    // (c) WIDE UNSIGNED, 2 limbs, N%64 == 36 — every byte distinct in position, so a
    //     byte swap, a limb swap or a reversal cannot cancel out. The top limb's
    //     value occupies exactly its 36 valid bits.
    MirLiteralValue c;
    c.core  = TypeKind::BitInt;
    c.value = BitIntValue{
        std::vector<std::uint64_t>{0x99aabbccddeeff00ull, 0x0000000fedcba987ull},
        100, /*isSigned=*/false};
    auto const rc = lowerBitInt(100, false, std::move(c));
    EXPECT_EQ(rc.errors, 0u) << rc.messages;
    ASSERT_EQ(rc.items.size(), 1u);
    EXPECT_EQ(bytesOf(rc), (std::vector<std::uint8_t>{
                               0x00, 0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99,
                               0x87, 0xa9, 0xcb, 0xed, 0x0f, 0x00, 0x00, 0x00}))
        << "ceil(100/64)*8 == 16 bytes, limb 0 THEN limb 1, each little-endian";

    // (d) THE PLAIN `std::int64_t` ARM — the one a variant-keyed dispatch misses.
    MirLiteralValue d;
    d.core  = TypeKind::BitInt;
    d.value = std::int64_t{-3};
    auto const rd = lowerBitInt(17, true, std::move(d));
    EXPECT_EQ(rd.errors, 0u) << rd.messages;
    EXPECT_EQ(bytesOf(rd), (std::vector<std::uint8_t>{0xfd, 0xff, 0xff, 0xff}))
        << "an i64-arm initializer must produce the SAME image as the BitIntValue arm";

    // (e) THE PLAIN `std::uint64_t` ARM.
    MirLiteralValue e;
    e.core  = TypeKind::BitInt;
    e.value = std::uint64_t{200};
    auto const re = lowerBitInt(8, false, std::move(e));
    EXPECT_EQ(re.errors, 0u) << re.messages;
    EXPECT_EQ(bytesOf(re), (std::vector<std::uint8_t>{0xc8}));

    // (f) THE `bool` ARM.
    MirLiteralValue f;
    f.core  = TypeKind::BitInt;
    f.value = true;
    auto const rf = lowerBitInt(17, true, std::move(f));
    EXPECT_EQ(rf.errors, 0u) << rf.messages;
    EXPECT_EQ(bytesOf(rf), (std::vector<std::uint8_t>{0x01, 0x00, 0x00, 0x00}));

    // (g) WIDE SIGNED NEGATIVE, N == 65 — the padding decision at a WIDE width. Bit
    //     64 is the last value bit; bits 65..127 are padding and are ONES here.
    //     clang emits eight 0xff then `01` and seven `00`.
    MirLiteralValue g;
    g.core  = TypeKind::BitInt;
    g.value = BitIntValue::fromI64(-1, 65, /*isSigned=*/true);
    auto const rg = lowerBitInt(65, true, std::move(g));
    EXPECT_EQ(rg.errors, 0u) << rg.messages;
    EXPECT_EQ(bytesOf(rg), std::vector<std::uint8_t>(16, 0xff))
        << "a wide negative _BitInt's padding is the SIGN extension, matching what "
           "DSS's own maskTopLimb leaves in memory at runtime";

    // (h) THE IMAGE IS THE **WRAPPED** VALUE, never the raw limbs handed in. A limb
    //     pair with every high bit set at N==100 must come back masked to 36 valid
    //     bits in the top limb — the static twin of the runtime top-limb mask.
    MirLiteralValue h;
    h.core  = TypeKind::BitInt;
    h.value = BitIntValue{std::vector<std::uint64_t>{~0ull, ~0ull}, 100,
                          /*isSigned=*/false};
    auto const rh = lowerBitInt(100, false, std::move(h));
    EXPECT_EQ(rh.errors, 0u) << rh.messages;
    EXPECT_EQ(bytesOf(rh), (std::vector<std::uint8_t>{
                               0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                               0xff, 0xff, 0xff, 0xff, 0x0f, 0x00, 0x00, 0x00}))
        << "bits 100..127 are ABOVE N and must be zero for an UNSIGNED value";
}

// ── D-CSUBSET-BITINT-DATA-GLOBAL: a wide `_BitInt` global's ALIGNMENT is the
// psABI's, NOT its container size. `_BitInt(128)` is SIXTEEN bytes with align
// EIGHT (x86-64 psABI; `computeLayout`'s BitInt arm, and pinned independently by
// `examples/c/c23_bitint_wide`'s `_Alignof(_BitInt(128)) == 8` static assert) —
// unlike `__int128`, which really is 16/16. This is the ONE assertion separating
// the `_BitInt` arm from the neighbouring 128-bit one, and it is why the arm asks
// `computeLayout` for its alignment instead of reusing the other scalar arms'
// `Alignment::ofRuntimePow2(width)` rule. RED-ON-DISABLE: swap the alignment source
// to the image width and this reads 16.
TEST(AsmDataSection, WideBitIntGlobalKeepsPsAbiAlignmentNotContainerSize) {
    TypeInterner ti{CompilationUnitId{1}};
    MirLiteralValue v;
    v.core  = TypeKind::BitInt;
    v.value = BitIntValue::fromU64(1, 128, /*isSigned=*/false);
    auto const r = lowerOneAggGlobal(ti, ti.bitInt(128, /*isSigned=*/false),
                                     std::move(v), kNatural16, DataModel::Lp64);
    ASSERT_EQ(r.errors, 0u) << r.messages;
    ASSERT_EQ(r.items.size(), 1u);
    EXPECT_EQ(r.items[0].bytes.size(), 16u);
    EXPECT_EQ(r.items[0].alignment.bytes(), 8u)
        << "_BitInt(128) is 16 bytes at align 8 — sharing a limb COUNT with "
           "__int128 is not sharing a LAYOUT";
}

// ── D-CSUBSET-BITINT-DATA-GLOBAL: an initializer in NO integer literal arm is
// REFUSED, never fabricated. The producer emits an image only from a value it can
// actually read; a `double`-arm literal carrying a `_BitInt` kind is a malformed
// pool entry, and the honest answer is a diagnostic naming the anchor rather than a
// container full of whatever the bit pattern happened to be. RED-ON-DISABLE: give
// the producer a `return zeroImage()` fallback and this goes green with 16 zero
// bytes and no error — the exact silent-miscompile shape the wall existed to stop.
TEST(AsmDataSection, BitIntGlobalWithNonIntegerInitializerFailsLoud) {
    TypeInterner ti{CompilationUnitId{1}};
    MirLiteralValue v;
    v.core  = TypeKind::BitInt;
    v.value = 1.5;                      // a `double` arm under a `_BitInt` kind
    auto const r = lowerOneAggGlobal(ti, ti.bitInt(100, /*isSigned=*/false),
                                     std::move(v), kNatural16, DataModel::Lp64);
    EXPECT_GE(r.errors, 1u) << "no integer arm ⇒ no image";
    EXPECT_TRUE(r.items.empty()) << "a refused global emits NO bytes";
    EXPECT_NE(r.messages.find("D-CSUBSET-BITINT-DATA-GLOBAL"), std::string::npos)
        << "the refusal names the anchor: " << r.messages;
    EXPECT_NE(r.messages.find("no integer literal arm"), std::string::npos)
        << "and states the actual cause: " << r.messages;
}

// ── D-CSUBSET-BITINT-DATA-GLOBAL: the AGGREGATE-LEAF recursion emits it too ──
// This is the half that makes the close a whole contract rather than a scalar-only
// one. A `struct S { _BitInt(17) a; unsigned _BitInt(8) b; }` global does NOT reach
// the scalar arm — it routes through `encodeAggregateValue`, whose scalar leaf pairs
// `scalarByteSize` (a KIND, so it cannot size a `_BitInt`) with
// `decodeScalarLiteralBits` (a `std::uint64_t`, so it cannot carry an N>64 image).
// Both sites now ask the SAME producer, so the two encoders cannot drift.
// RED-ON-DISABLE: delete the leaf's `_BitInt` arm and every case here refuses with
// the generic aggregate text; give the leaf its own copy of the encoder and the
// padding decision has two homes to disagree in.
TEST(AsmDataSection, BitIntAggregateLeafEmitsImage) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const b17 = ti.bitInt(17, /*isSigned=*/true);
    TypeId const u8b = ti.bitInt(8, /*isSigned=*/false);
    TypeId const w65 = ti.bitInt(65, /*isSigned=*/false);

    // (1) STRUCT — a@0 (4 bytes, sign-extended padding), b@4 (ONE byte), size 8.
    std::array<TypeId, 2> const sf{b17, u8b};
    TypeId const st  = ti.structType("SBitInt", sf);
    auto const   lay = computeLayout(st, ti, kNatural16, DataModel::Lp64);
    ASSERT_TRUE(lay.has_value());
    ASSERT_EQ(lay->size, 8u) << "fixture precondition: 4-byte + 1-byte members, align 4";
    auto const rs = lowerOneAggGlobal(
        ti, st,
        aggOf({intField(-3, TypeKind::BitInt), intField(200, TypeKind::BitInt)},
              TypeKind::Struct),
        kNatural16, DataModel::Lp64);
    ASSERT_EQ(rs.errors, 0u) << rs.messages;
    ASSERT_EQ(rs.items.size(), 1u);
    EXPECT_EQ(rs.items[0].bytes,
              (std::vector<std::uint8_t>{0xfd, 0xff, 0xff, 0xff, 0xc8, 0, 0, 0}))
        << "the 1-byte member must NOT widen over the struct's tail padding";

    // (2) ARRAY of a WIDE element — the 16-byte stride is load-bearing: element 0
    //     has bit 64 set (a low-limb-only leaf writes 0 there) and the two elements
    //     differ in their low bytes, so a repeated or reversed walk shows.
    TypeId const arr = ti.array(w65, 2);
    MirLiteralValue e0;
    e0.core  = TypeKind::BitInt;
    e0.value = BitIntValue{std::vector<std::uint64_t>{23ull, 1ull}, 65,
                           /*isSigned=*/false};
    MirLiteralValue e1;
    e1.core  = TypeKind::BitInt;
    e1.value = BitIntValue::fromU64(29, 65, /*isSigned=*/false);
    auto const rr = lowerOneAggGlobal(
        ti, arr, aggOf({std::move(e0), std::move(e1)}, TypeKind::Array),
        kNatural16, DataModel::Lp64);
    ASSERT_EQ(rr.errors, 0u) << rr.messages;
    ASSERT_EQ(rr.items.size(), 1u);
    EXPECT_EQ(rr.items[0].bytes,
              (std::vector<std::uint8_t>{23, 0, 0, 0, 0, 0, 0, 0,
                                          1, 0, 0, 0, 0, 0, 0, 0,
                                         29, 0, 0, 0, 0, 0, 0, 0,
                                          0, 0, 0, 0, 0, 0, 0, 0}))
        << "two 16-byte elements; element 0's bit 64 lives in its SECOND limb";

    // (3) UNION — the first member is written at offset 0 and the slack stays zero.
    std::array<TypeId, 1> const uf{w65};
    TypeId const uni = ti.unionType("UBitInt", uf);
    MirLiteralValue um;
    um.core  = TypeKind::BitInt;
    um.value = BitIntValue{std::vector<std::uint64_t>{19ull, 1ull}, 65,
                           /*isSigned=*/false};
    auto const ru = lowerOneAggGlobal(ti, uni, aggOf({std::move(um)}, TypeKind::Union),
                                      kNatural16, DataModel::Lp64);
    ASSERT_EQ(ru.errors, 0u) << ru.messages;
    ASSERT_EQ(ru.items.size(), 1u);
    EXPECT_EQ(ru.items[0].bytes,
              (std::vector<std::uint8_t>{19, 0, 0, 0, 0, 0, 0, 0,
                                          1, 0, 0, 0, 0, 0, 0, 0}));

    // (4) A NEGATIVE WIDE MEMBER — the padding decision inside an aggregate. Its
    //     bits 65..127 are ONES; a zero-filling leaf turns -1 into 2^65-1 silently.
    std::array<TypeId, 1> const nf{ti.bitInt(65, /*isSigned=*/true)};
    TypeId const nst = ti.structType("SNegWide", nf);
    MirLiteralValue nm;
    nm.core  = TypeKind::BitInt;
    nm.value = BitIntValue::fromI64(-1, 65, /*isSigned=*/true);
    auto const rn = lowerOneAggGlobal(ti, nst, aggOf({std::move(nm)}, TypeKind::Struct),
                                      kNatural16, DataModel::Lp64);
    ASSERT_EQ(rn.errors, 0u) << rn.messages;
    ASSERT_EQ(rn.items.size(), 1u);
    EXPECT_EQ(rn.items[0].bytes, std::vector<std::uint8_t>(16, 0xff));
}

// ── D-CSUBSET-BITINT-DATA-GLOBAL: the aggregate encoder's OTHER leaf — the static
// BIT-FIELD packer, which packs into an allocation unit instead of writing an image.
// ✔MEASURED as a LIVE gap while closing this row and fixed in the same pass:
// `struct B { unsigned _BitInt(17) a : 5; unsigned _BitInt(17) b : 7; } g = {3uwb,
// 9uwb};` refused with the GENERIC aggregate text, because the packer's only decoder
// was `decodeScalarLiteralBits`, which has no `BitIntValue` arm at all — so a
// const-folded `_BitInt` bit-field value returned nullopt. The RUNTIME twin
// (`examples/c/c23_bitint_bitfield`) has always worked, which is exactly why nothing
// caught it: only the STATIC initializer was walled.
// RED-ON-DISABLE: route the `_BitInt` bit-field leaf back through
// `decodeScalarLiteralBits` and every case here refuses; keep the route but drop the
// value normalizer's conversion to the declared (N, signedness) and the SIGNED field
// packs the wrong low bits.
TEST(AsmDataSection, BitIntBitFieldStaticInitializerPacks) {
    TypeInterner ti{CompilationUnitId{1}};
    // `struct B { unsigned _BitInt(8) u : 4; signed _BitInt(8) s : 4; }` — one u8
    // allocation unit, u in bits 0..3 and s in bits 4..7 (LSB-first packing).
    std::array<TypeId, 2> const f{ti.bitInt(8, /*isSigned=*/false),
                                  ti.bitInt(8, /*isSigned=*/true)};
    std::array<std::int64_t, 2> const widths{4, 4};
    TypeId const st = ti.structType("BFBitInt", f, widths);
    // A bit-field layout needs a REALIZED packing strategy — `kNatural16` leaves it
    // unset, which makes `computeLayout` refuse the struct outright (the shipped
    // bit-field pins in this file set it the same way).
    AggregateLayoutParams gnuPacked{ScalarAlignmentRule::Natural, 16};
    gnuPacked.bitFieldStrategy = BitFieldStrategy::GnuPacked;

    // u = 13 (0b1101) and s = -3, whose low FOUR bits are also 0b1101. The unit is
    // therefore 0xdd — and the two nibbles being EQUAL is the point: it is the value
    // a packer gets only by masking each field to its own width, so a sign-extended
    // `s` (0xfd..) would blow past its nibble and show.
    MirLiteralValue u;
    u.core  = TypeKind::BitInt;
    u.value = BitIntValue::fromU64(13, 8, /*isSigned=*/false);
    MirLiteralValue s;
    s.core  = TypeKind::BitInt;
    s.value = BitIntValue::fromI64(-3, 8, /*isSigned=*/true);
    auto const r = lowerOneAggGlobal(
        ti, st, aggOf({std::move(u), std::move(s)}, TypeKind::Struct), gnuPacked,
        DataModel::Lp64);
    ASSERT_EQ(r.errors, 0u) << r.messages;
    ASSERT_EQ(r.items.size(), 1u);
    EXPECT_EQ(r.items[0].bytes, (std::vector<std::uint8_t>{0xdd}))
        << "u:4 == 13 in bits 0..3, s:4 == -3 masked to 0b1101 in bits 4..7";

    // The PLAIN `std::int64_t` arm packs identically — the bit-field leaf reaches the
    // same normalizer, so its four literal arms are the scalar arm's four.
    auto const r2 = lowerOneAggGlobal(
        ti, st,
        aggOf({intField(13, TypeKind::BitInt), intField(-3, TypeKind::BitInt)},
              TypeKind::Struct),
        gnuPacked, DataModel::Lp64);
    ASSERT_EQ(r2.errors, 0u) << r2.messages;
    ASSERT_EQ(r2.items.size(), 1u);
    EXPECT_EQ(r2.items[0].bytes, (std::vector<std::uint8_t>{0xdd}));
}

// ── D-CSUBSET-INT128-DATA-GLOBAL: a 128-bit integer global emits 16 REAL bytes ──
// ⚠ THIS TEST USED TO ASSERT THE OPPOSITE, AND THE INVERSION IS THE POINT. TF-C94
// walled this arm fail-loud because `appendLE` takes a `std::uint64_t`, so a
// width-16 append shifted by 64..120 bits — UB that on both shipped host arches
// masks the count to 6 bits and REPEATS the low 8 bytes into the high 8. The wall
// was the right answer to "we cannot encode this"; it was never the answer to
// "128-bit globals are unsupported". P36 widened the SOURCE instead of the shift
// (two little-endian limbs, neither append exceeding width 8), so the arm is a
// producer and the assertions turn over with it.
//
// ★ THE THREE CASES ARE THE THREE WAYS A 128-BIT INITIALIZER REACHES THIS ARM,
// and the row's own history says why each is load-bearing:
//   (1) FITS IN 64 BITS — folds into a PLAIN `std::uint64_t` pool arm, never a
//       `BitIntValue`. A dispatch keyed on the value's VARIANT never fires for it
//       (the miss the row recorded), and the UB shift made its high half a repeat
//       of the low, so its high 8 bytes being ZERO is the pin.
//   (2) WIDER THAN 64 BITS — the `BitIntValue` pool arm, the only one wide
//       enough. Every byte of the fixture is distinct in position, so a
//       byte-order error, a limb swap, or a repeat cannot cancel out.
//   (3) NEGATIVE SIGNED — the high limb must be SIGN-extended, not zero-filled.
//       Its `std::int64_t` arm carries no high limb at all, so the extension is a
//       decision the encoder makes and can get silently wrong.
// RED-ON-DISABLE: drop the high-limb append and every case loses 8 bytes; keep
// the append but zero-fill the extension and only (3) reddens; swap the two
// appends and only (2) reddens. Three cases, three distinct failure modes.
TEST(AsmDataSection, Int128GlobalEmitsSixteenLittleEndianBytes) {
    TypeInterner ti{CompilationUnitId{1}};

    auto const bytesOf = [](LoweredAgg const& r) {
        return r.items.empty() ? std::vector<std::uint8_t>{} : r.items[0].bytes;
    };

    // (1) FITS IN 64 BITS — `__uint128_t g = 5;`.
    MirLiteralValue fits;
    fits.core  = TypeKind::U128;
    fits.value = std::uint64_t{5};
    auto const rf = lowerOneAggGlobal(ti, ti.primitive(TypeKind::U128),
                                      std::move(fits), kNatural16, DataModel::Lp64);
    EXPECT_EQ(rf.errors, 0u) << rf.messages;
    ASSERT_EQ(rf.items.size(), 1u);
    std::vector<std::uint8_t> const wantFits{5, 0, 0, 0, 0, 0, 0, 0,
                                             0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(bytesOf(rf), wantFits)
        << "the high 8 bytes must be ZERO — the UB shift repeated the low word "
           "here, which is why a fits-in-64 value is the pin and not a triviality";

    // (2) WIDER THAN 64 BITS — high 0x1122334455667788, low 0x99aabbccddeeff00.
    MirLiteralValue wide;
    wide.core  = TypeKind::U128;
    wide.value = BitIntValue{
        std::vector<std::uint64_t>{0x99aabbccddeeff00ull, 0x1122334455667788ull},
        128, /*isSigned=*/false};
    auto const rw = lowerOneAggGlobal(ti, ti.primitive(TypeKind::U128),
                                      std::move(wide), kNatural16, DataModel::Lp64);
    EXPECT_EQ(rw.errors, 0u) << rw.messages;
    ASSERT_EQ(rw.items.size(), 1u);
    std::vector<std::uint8_t> const wantWide{
        0x00, 0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99,
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    EXPECT_EQ(bytesOf(rw), wantWide)
        << "every byte is distinct in position, so a swap or a reversal shows";

    // (3) NEGATIVE SIGNED — `__int128 g = -3;`, high limb all ones.
    MirLiteralValue sig;
    sig.core  = TypeKind::I128;
    sig.value = std::int64_t{-3};
    auto const rs = lowerOneAggGlobal(ti, ti.primitive(TypeKind::I128),
                                      std::move(sig), kNatural16, DataModel::Lp64);
    EXPECT_EQ(rs.errors, 0u) << rs.messages;
    ASSERT_EQ(rs.items.size(), 1u);
    std::vector<std::uint8_t> const wantSigned{
        0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    EXPECT_EQ(bytesOf(rs), wantSigned)
        << "a negative i128's high limb is SIGN-extended; zero-filling it would "
           "turn -3 into 2^128-3 with no diagnostic anywhere";
}

// ── TF-C94: the 128-bit wall holds at the AGGREGATE-LEAF recursion too ──
// A `struct { __uint128_t x; }` global does NOT reach the scalar arm above — it
// routes through the aggregate-leaf recursion, whose sole scalar encoder is
// `decodeScalarLiteralBits`. That chokepoint must return nullopt for I128/U128
// (the 16-byte value cannot pass through a u64), exactly as it does for F80/F128.
// Without it the leaf writes the low 8 bytes as though they were the whole value.
TEST(AsmDataSection, Int128AggregateMemberFailsLoud) {
    TypeInterner ti{CompilationUnitId{1}};
    std::array<TypeId, 1> const f{ti.primitive(TypeKind::U128)};
    TypeId const st = ti.structType("S128", f);
    auto const r = lowerOneAggGlobal(
        ti, st, aggOf({intField(9, TypeKind::U128)}, TypeKind::Struct),
        kNatural16, DataModel::Lp64);
    EXPECT_GE(r.errors, 1u)
        << "a struct with a 128-bit integer member must fail loud at the "
           "aggregate-leaf recursion (D-CSUBSET-INT128-DATA-GLOBAL)";
    EXPECT_TRUE(r.items.empty())
        << "red-on-disable: drop the I128/U128 arm from decodeScalarLiteralBits "
           "and this emits a struct whose 16-byte member holds 8 value bytes "
           "plus 8 bytes of whatever the encoder produced";
}

// ── LD-3 (D-CSUBSET-LONG-DOUBLE-CONSTFOLD-PRECISION): folded F80/F128 global
// bytes. A const-folded `long double` global carries a `WideFloatValue` pool arm;
// the globals emitter routes it through `appendWideFloatBits` (checked BEFORE the
// `double` arm), producing the EXACT 16-byte x87/binary128 slot. These pins are
// the asm-tier red-on-disable check that the folded arm emits the oracle bytes;
// the sibling `unfolded double still works` pin proves the additive branch did
// NOT disturb the pre-existing `appendF80Extended` widen path. ──────────────────

// A folded F80 global (20.0L + 22.0L = 42.0L via the kernel) emits the exact x87
// extended bytes: significand 0xa8.. (1.3125), exponent 0x4004 (e=5), +6 pad.
TEST(AsmDataSection, FoldedF80GlobalEmitsWideFloatBytes) {
    TypeInterner ti{CompilationUnitId{1}};
    MirLiteralValue v;
    v.core  = TypeKind::F80;
    v.value = *WideFloatValue::add(WideFloatValue::fromDouble(20.0, TypeKind::F80),
                                   WideFloatValue::fromDouble(22.0, TypeKind::F80));
    auto const r = lowerOneAggGlobal(ti, ti.primitive(TypeKind::F80), std::move(v),
                                     kNatural16, DataModel::Lp64);
    ASSERT_EQ(r.errors, 0u) << r.messages;
    ASSERT_EQ(r.items.size(), 1u);
    std::vector<std::uint8_t> const expect{0,0,0,0,0,0,0,0xa8,0x04,0x40,0,0,0,0,0,0};
    EXPECT_EQ(r.items[0].bytes, expect) << "folded F80 42.0L rodata bytes (appendWideFloatBits)";
}

// A folded F128 global emits the exact binary128 bytes: frac top 0x50 (.0101),
// exponent 0x4004.
TEST(AsmDataSection, FoldedF128GlobalEmitsWideFloatBytes) {
    TypeInterner ti{CompilationUnitId{1}};
    MirLiteralValue v;
    v.core  = TypeKind::F128;
    v.value = *WideFloatValue::add(WideFloatValue::fromDouble(20.0, TypeKind::F128),
                                   WideFloatValue::fromDouble(22.0, TypeKind::F128));
    auto const r = lowerOneAggGlobal(ti, ti.primitive(TypeKind::F128), std::move(v),
                                     kNatural16, DataModel::Lp64);
    ASSERT_EQ(r.errors, 0u) << r.messages;
    ASSERT_EQ(r.items.size(), 1u);
    std::vector<std::uint8_t> const expect{0,0,0,0,0,0,0,0,0,0,0,0,0,0x50,0x04,0x40};
    EXPECT_EQ(r.items[0].bytes, expect) << "folded F128 42.0L rodata bytes (appendWideFloatBits)";
}

// Regression: an UNFOLDED F80 leaf (the `double` pool arm — the LD-1 widen path)
// STILL emits via appendF80Extended. RED if adding the WideFloatValue branch
// swallowed the double arm. 42.0 is exact, so the bytes match the folded case.
TEST(AsmDataSection, UnfoldedDoubleF80GlobalStillWidens) {
    TypeInterner ti{CompilationUnitId{1}};
    MirLiteralValue v;
    v.core  = TypeKind::F80;
    v.value = 42.0;   // the pre-existing `double` arm (unfolded l-suffixed leaf)
    auto const r = lowerOneAggGlobal(ti, ti.primitive(TypeKind::F80), std::move(v),
                                     kNatural16, DataModel::Lp64);
    ASSERT_EQ(r.errors, 0u) << r.messages;
    ASSERT_EQ(r.items.size(), 1u);
    std::vector<std::uint8_t> const expect{0,0,0,0,0,0,0,0xa8,0x04,0x40,0,0,0,0,0,0};
    EXPECT_EQ(r.items[0].bytes, expect) << "unfolded double-arm F80 still widens (appendF80Extended)";
}

// A CONST initialized global stays read-only `.rodata`. RED if the section
// selection drops the `isConst` discriminator (routing const to `.data`).
TEST(AsmDataSection, ConstInitializedGlobalLowersToRodata) {
    TypeInterner ti{CompilationUnitId{1}};
    auto const r = lowerOneScalarGlobal(ti, /*init=*/std::int64_t{5},
                                        /*isConst=*/true);
    ASSERT_EQ(r.errors, 0u);
    ASSERT_EQ(r.items.size(), 1u);
    EXPECT_EQ(r.items[0].section, DataSectionKind::Rodata)
        << "a const initialized global must stay read-only .rodata";
    std::vector<std::uint8_t> const expect{5, 0, 0, 0};
    EXPECT_EQ(r.items[0].bytes, expect);
}

// c145 (D-LK-RELRO-CONST-DATA-RELOCATABLE): a CONST global initialized to a
// SYMBOL ADDRESS (`int *const p = &x;` — reloc-bearing) routes to RelRoConst
// (relocated-read-only) — NOT read-only `.rodata` (the loader must WRITE the
// resolved VA into the slot) and NOT writable `.data` (it is const). This is
// the asm-tier red-on-disable pin for the `relocBearingGlobalSection`
// chokepoint that BOTH the scalar symbol-address arm and the aggregate arm
// route through: revert its `isConst ? RelRoConst` to `Data` and this flips to
// `.data`. The -exec corpus cannot catch that regression (relro FOLDS into
// writable `.data` on the exec path -> same runtime result); only the `.o`
// path makes `.data.rel.ro` observable, so this producer-tier pin is where the
// routing is red-on-disable.
TEST(AsmDataSection, ConstSymbolAddressGlobalLowersToRelRo) {
    TypeInterner ti{CompilationUnitId{1}};
    auto const r = lowerOneSymAddrScalarGlobal(ti, /*isConst=*/true);
    ASSERT_EQ(r.errors, 0u);
    ASSERT_EQ(r.items.size(), 1u);
    EXPECT_EQ(r.items[0].section, DataSectionKind::RelRoConst)
        << "a const symbol-address (reloc-bearing) global must route to RelRoConst";
    EXPECT_FALSE(r.items[0].relocations.empty())
        << "it carries the abs64 pointer relocation the linker resolves";
    EXPECT_EQ(r.items[0].bytes.size(), 8u) << "pointer-width zero slot";
}

// A MUTABLE symbol-address global (`int *p = &x;`) stays writable `.data` (NOT
// RelRoConst — const is the discriminator). RED-ON-DISABLE: drop the isConst
// arm of the chokepoint (routing every reloc-bearing global to RelRoConst).
TEST(AsmDataSection, MutableSymbolAddressGlobalLowersToData) {
    TypeInterner ti{CompilationUnitId{1}};
    auto const r = lowerOneSymAddrScalarGlobal(ti, /*isConst=*/false);
    ASSERT_EQ(r.errors, 0u);
    ASSERT_EQ(r.items.size(), 1u);
    EXPECT_EQ(r.items[0].section, DataSectionKind::Data)
        << "a mutable symbol-address global stays writable .data";
    EXPECT_FALSE(r.items[0].relocations.empty());
}

// TF-C38 (D-CSUBSET-STATIC-INT-TO-PTR-ABSOLUTE): the DECODE-level proof that an
// int→ptr absolute-address leaf and a symbol-address (reloc) leaf coexist in ONE
// aggregate — the `{ "a", (void*)0x5 }` mix. `.a` (offset 0) emits an abs64
// RELOCATION over a pre-zeroed slot (the linker writes the string's VA); `.b`
// (offset 8) emits 8 RAW LE bytes (5,0,…) with NO relocation. Byte-exact companion
// to the end-to-end MIR pin StaticStructSymbolPlusIntToPtrMixKeepsRelocAndRawLeaf:
// the classifier produces this MirAggregateValue shape; here the encoder writes it.
// A plain uint64 Ptr leaf rides the scalar-leaf arm (decodeScalarLiteralBits) — the
// SAME arm the c80 null-pointer leaf uses, here proven for a NON-zero value.
TEST(AsmAggregateGlobal, IntToPtrLeafBesideSymbolEncodesRawBytesPlusReloc) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const ptr = ti.pointer(ti.primitive(TypeKind::Void));
    std::array<TypeId, 2> const f{ptr, ptr};
    TypeId const s = ti.structType("MixTwo", f);
    // field 0: a link-time symbol address (a string's rodata global) → reloc.
    MirLiteralValue sym;
    sym.value = MirSymbolAddrValue{/*symbol=*/2u, /*addend=*/0};
    sym.core  = TypeKind::Ptr;
    // field 1: an int→ptr absolute-address leaf (the classifier's output) → raw bytes.
    MirLiteralValue itp;
    itp.value = std::uint64_t{5};
    itp.core  = TypeKind::Ptr;
    MirBuilder b;
    std::uint32_t const lit =
        b.literalPoolAdd(aggOf({std::move(sym), std::move(itp)}, TypeKind::Struct));
    b.addGlobal(s, SymbolId{1}, lit, MirFuncId{}, SymbolBinding::Global,
                SymbolVisibility::Default, /*isConst=*/false, MirThreadStorage::Shared);
    Mir const m = std::move(b).finish();
    DiagnosticReporter rep;
    auto const items = lowerMirGlobalsToDataItems(m, ti, kNatural16, DataModel::Lp64,
                                                  rep, RelocationKind{7});
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(items.size(), 1u);
    ASSERT_EQ(items[0].bytes.size(), 16u) << "two 8-byte pointer slots";
    // `.b` (offset 8) holds the raw LE absolute address 5 — NO relocation.
    std::vector<std::uint8_t> const bTail(items[0].bytes.begin() + 8,
                                          items[0].bytes.end());
    std::vector<std::uint8_t> const expectB{5, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(bTail, expectB) << "int→ptr leaf = 8 raw LE bytes at offset 8";
    // `.a` (offset 0) is a pre-zeroed slot with ONE abs64 relocation.
    ASSERT_EQ(items[0].relocations.size(), 1u) << "exactly one reloc — the symbol field";
    EXPECT_EQ(items[0].relocations[0].offset, 0u) << "the reloc sits at the .a symbol slot";
    EXPECT_EQ(items[0].relocations[0].target.v, 2u);
    std::vector<std::uint8_t> const aHead(items[0].bytes.begin(),
                                          items[0].bytes.begin() + 8);
    std::vector<std::uint8_t> const zero8{0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(aHead, zero8) << "the symbol slot stays zero (linker writes the VA)";
}

// A TENTATIVE (zero-init, no initializer) global lowers to `.bss` with EMPTY
// bytes + a non-zero `reservedSize` (the zero-fill extent). RED if asm.cpp
// reverts the bss arm to the former fail-loud (D-LK4-RODATA-PRODUCER-BSS-EMIT)
// or emits file bytes for it.
TEST(AsmDataSection, TentativeGlobalLowersToBssEmptyBytesNonzeroSize) {
    TypeInterner ti{CompilationUnitId{1}};
    auto const r = lowerOneScalarGlobal(ti, /*init=*/std::nullopt,
                                        /*isConst=*/false);
    ASSERT_EQ(r.errors, 0u)
        << "a tentative global must now EMIT a .bss item (was fail-loud)";
    ASSERT_EQ(r.items.size(), 1u);
    EXPECT_EQ(r.items[0].section, DataSectionKind::Bss);
    EXPECT_TRUE(r.items[0].bytes.empty())
        << ".bss is zero-fill — it carries NO on-disk bytes";
    EXPECT_EQ(r.items[0].reservedSize, 4u)
        << ".bss must record the byte SIZE (sizeof(int)=4) for the section header";
    EXPECT_EQ(r.items[0].sizeInSection(), 4u);
}

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

// D-CSUBSET-BITFIELD-ABI-EXACT: the static-data encoder packs a bit-field GLOBAL
// into the MsvcStraddle layout (the PE byte path) — proving the per-ABI strategy
// reaches the codegen byte encoder, not only `computeLayout`. `struct {char a:7;
// int b:25;}` init {0x7F, 0x1FFFFFF} under msvc_straddle: a in a CHAR unit @byte0
// = 0x7F; b is an int (≠char) → a NEW int unit @byte4 → bits 0..24 set =
// FF FF FF 01 in bytes [4,8); size 8. (Under gnu_packed the SAME struct is size 4
// with b packed into a's unit at bit 7 — see test_type_layout B; this byte buffer
// is the MS-distinct golden, byte-for-byte what cl.exe lays B.b out as.)
// Red-on-disable: flip the strategy to gnu_packed → b packs at bit 7 of a 4-byte
// unit and the buffer (+size) differs.
TEST(AsmAggregateGlobal, BitFieldStructInitMsvcStraddleByteExact) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i8  = ti.primitive(TypeKind::I8);   // `char a:7`
    TypeId const i32 = ti.primitive(TypeKind::I32);  // `int b:25`
    std::array<TypeId, 2> const f{i8, i32};
    std::array<std::int64_t, 2> const widths{7, 25};
    TypeId const s = ti.structType("B", f, widths);
    AggregateLayoutParams msvc{ScalarAlignmentRule::Natural, 16};
    msvc.bitFieldStrategy = BitFieldStrategy::MsvcStraddle;
    auto const r = lowerOneAggGlobal(
        ti, s,
        aggOf({intField(0x7F, TypeKind::I8), intField(0x1FFFFFF, TypeKind::U32)},
              TypeKind::Struct),
        msvc, DataModel::Lp64);
    ASSERT_EQ(r.errors, 0u);
    ASSERT_EQ(r.items.size(), 1u);
    // a@byte0 = 0x7F (char unit); b@byte4 = 0x01FFFFFF (int unit), LE → FF FF FF 01.
    std::vector<std::uint8_t> const expect{0x7F, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0x01};
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

// FC8 (cycle-4 audit coverage debt): a UNION bit-field aggregate init. A union
// brace-init sets the FIRST member only; a bit-field member occupies bits [0,W)
// of its own allocation unit at offset 0. `{5}` on `union { unsigned a:3; ... }`
// packs a=5 into bits 0..2 of the pre-zeroed unit → byte 0 == 5, rest 0. Locks
// the union arm of the static-data bit-field encoder (cycle-4's union-init path
// was chokepoint-covered but unpinned).
TEST(AsmAggregateGlobal, BitFieldUnionInitPacksFirstMemberByteExact) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const u32 = ti.primitive(TypeKind::U32);
    std::array<TypeId, 2> const f{u32, u32};
    std::array<std::int64_t, 2> const widths{3, 5};   // a:3, b:5 (independent)
    TypeId const u = ti.unionType("Flags", f, widths);
    AggregateLayoutParams gnuPacked{ScalarAlignmentRule::Natural, 16};
    gnuPacked.bitFieldStrategy = BitFieldStrategy::GnuPacked;
    auto const r = lowerOneAggGlobal(
        ti, u,
        aggOf({intField(5, TypeKind::U32)}, TypeKind::Union),   // first member a:3=5
        gnuPacked, DataModel::Lp64);
    ASSERT_EQ(r.errors, 0u);
    ASSERT_EQ(r.items.size(), 1u);
    std::vector<std::uint8_t> const expect{5, 0, 0, 0};   // a=5 @bits 0..2 of unit 0
    EXPECT_EQ(r.items[0].bytes, expect);
}

// SHORT-init zero-fill — the path c cannot reach (HIR delivers a full
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

// ── D-MIR-OVERLAP-STRUCT-ZERO-INIT: the STATIC-DATA encoder's THREE outcomes ──
//
// `encodeAggregateValue`'s struct arm used to refuse EVERY explicit-offset struct
// outright (`if (in.hasExplicitOffsets(ty)) return false;`). It now asks the
// narrower — and correct — question: do the members ACTUALLY share bytes. That
// splits the one blanket refusal into THREE outcomes, and MOVES two of them:
//   (a) OVERLAP + all-zero initializer → ENCODES, as the layout-sized pre-zeroed
//       buffer (was: refused). `{0}`/`{}` denote a whole object of zero bytes,
//       unambiguous however many members alias them.
//   (b) DISJOINT explicit offsets      → ENCODES MEMBER-WISE at the DECLARED
//       offsets (was: refused — a FALSE refusal). This is a brand-new code path:
//       `lay->fieldOffsets[i]` had never been reached for an explicit-offset
//       struct, and for such a struct those offsets come from the descriptor
//       verbatim (type_layout.cpp's explicit-offset arm), not from alignment.
//   (c) OVERLAP + any non-zero leaf    → still REFUSED, loud (the rule is
//       unchanged; A1 gave it an ACCURATE diagnostic instead of the caller's
//       generic "shape mismatch or unencodable leaf" text, which named none of
//       the actual cause).
//
// WHY THESE LIVE HERE AND NOT ONLY IN THE CORPUS: an overlapping struct cannot be
// SPELLED in C — the explicit offsets arrive only from a shipped-library
// descriptor — and until this cycle the corpus example declared only LOCALS, so
// nothing reached the static-data encoder for any of the three shapes. These are
// the always-on guards, independent of whether a pe/darwin corpus arm runs on the
// host executing this suite. The runtime twins are
// `tests/mir/test_overlap_struct_zero_init.cpp` (MIR tier) and
// `examples/c/overlap_struct_zero_init/` (end-to-end).
//
// Two further pins guard the machinery the three outcomes rest on, each covering
// code this cycle introduced and nothing else reaches: the `-0.0` arm of the new
// `isAllZeroMirLiteral` (numerically zero, NOT all-zero bytes — the one shape
// where a wrong answer is a SILENT miscompile), and the recursion threading of the
// A1 `why` reason (an overlap one level down must still report accurately).
//
// Every pin asserts BYTES (or a code+message), never a bare "it returned true".

namespace {

// The real `windows.json` shape: `ULARGE_INTEGER {QuadPart u64@0, LowPart u32@0,
// HighPart u32@4}` — both 32-bit halves live INSIDE the 64-bit whole (size 8,
// align 8). Byte-identical to the MIR twin's `ulargeStruct` fixture on purpose:
// the two tiers must be arguing about the SAME type, or "the twins agree" is not
// a statement about anything.
[[nodiscard]] TypeId ulargeOverlayStruct(TypeInterner& ti) {
    TypeId const u64 = ti.primitive(TypeKind::U64);
    TypeId const u32 = ti.primitive(TypeKind::U32);
    std::array<TypeId, 3>        const fields{u64, u32, u32};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 3> const offsets{0, 0, 4};
    return ti.structType("ULARGE_INTEGER", fields, noWidths, offsets);
}

// A DOUBLE overlaid by the integer that reads its bits: `{f64@0, u64@0}` (size 8,
// align 8). The vehicle for the `-0.0` pin — the one initializer that is
// numerically zero but whose OBJECT REPRESENTATION is not all-zero bytes.
[[nodiscard]] TypeId doubleOverlayStruct(TypeInterner& ti) {
    TypeId const f64 = ti.primitive(TypeKind::F64);
    TypeId const u64 = ti.primitive(TypeKind::U64);
    std::array<TypeId, 2>        const fields{f64, u64};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 2> const offsets{0, 0};
    return ti.structType("DoubleOverlay", fields, noWidths, offsets);
}

// A double leaf carrying exactly `d` (so `-0.0` keeps its sign bit — `intField`
// could not express it, and a `0.0` literal would silently lose the distinction).
[[nodiscard]] MirLiteralValue doubleField(double d) {
    MirLiteralValue f;
    f.value = d;
    f.core  = TypeKind::F64;
    return f;
}

// Explicit offsets that are DISJOINT — `{u32@0, u32@8}` → size 12, align 4. A
// foreign layout that simply is not the natural one; nothing about it is
// ambiguous. The NATURAL layout of the same two fields is {0, 4} at size 8, so a
// walk that ignored the declared offsets emits a DIFFERENT byte string of a
// DIFFERENT length — which is what makes the (b) pin a real offset assertion.
[[nodiscard]] TypeId disjointOffsetStruct(TypeInterner& ti) {
    TypeId const u32 = ti.primitive(TypeKind::U32);
    std::array<TypeId, 2>        const fields{u32, u32};
    std::array<std::int64_t, 0>  const noWidths{};
    std::array<std::uint64_t, 2> const offsets{0, 8};
    return ti.structType("Disjoint", fields, noWidths, offsets);
}

} // namespace

// (a) OVERLAP + ALL-ZERO → encodes the FULL declared size as zero bytes.
// RED-ON-DISABLE (MEASURED): restore `if (in.hasExplicitOffsets(ty)) return
// false;` and this errors (1 diagnostic, 0 items) instead of emitting 8 bytes.
// The assertion is the SIZE and the CONTENT together: the buffer is pre-zeroed by
// the caller, so "all zero" alone would be satisfied by an encoder that emitted
// nothing at all — it is `bytes.size() == computeLayout(...)->size` that rejects a
// short (or absent) object, whose tail an aliasing member would read out of the
// next object in the section.
TEST(AsmAggregateGlobal, OverlappingStructAllZeroInitEncodesFullSizeZeroBytes) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const s = ulargeOverlayStruct(ti);
    ASSERT_TRUE(compositeFieldsOverlap(s, ti, kNatural16, DataModel::Lp64))
        << "fixture precondition: these members must ACTUALLY share bytes";
    auto const lay = computeLayout(s, ti, kNatural16, DataModel::Lp64);
    ASSERT_TRUE(lay.has_value());
    ASSERT_EQ(lay->size, 8u) << "fixture precondition: the overlay is 8 bytes";

    auto const r = lowerOneAggGlobal(
        ti, s,
        aggOf({intField(0, TypeKind::U64), intField(0, TypeKind::U32),
               intField(0, TypeKind::U32)}, TypeKind::Struct),
        kNatural16, DataModel::Lp64);

    ASSERT_EQ(r.errors, 0u)
        << "an ALL-ZERO static initializer of an overlapping struct must ENCODE — "
           "zeroed bytes read the same through every aliasing member: " << r.messages;
    ASSERT_EQ(r.items.size(), 1u);
    std::vector<std::uint8_t> const expect(static_cast<std::size_t>(lay->size), 0u);
    EXPECT_EQ(r.items[0].bytes, expect) << "every byte of the object must be zero";
    EXPECT_EQ(r.items[0].bytes.size(), static_cast<std::size_t>(lay->size))
        << "the object must span its FULL declared size, not the first member's width";
    EXPECT_TRUE(r.items[0].relocations.empty())
        << "an all-zero object carries no load-time fixups";
}

// (b) DISJOINT explicit offsets → encodes MEMBER-WISE, each member's bytes at its
// DECLARED offset. RED-ON-DISABLE (MEASURED): restore `if
// (in.hasExplicitOffsets(ty)) return false;` and this errors instead of emitting.
// The byte vector is also an OFFSET assertion, not merely a content one: under the
// NATURAL offsets {0,4} the same two values encode as {44 33 22 11 88 77 66 55}
// (8 bytes), so any walk that derived offsets from alignment rather than reading
// `lay->fieldOffsets` fails here on both length and content.
TEST(AsmAggregateGlobal, DisjointExplicitOffsetStructEncodesAtDeclaredOffsets) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const s = disjointOffsetStruct(ti);
    ASSERT_TRUE(ti.hasExplicitOffsets(s))
        << "fixture precondition: the offsets must be EXPLICIT";
    ASSERT_FALSE(compositeFieldsOverlap(s, ti, kNatural16, DataModel::Lp64))
        << "fixture precondition: and they must NOT overlap";
    auto const lay = computeLayout(s, ti, kNatural16, DataModel::Lp64);
    ASSERT_TRUE(lay.has_value());
    ASSERT_EQ(lay->size, 12u) << "fixture precondition: 0..4 + a 4-byte gap + 8..12";
    ASSERT_EQ(lay->fieldOffsets.size(), 2u);
    EXPECT_EQ(lay->fieldOffsets[0], 0u);
    EXPECT_EQ(lay->fieldOffsets[1], 8u) << "the DECLARED offset, not the natural 4";

    auto const r = lowerOneAggGlobal(
        ti, s,
        aggOf({intField(0x11223344, TypeKind::U32),
               intField(0x55667788, TypeKind::U32)}, TypeKind::Struct),
        kNatural16, DataModel::Lp64);

    ASSERT_EQ(r.errors, 0u)
        << "disjoint explicit offsets are unambiguous — member-wise is correct: "
        << r.messages;
    ASSERT_EQ(r.items.size(), 1u);
    std::vector<std::uint8_t> const expect{
        0x44, 0x33, 0x22, 0x11,          // field 0 @ declared offset 0 (LE)
        0,    0,    0,    0,             // the descriptor's 4-byte hole stays zero
        0x88, 0x77, 0x66, 0x55};         // field 1 @ declared offset 8 (LE)
    EXPECT_EQ(r.items[0].bytes, expect);
}

// (c) OVERLAP + a NON-ZERO leaf → still REFUSED, loud, with the A1 diagnostic.
// `{0, 1, 0}` sets `LowPart = 1`, which aliases `QuadPart`'s low four bytes: a
// positional walk's result would depend on declaration order. Exactly the
// ambiguity the refusal exists for, and it must survive the zero-init relaxation.
// RED-ON-DISABLE, two independent halves (both MEASURED):
//   * the REFUSAL — drop the `!isAllZeroMirLiteral(v)` test (make the overlap arm
//     `return true`) and this emits 8 wrong bytes with 0 errors;
//   * the DIAGNOSTIC — drop the `why = …` assignment and the caller falls back to
//     its generic text, which names f16/f80/f128 and address-relocated leaves,
//     none of which is the cause; the three message assertions below then fail.
TEST(AsmAggregateGlobal, NonZeroInitIntoOverlappingStructFailsLoudWithOverlapReason) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const s = ulargeOverlayStruct(ti);
    auto const r = lowerOneAggGlobal(
        ti, s,
        aggOf({intField(0, TypeKind::U64), intField(1, TypeKind::U32),
               intField(0, TypeKind::U32)}, TypeKind::Struct),
        kNatural16, DataModel::Lp64);

    EXPECT_EQ(r.errors, 1u) << "a non-zero overlapping static init must be REFUSED";
    EXPECT_TRUE(r.items.empty())
        << "a refused initializer must emit NO partial member bytes";
    ASSERT_EQ(r.codes.size(), 1u);
    EXPECT_EQ(r.codes[0], DiagnosticCode::K_NoMatchingObjectFormat);
    EXPECT_NE(r.messages.find("overlapping explicit-offset struct is unsupported"),
              std::string::npos)
        << "the refusal must name the ACTUAL cause: " << r.messages;
    EXPECT_NE(r.messages.find("its members share bytes; assign the members individually"),
              std::string::npos)
        << "…and carry the same remedy the MIR twin gives: " << r.messages;
    EXPECT_EQ(r.messages.find("unencodable leaf"), std::string::npos)
        << "the generic enumerating text names causes this user does NOT have: "
        << r.messages;
}

// `-0.0` — the SILENT-MISCOMPILE arm of the new `isAllZeroMirLiteral` helper, and
// a MATCHED-CONTROL pair: the same struct, the same member, `+0.0` versus `-0.0`.
// `-0.0` compares EQUAL to zero yet its object representation is 0x8000000000000000,
// so treating it as an all-zero fill would emit eight 0x00 bytes and silently drop
// the sign — no diagnostic, wrong data, exactly the failure class the pre-zeroed
// buffer makes easy to fall into. It must be REFUSED; `+0.0` must still encode.
// RED-ON-DISABLE: weaken the helper's double arm to `return *d == 0.0;` (drop the
// `!std::signbit(*d)` conjunct) and the `-0.0` half goes green-with-wrong-bytes,
// which this pin catches as 0 errors + an emitted item. The `+0.0` half is the
// control that keeps the fix from being "refuse all doubles".
TEST(AsmAggregateGlobal, NegativeZeroIntoOverlappingStructIsNotAZeroFill) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const s = doubleOverlayStruct(ti);
    ASSERT_TRUE(compositeFieldsOverlap(s, ti, kNatural16, DataModel::Lp64))
        << "fixture precondition: the double and the integer must share bytes";

    // CONTROL: +0.0 IS all-zero bytes → encodes, eight zero bytes.
    auto const plus = lowerOneAggGlobal(
        ti, s, aggOf({doubleField(0.0), intField(0, TypeKind::U64)}, TypeKind::Struct),
        kNatural16, DataModel::Lp64);
    ASSERT_EQ(plus.errors, 0u) << "+0.0 IS all-zero bytes: " << plus.messages;
    ASSERT_EQ(plus.items.size(), 1u);
    std::vector<std::uint8_t> const zero8(8, 0u);
    EXPECT_EQ(plus.items[0].bytes, zero8);

    // THE PIN: -0.0 is numerically zero but byte 7 is 0x80 → must be REFUSED.
    auto const minus = lowerOneAggGlobal(
        ti, s, aggOf({doubleField(-0.0), intField(0, TypeKind::U64)}, TypeKind::Struct),
        kNatural16, DataModel::Lp64);
    EXPECT_EQ(minus.errors, 1u)
        << "-0.0 has its sign bit SET — a zero-fill would silently drop it";
    EXPECT_TRUE(minus.items.empty())
        << "a refused initializer must emit NO bytes, least of all wrong ones";
    EXPECT_NE(minus.messages.find("overlapping explicit-offset struct is unsupported"),
              std::string::npos)
        << "and it must be refused for the OVERLAP reason: " << minus.messages;
}

// A1, recursion half: the reason is threaded, so an overlapping struct nested as a
// MEMBER of an ordinary struct still reports the accurate cause at the top. Without
// the shared `why` reference this would silently degrade to the generic text — the
// only witness for the threading itself. RED-ON-DISABLE: pass a local `std::string`
// at either recursion site instead of `why` and the specific substring vanishes.
TEST(AsmAggregateGlobal, NestedOverlappingMemberReportsOverlapReasonAtTopLevel) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const inner = ulargeOverlayStruct(ti);
    std::array<TypeId, 1> const f{inner};
    TypeId const outer = ti.structType("Outer", f);
    ASSERT_FALSE(compositeFieldsOverlap(outer, ti, kNatural16, DataModel::Lp64))
        << "fixture precondition: the OUTER struct is naturally laid out — the "
           "overlap is one level down, so only the recursion can find it";

    auto const r = lowerOneAggGlobal(
        ti, outer,
        aggOf({aggOf({intField(0, TypeKind::U64), intField(1, TypeKind::U32),
                      intField(0, TypeKind::U32)}, TypeKind::Struct)},
              TypeKind::Struct),
        kNatural16, DataModel::Lp64);

    EXPECT_EQ(r.errors, 1u);
    EXPECT_TRUE(r.items.empty());
    EXPECT_NE(r.messages.find("overlapping explicit-offset struct is unsupported"),
              std::string::npos)
        << "a nested cause must reach the caller unchanged: " << r.messages;
}

// ── D-CSUBSET-ENUM-GLOBAL-CODEGEN: an ENUM-typed static object emits bytes ──
//
// C 6.7.2.2: an enumerated type has an implementation-defined COMPATIBLE integer
// type, and its object representation IS that integer's. `TypeKind::Enum` is a
// NOMINAL-IDENTITY marker carrying no width of its own — `scalarByteSize(Enum)`
// is nullopt BY CONSTRUCTION, because the kind alone cannot know the width; only
// the enum's `scalars[0]` underlying does.
//
// ⚠ WHAT THESE PINS EXIST FOR: `lowerMirGlobalsToDataItems` used to dispatch on
// the DECLARED kind, so `enum E g = B;` — an utterly ordinary file-scope object
// that gcc 13 -std=c2x and clang 18 -std=c23 both compile AND run — hit the
// scalar arm's "non-primitive global types" refusal instead of emitting four
// bytes. One gate refused EVERY enum-typed static shape at once (scalar,
// tentative, struct member, array element, union member), which is why these
// pins cover all of them: a subset would leave the rest latent (the multi-site
// contract rule). The end-to-end runtime witness is
// `examples/c/enum_typed_global`; these are the BYTE-EXACT pins that example's
// exit code can only summarize, plus the substrate arms no C source can reach.
namespace {

// Lower ONE TENTATIVE (zero-init) module global of `type`, with a chosen
// aggregate-layout block, and return the item + error count. The shipped
// `lowerOneScalarGlobal` is I32-only and always passes `kNatural16`; the arm
// under test here is precisely the one that must NOT need a layout block.
[[nodiscard]] LoweredAgg lowerOneTentativeGlobal(
        TypeInterner const& ti, TypeId type,
        std::optional<AggregateLayoutParams> lp, DataModel dm) {
    MirBuilder b;
    b.addGlobal(type, SymbolId{1}, UINT32_MAX, MirFuncId{}, SymbolBinding::Global,
                SymbolVisibility::Default, /*isConst=*/false,
                MirThreadStorage::Shared);
    Mir const          m = std::move(b).finish();
    DiagnosticReporter rep;
    auto               items = lowerMirGlobalsToDataItems(m, ti, lp, dm, rep);
    std::string                 msgs;
    std::vector<DiagnosticCode> codes;
    for (auto const& d : rep.all()) {
        msgs += d.actual;
        codes.push_back(d.code);
    }
    return {std::move(items), rep.errorCount(), std::move(msgs), std::move(codes)};
}

} // namespace

// A DEFAULT-underlying (`int`) enum scalar global emits its underlying's four
// little-endian bytes at its natural alignment. RED-ON-DISABLE: revert the
// `materialScalarKind` projection at the initialized-global arm and this does not
// merely change bytes — the global is REFUSED outright (errors >= 1, no items),
// which is the exact defect this cycle closes.
TEST(AsmEnumGlobal, ScalarEnumGlobalEmitsItsUnderlyingWidth) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const e = ti.enumType("E", TypeKind::I32);

    MirLiteralValue v;
    v.core  = TypeKind::I32;          // the underlying — the pipeline's own tag
    v.value = std::int64_t{3};
    auto const r = lowerOneAggGlobal(ti, e, std::move(v), kNatural16,
                                     DataModel::Lp64);

    ASSERT_EQ(r.errors, 0u) << r.messages;
    ASSERT_EQ(r.items.size(), 1u);
    std::vector<std::uint8_t> const want{3, 0, 0, 0};
    EXPECT_EQ(r.items[0].bytes, want)
        << "an int-backed enum global is four little-endian bytes of its value";
    EXPECT_EQ(r.items[0].alignment.bytes(), 4u)
        << "natural alignment comes from the UNDERLYING width, not from Enum";
    EXPECT_EQ(r.items[0].section, DataSectionKind::Data)
        << "a mutable initialized enum global is ordinary writable data";

    // ★ THE TYPE DECIDES, NOT THE POOL'S `core` TAG. The literal pool carries a
    // `core` kind alongside the value; the encoder must take its width from the
    // GLOBAL'S TYPE. Re-tagging the identical value `Enum` must not move a byte —
    // if it did, the width would be coming from the tag, and a tag the front end
    // spells differently tomorrow would silently change the image.
    MirLiteralValue tagged;
    tagged.core  = TypeKind::Enum;
    tagged.value = std::int64_t{3};
    auto const rt = lowerOneAggGlobal(ti, e, std::move(tagged), kNatural16,
                                      DataModel::Lp64);
    ASSERT_EQ(rt.errors, 0u) << rt.messages;
    ASSERT_EQ(rt.items.size(), 1u);
    EXPECT_EQ(rt.items[0].bytes, want)
        << "the emitted bytes must depend on the global's TYPE, not the pool tag";
}

// ★ THE WIDTH PIN, and the one a plausible WRONG fix fails. C23 6.7.2.2 lets an
// enum FIX its underlying type (`enum Small : unsigned char`), so "an enum is an
// int" is false: this global is ONE byte, value 200, aligned 1. A fix that mapped
// Enum to I32 unconditionally would emit four bytes here and stay green on every
// default-underlying pin above it.
TEST(AsmEnumGlobal, FixedUnderlyingEnumGlobalEmitsExactlyOneByte) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const small = ti.enumType("Small", TypeKind::U8);

    MirLiteralValue v;
    v.core  = TypeKind::U8;
    v.value = std::uint64_t{200};
    auto const r = lowerOneAggGlobal(ti, small, std::move(v), kNatural16,
                                     DataModel::Lp64);

    ASSERT_EQ(r.errors, 0u) << r.messages;
    ASSERT_EQ(r.items.size(), 1u);
    std::vector<std::uint8_t> const want{200};
    EXPECT_EQ(r.items[0].bytes, want)
        << "an unsigned-char-backed enum is ONE byte — four would silently widen "
           "the object and shift every neighbour that follows it";
    EXPECT_EQ(r.items[0].alignment.bytes(), 1u);
}

// A TENTATIVE (`enum E g;`) enum global reserves its underlying's byte extent in
// .bss with NO on-disk bytes — and, the load-bearing half, WITHOUT an
// `aggregateLayout` block. Before the projection this arm reached the right size
// only through the `computeLayout` fallback, so its correctness silently depended
// on the target declaring a layout block that an integer-sized object has no
// business needing. Passing `std::nullopt` is what makes this pin see that
// difference at all; with `kNatural16` it would be green either way.
TEST(AsmEnumGlobal, TentativeEnumGlobalReservesItsWidthWithNoLayoutBlock) {
    TypeInterner ti{CompilationUnitId{1}};

    auto const wide = lowerOneTentativeGlobal(ti, ti.enumType("E", TypeKind::I32),
                                              /*lp=*/std::nullopt, DataModel::Lp64);
    ASSERT_EQ(wide.errors, 0u) << wide.messages;
    ASSERT_EQ(wide.items.size(), 1u);
    EXPECT_EQ(wide.items[0].section, DataSectionKind::Bss);
    EXPECT_TRUE(wide.items[0].bytes.empty())
        << "a zero-fill global carries NO file bytes — only a reserved extent";
    EXPECT_EQ(wide.items[0].reservedSize, 4u)
        << "the reserved extent is the UNDERLYING integer's width";
    EXPECT_EQ(wide.items[0].alignment.bytes(), 4u);

    // The fixed-underlying twin, same arm: one byte, aligned 1.
    auto const narrow = lowerOneTentativeGlobal(ti, ti.enumType("Small", TypeKind::U8),
                                                /*lp=*/std::nullopt, DataModel::Lp64);
    ASSERT_EQ(narrow.errors, 0u) << narrow.messages;
    ASSERT_EQ(narrow.items.size(), 1u);
    EXPECT_EQ(narrow.items[0].reservedSize, 1u);
    EXPECT_EQ(narrow.items[0].alignment.bytes(), 1u);
}

// The AGGREGATE-LEAF half of the same defect: an enum member/element is a scalar
// leaf, and the leaf encoder asked the same un-sized Enum kind for its width. All
// four composite shapes are here because one gate refused them all. RED-ON-
// DISABLE: revert the projection at `encodeAggregateValue` and every case below
// turns into a refusal ("aggregate initializer could not be encoded"), not merely
// into wrong bytes.
TEST(AsmEnumGlobal, EnumLeavesInsideAggregatesEncodeAtTheirOffsets) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const e     = ti.enumType("E", TypeKind::I32);
    TypeId const small = ti.enumType("Small", TypeKind::U8);
    TypeId const i32   = ti.primitive(TypeKind::I32);
    TypeId const i8    = ti.primitive(TypeKind::I8);

    // (1) STRUCT `{ enum E e; int n; }` = { 3, 40 } — e@0, n@4.
    std::array<TypeId, 2> const pairFields{e, i32};
    TypeId const pair = ti.structType("Pair", pairFields);
    auto const rp = lowerOneAggGlobal(
        ti, pair, aggOf({intField(3, TypeKind::I32), intField(40, TypeKind::I32)},
                        TypeKind::Struct),
        kNatural16, DataModel::Lp64);
    ASSERT_EQ(rp.errors, 0u) << rp.messages;
    ASSERT_EQ(rp.items.size(), 1u);
    std::vector<std::uint8_t> const wantPair{3, 0, 0, 0, 40, 0, 0, 0};
    EXPECT_EQ(rp.items[0].bytes, wantPair)
        << "the enum member occupies its underlying's four bytes at offset 0";

    // (2) ARRAY `enum E[3] = { 5, 3, 1 }` — three distinct values, so a stride
    //     error, a reversed walk, or a repeated element cannot cancel out.
    TypeId const arr = ti.array(e, 3);
    auto const ra = lowerOneAggGlobal(
        ti, arr,
        aggOf({intField(5, TypeKind::I32), intField(3, TypeKind::I32),
               intField(1, TypeKind::I32)}, TypeKind::Array),
        kNatural16, DataModel::Lp64);
    ASSERT_EQ(ra.errors, 0u) << ra.messages;
    ASSERT_EQ(ra.items.size(), 1u);
    std::vector<std::uint8_t> const wantArr{5, 0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0};
    EXPECT_EQ(ra.items[0].bytes, wantArr);

    // (3) UNION `{ enum E e; int n; }` = { 5 } — the first member is written and
    //     the union's remaining bytes stay zero.
    std::array<TypeId, 2> const uniFields{e, i32};
    TypeId const uni = ti.unionType("U", uniFields);
    auto const ru = lowerOneAggGlobal(
        ti, uni, aggOf({intField(5, TypeKind::I32)}, TypeKind::Union),
        kNatural16, DataModel::Lp64);
    ASSERT_EQ(ru.errors, 0u) << ru.messages;
    ASSERT_EQ(ru.items.size(), 1u);
    std::vector<std::uint8_t> const wantUni{5, 0, 0, 0};
    EXPECT_EQ(ru.items[0].bytes, wantUni);

    // (4) ★ THE NARROW LEAF, and it is the case that cannot fail QUIETLY.
    //     `{ signed char tag; enum Small s; }` is TWO bytes — tag@0, s@1. A leaf
    //     that assumed four bytes for the enum would run off the item's own byte
    //     extent, which the layout-vs-encoder bounds check refuses LOUD. So this
    //     pin discriminates the width in both directions: right bytes, or a
    //     diagnostic — never a quietly widened object.
    std::array<TypeId, 2> const packedFields{i8, small};
    TypeId const packed = ti.structType("Packed", packedFields);
    auto const layout = computeLayout(packed, ti, kNatural16, DataModel::Lp64);
    ASSERT_TRUE(layout.has_value());
    ASSERT_EQ(layout->size, 2u)
        << "fixture precondition: a u8-backed enum member makes this struct 2 bytes";
    auto const rk = lowerOneAggGlobal(
        ti, packed,
        aggOf({intField(2, TypeKind::I8), intField(200, TypeKind::U8)},
              TypeKind::Struct),
        kNatural16, DataModel::Lp64);
    ASSERT_EQ(rk.errors, 0u) << rk.messages;
    ASSERT_EQ(rk.items.size(), 1u);
    std::vector<std::uint8_t> const wantPacked{2, 200};
    EXPECT_EQ(rk.items[0].bytes, wantPacked);
}

// FAIL LOUD ON A MALFORMED RECORD — never a guessed width. An Enum whose
// underlying scalar is outside the core kind range is not something a C source
// can produce; it is what a corrupted or incompletely-built type record looks
// like. The projection must hand such a record back UNCHANGED so the existing
// refusal fires, rather than invent a width for it.
//
// ★ RED-ON-DISABLE, AND THE MUTATION HAD TO BE CHOSEN CAREFULLY — ✔MEASURED,
// because the obvious one leaves this pin GREEN. Deleting the whole projection
// reddens the four pins above but NOT this one: with no projection every enum
// refuses, including this one, for the reason the pin asserts. Deleting only the
// RANGE CHECK also leaves it green: `TypeKind`'s underlying type is fixed
// (`std::uint16_t`), so an out-of-range scalar still lands on a kind
// `scalarByteSize` has no arm for, and the refusal still fires. What this pin
// actually guards is the choice between REFUSING and GUESSING — so its mutation
// is `return TypeKind::I32;` in place of `return k;`, i.e. the naive "an enum is
// an int" fallback. ✔MEASURED RED under exactly that mutant (4 bytes emitted,
// zero errors), and it is a realistic wrong implementation rather than a
// contrived one: `FixedUnderlyingEnumGlobalEmitsExactlyOneByte` catches the same
// mistake from the well-formed side, this one from the malformed side.
TEST(AsmEnumGlobal, AnEnumWithAnOutOfRangeUnderlyingFailsLoud) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const bad = ti.enumType("Bad", static_cast<TypeKind>(9999));

    MirLiteralValue v;
    v.core  = TypeKind::I32;
    v.value = std::int64_t{3};
    auto const r = lowerOneAggGlobal(ti, bad, std::move(v), kNatural16,
                                     DataModel::Lp64);

    EXPECT_GE(r.errors, 1u)
        << "an enum with no usable underlying must be refused, not sized by guess";
    EXPECT_TRUE(r.items.empty()) << "a refused global emits NO bytes";
    EXPECT_NE(r.messages.find("non-primitive global types"), std::string::npos)
        << "and it reaches the pre-existing scalar refusal: " << r.messages;
}

// ══ D-CSUBSET-COMPLEX-STATIC-STORAGE-INITIALIZER-HAS-NO-CONSTANT-IMAGE ═════════
// ★★ A `_Complex` STATIC INITIALIZER EMITS A TWO-COMPONENT IMAGE — real at 0,
//    imaginary at the ELEMENT SIZE.
//
// C 6.2.5p13 lays a complex out exactly like an array of two element-floats, and
// `computeLayout`'s Complex arm realizes that as `StructLayout{es * 2, elem->align}`
// — the imaginary component at exactly `es`, NOT at the align-rounded stride the
// ARRAY arm uses. Copying the array arm would be invisible on F32/F64 (size ==
// align) and wrong on any element where they differ, which is the class of defect
// that surfaces one cycle after the gate it passed.
//
// ✔MEASURED at 301e2a63: every one of these refused with
// `error[K_NoMatchingObjectFormat] … has a runtime initializer (__module_init__-
// driven)`, because a complex initializer had no constant-image path at all and fell
// to a load-time store-chain the producer does not emit. gcc 13.3.0 (`-std=c2x`) and
// clang 18.1.3 (`-std=c23`), probed SEPARATELY, compile and run all of them.
//
// ★ THE IMAGINARY HALF IS WHAT EVERY CASE IS BUILT TO CATCH. A half-emitted image
// (real right, imaginary zero) is a SILENT MISCOMPILE, not a refusal — nothing
// recomputes the value at load. So no case below has a zero imaginary component
// except the one that MUST (the real→complex promotion), and the two components are
// never equal, so a producer that wrote `re` twice, swapped them, or dropped `im`
// reddens on the bytes rather than on a count.
//
// RED-ON-DISABLE: delete the `TypeKind::Complex` arm from `encodeAggregateValue` and
// every case refuses (the generic aggregate text); place the imaginary component at
// the align-rounded stride instead of `elemLay->size` and the F32 case still passes
// while a hypothetical wider element would not — which is why the F32 case pins the
// OFFSET explicitly rather than trusting the F64 one.
TEST(AsmDataSection, ComplexGlobalEmitsTwoComponentImageAtElementOffsets) {
    TypeInterner ti{CompilationUnitId{1}};
    auto const f64c = ti.complex(ti.primitive(TypeKind::F64));
    auto const f32c = ti.complex(ti.primitive(TypeKind::F32));
    auto const comp = [](double v, TypeKind k) {
        MirLiteralValue l;
        l.value = v;
        l.core  = k;
        return l;
    };

    // (a) F64 elements, BOTH components non-zero and DISTINCT — (3.0, 4.0).
    //     16 bytes: 3.0 little-endian at 0, 4.0 little-endian at 8.
    auto const ra = lowerOneAggGlobal(
        ti, f64c,
        aggOf({comp(3.0, TypeKind::F64), comp(4.0, TypeKind::F64)},
              TypeKind::Complex),
        kNatural16, DataModel::Lp64);
    ASSERT_EQ(ra.errors, 0u) << ra.messages;
    ASSERT_EQ(ra.items.size(), 1u);
    EXPECT_EQ(ra.items[0].bytes,
              (std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x40,
                                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x40}))
        << "real 3.0 at byte 0, imaginary 4.0 at byte 8 — the exact image clang "
           "emits for `double _Complex g = __builtin_complex(3.0, 4.0);`";
    EXPECT_EQ(ra.items[0].alignment.bytes(), 8u)
        << "a complex aligns as its ELEMENT does, never as its 16-byte size";

    // (b) F32 elements — 8 bytes total, and the imaginary component starts at byte
    //     FOUR. This is the offset assertion: it is `elemLay->size`, the layout
    //     authority's own formula, not a stride derived some other way.
    auto const rb = lowerOneAggGlobal(
        ti, f32c,
        aggOf({comp(1.5, TypeKind::F32), comp(2.5, TypeKind::F32)},
              TypeKind::Complex),
        kNatural16, DataModel::Lp64);
    ASSERT_EQ(rb.errors, 0u) << rb.messages;
    ASSERT_EQ(rb.items.size(), 1u);
    EXPECT_EQ(rb.items[0].bytes,
              (std::vector<std::uint8_t>{0x00, 0x00, 0xc0, 0x3f,
                                         0x00, 0x00, 0x20, 0x40}))
        << "F32 components: 1.5f at byte 0, 2.5f at byte 4";
    EXPECT_EQ(rb.items[0].alignment.bytes(), 4u);

    // (c) NEGATIVE imaginary — the sign must survive into the image. `conj(3+4i)`
    //     is (3.0, -4.0), and gcc folds exactly this in a static initializer.
    auto const rc = lowerOneAggGlobal(
        ti, f64c,
        aggOf({comp(3.0, TypeKind::F64), comp(-4.0, TypeKind::F64)},
              TypeKind::Complex),
        kNatural16, DataModel::Lp64);
    ASSERT_EQ(rc.errors, 0u) << rc.messages;
    ASSERT_EQ(rc.items.size(), 1u);
    EXPECT_EQ(rc.items[0].bytes,
              (std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x40,
                                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0xc0}));

    // (d) A ONE-COMPONENT value — the real→complex promotion `double _Complex g =
    //     7.0;`. The imaginary half is zero BY CONSTRUCTION (the caller pre-zeroes
    //     the buffer to the layout size), which is the C 6.3.1.7 answer, and the
    //     image is still the full 16 bytes rather than a short 8.
    auto const rd = lowerOneAggGlobal(
        ti, f64c, aggOf({comp(7.0, TypeKind::F64)}, TypeKind::Complex),
        kNatural16, DataModel::Lp64);
    ASSERT_EQ(rd.errors, 0u) << rd.messages;
    ASSERT_EQ(rd.items.size(), 1u);
    EXPECT_EQ(rd.items[0].bytes,
              (std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x40,
                                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));

    // (e) FAIL LOUD, never partial. THREE components is a shape the type cannot
    //     hold; writing the first two and dropping the third is precisely the
    //     silent half-emit this arm exists to prevent.
    auto const re = lowerOneAggGlobal(
        ti, f64c,
        aggOf({comp(1.0, TypeKind::F64), comp(2.0, TypeKind::F64),
               comp(3.0, TypeKind::F64)},
              TypeKind::Complex),
        kNatural16, DataModel::Lp64);
    EXPECT_GT(re.errors, 0u)
        << "an over-long complex initializer must REFUSE, not truncate";
    EXPECT_NE(re.messages.find(
                  "D-CSUBSET-COMPLEX-STATIC-STORAGE-INITIALIZER-HAS-NO-CONSTANT-IMAGE"),
              std::string::npos)
        << "the refusal must name its own anchor, not the generic aggregate text";

    // (f) A SCALAR leaf where a two-component value is owed — the other half of the
    //     same wall, and the arm that would otherwise silently write 8 of 16 bytes.
    MirLiteralValue bare;
    bare.value = 3.0;
    bare.core  = TypeKind::F64;
    auto const rf = lowerOneAggGlobal(ti, f64c, std::move(bare), kNatural16,
                                      DataModel::Lp64);
    EXPECT_GT(rf.errors, 0u)
        << "a scalar leaf cannot carry both complex components";
}
