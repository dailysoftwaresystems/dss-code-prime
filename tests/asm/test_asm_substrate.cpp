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
#include "lir/lir.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "lowered_lir_fixture.hpp"
#include "mir/mir_node.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

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
            { "name": "rel32",  "kind": 1, "formula": "S + A - P - 4" },
            { "name": "abs64",  "kind": 2, "formula": "S + A"         }
        ]
    })";
    auto schema = TargetSchema::loadFromText(kJson, "synth.target.json");
    ASSERT_TRUE(schema.has_value());
    EXPECT_EQ((*schema)->relocationCount(), 2u);

    auto const* rel32 = (*schema)->relocationByName("rel32");
    ASSERT_NE(rel32, nullptr);
    EXPECT_EQ(rel32->kind, RelocationKind{1});
    EXPECT_EQ(rel32->formula, "S + A - P - 4");

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

// ── Diagnostic surface: A_* renders with the `A` prefix ───────────────

TEST(AsmDiagnostics, AnNibbleRendersAsLetterA) {
    // The 0x1xxx high-nibble allocation maps to the letter 'A' via
    // diagnosticCodePrefix's switch. Pinning this here keeps plan 00
    // §0.3 + parse_diagnostic.cpp + this test triangulated.
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::A_NoEncodingDeclared),     "A0001");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::A_NoEncodingShapeWalker),  "A0002");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::A_LirToMirSizeMismatch),   "A0003");
}
