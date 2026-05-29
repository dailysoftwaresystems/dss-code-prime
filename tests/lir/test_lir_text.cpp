// LIR text emitter tests (ML8 cycle 1). Pins:
//   * Preamble: `dsslir 1` + `target <name>`
//   * Symbols section: every reachable id auto-declared (named or
//     synthetic); ctx-provided names take precedence
//   * Literal pool inline (every entry carries a `core <TypeKind>` tag)
//   * Module {function {block {inst}}} structure
//   * Operand kinds: Reg (phys mnemonic / virtual %v.N), ImmInt (#N),
//     BlockRef (^bN), SymbolRef (@N), MemBase (*scale), MemOffset
//     (+offset / -offset), LiteralIndex (lit#N), None (_)
//   * Block headers carry [entry] flag + successor list
//   * Instruction always emits `; payload=N flags=M` (lossless)
//   * Phys-reg OOR ordinal → L_PhysRegOrdinalOutOfRange warning
//
// Round-trip parsing + verifier-on-load lands in ML8 cycle 2.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "lir/lir.hpp"
#include "lir/lir_literal_pool.hpp"
#include "lir/lir_text.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] std::shared_ptr<TargetSchema> shippedX86() {
    auto target = TargetSchema::loadShipped("x86_64");
    if (!target) { ADD_FAILURE() << "loadShipped(x86_64) failed"; std::abort(); }
    return *target;
}

// Hand-build a minimal one-function LIR for golden-output tests.
// Keeps the corpus deterministic: no regalloc / no rewrite / no
// callconv interleave.
[[nodiscard]] Lir buildOneInstFunction(TargetSchema const& sch,
                                       std::uint16_t opMov,
                                       std::uint16_t opRet) {
    LirBuilder b{sch};
    b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const rax = makePhysicalReg(0, LirRegClass::GPR);  // x86_64 rax
    std::array<LirOperand, 1> movOps{LirOperand::makeImmInt32(42)};
    b.addInst(opMov, rax, movOps);
    std::array<LirOperand, 1> retOps{LirOperand::makeReg(rax)};
    b.addReturn(opRet, retOps);
    return std::move(b).finish();
}

} // namespace

TEST(LirText, EmitterPreambleCarriesVersionAndTargetName) {
    auto sch = shippedX86();
    LirBuilder b{*sch};
    Lir empty = std::move(b).finish();
    DiagnosticReporter rep;
    LirTextContext ctx;
    std::string const text = emitLir(empty, *sch, ctx, rep);
    EXPECT_NE(text.find("dsslir 1\n"), std::string::npos);
    EXPECT_NE(text.find("target x86_64\n"), std::string::npos);
}

TEST(LirText, EmitterEmptyModuleProducesValidStructure) {
    auto sch = shippedX86();
    LirBuilder b{*sch};
    Lir empty = std::move(b).finish();
    DiagnosticReporter rep;
    LirTextContext ctx;
    std::string const text = emitLir(empty, *sch, ctx, rep);
    EXPECT_NE(text.find("symbols {"), std::string::npos);
    EXPECT_NE(text.find("literal_pool {"), std::string::npos);
    EXPECT_NE(text.find("module {"), std::string::npos);
}

TEST(LirText, EmitterRendersPhysicalRegByMnemonic) {
    auto sch = shippedX86();
    auto const movOp = sch->opcodeByMnemonic("mov");
    auto const retOp = sch->opcodeByMnemonic("ret");
    ASSERT_TRUE(movOp.has_value());
    ASSERT_TRUE(retOp.has_value());
    Lir const lir = buildOneInstFunction(*sch, *movOp, *retOp);
    DiagnosticReporter rep;
    LirTextContext ctx;
    std::string const text = emitLir(lir, *sch, ctx, rep);
    EXPECT_NE(text.find("rax = mov #42"), std::string::npos)
        << "physical reg ordinal 0 should render as `rax` (x86_64 schema)";
    EXPECT_NE(text.find("ret rax"), std::string::npos);
}

TEST(LirText, EmitterRendersVirtualRegAsVDotId) {
    auto sch = shippedX86();
    LirBuilder b{*sch};
    b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const v1 = b.newVReg(LirRegClass::GPR);
    auto const movOp = sch->opcodeByMnemonic("mov");
    auto const retOp = sch->opcodeByMnemonic("ret");
    ASSERT_TRUE(movOp.has_value());
    ASSERT_TRUE(retOp.has_value());
    std::array<LirOperand, 1> movOps{LirOperand::makeImmInt32(7)};
    b.addInst(*movOp, v1, movOps);
    std::array<LirOperand, 1> retOps{LirOperand::makeReg(v1)};
    b.addReturn(*retOp, retOps);
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    LirTextContext ctx;
    std::string const text = emitLir(lir, *sch, ctx, rep);
    EXPECT_NE(text.find("%v.1 = mov #7"), std::string::npos)
        << "virtual reg id 1 should render as `%v.1`";
    EXPECT_NE(text.find("ret %v.1"), std::string::npos);
}

TEST(LirText, EmitterRendersFprPhysReg) {
    auto sch = shippedX86();
    auto const movOp = sch->opcodeByMnemonic("mov");
    auto const retOp = sch->opcodeByMnemonic("ret");
    ASSERT_TRUE(movOp.has_value());
    ASSERT_TRUE(retOp.has_value());
    LirBuilder b{*sch};
    b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    // First FPR ordinal on x86_64 (xmm0) — confirms class-blind name
    // resolution via the schema register table.
    auto const xmm0Ord = sch->registerByName("xmm0");
    ASSERT_TRUE(xmm0Ord.has_value());
    LirReg const xmm0 = makePhysicalReg(*xmm0Ord, LirRegClass::FPR);
    std::array<LirOperand, 1> movOps{LirOperand::makeImmInt32(0)};
    b.addInst(*movOp, xmm0, movOps);
    b.addReturn(*retOp, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    LirTextContext ctx;
    std::string const text = emitLir(lir, *sch, ctx, rep);
    EXPECT_NE(text.find("xmm0 = mov #0"), std::string::npos)
        << "FPR-class phys reg renders by mnemonic, same path as GPR";
}

TEST(LirText, EmitterRendersBlockHeadersWithEntryFlagAndSuccessors) {
    auto sch = shippedX86();
    auto const jmpOp = sch->opcodeByMnemonic("jmp");
    auto const retOp = sch->opcodeByMnemonic("ret");
    ASSERT_TRUE(jmpOp.has_value());
    ASSERT_TRUE(retOp.has_value());
    LirBuilder b{*sch};
    b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    LirBlockId const tail  = b.createBlock();
    b.beginBlock(entry);
    b.addBr(*jmpOp, tail);
    b.beginBlock(tail);
    b.addReturn(*retOp, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    LirTextContext ctx;
    std::string const text = emitLir(lir, *sch, ctx, rep);
    EXPECT_NE(text.find("block ^b0 [entry] -> [^b1]"), std::string::npos);
    EXPECT_NE(text.find("block ^b1 -> []"), std::string::npos);
}

TEST(LirText, EmitterRendersAllOperandKindSigils) {
    auto sch = shippedX86();
    auto const loadOp = sch->opcodeByMnemonic("load");
    auto const retOp  = sch->opcodeByMnemonic("ret");
    ASSERT_TRUE(loadOp.has_value());
    ASSERT_TRUE(retOp.has_value());
    LirBuilder b{*sch};
    b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const result = makePhysicalReg(0, LirRegClass::GPR);
    LirReg const base   = makePhysicalReg(4, LirRegClass::GPR);  // rsp
    std::array<LirOperand, 3> ops{
        LirOperand::makeReg(base),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(-16)
    };
    b.addInst(*loadOp, result, ops);
    b.addReturn(*retOp, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    LirTextContext ctx;
    std::string const text = emitLir(lir, *sch, ctx, rep);
    // base reg (rsp), MemBase (*1), MemOffset (-16) — negative offset
    // renders WITHOUT the `+` sign so the sigil pair `*1, -16` is
    // unambiguous on parse.
    EXPECT_NE(text.find("*1"), std::string::npos);
    EXPECT_NE(text.find("-16"), std::string::npos);
    // positive offset case
    LirBuilder b2{*sch};
    b2.addFunction(SymbolId{2});
    LirBlockId const e2 = b2.createBlock();
    b2.beginBlock(e2);
    std::array<LirOperand, 3> ops2{
        LirOperand::makeReg(base),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(24)
    };
    b2.addInst(*loadOp, result, ops2);
    b2.addReturn(*retOp, std::span<LirOperand const>{});
    Lir lir2 = std::move(b2).finish();
    std::string const text2 = emitLir(lir2, *sch, ctx, rep);
    EXPECT_NE(text2.find("+24"), std::string::npos);
}

TEST(LirText, EmitterRendersSymbolRefSigil) {
    auto sch = shippedX86();
    auto const callOp = sch->opcodeByMnemonic("call");
    auto const retOp  = sch->opcodeByMnemonic("ret");
    ASSERT_TRUE(callOp.has_value());
    ASSERT_TRUE(retOp.has_value());
    LirBuilder b{*sch};
    b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    std::array<LirOperand, 1> ops{LirOperand::makeSymbolRef(7)};
    b.addInst(*callOp, InvalidLirReg, ops);
    b.addReturn(*retOp, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    LirTextContext ctx;
    std::string const text = emitLir(lir, *sch, ctx, rep);
    EXPECT_NE(text.find("call @7"), std::string::npos)
        << "SymbolRef operand should render as `@<id>`";
    // And the reachable-symbol sweep must declare it in the preamble.
    EXPECT_NE(text.find("%7 <synthetic>"), std::string::npos)
        << "out-of-ctx symbol id must be auto-declared in symbols { }";
}

TEST(LirText, EmitterRendersNoneSigilForBlankPaddingOperand) {
    auto sch = shippedX86();
    auto const movOp = sch->opcodeByMnemonic("mov");
    auto const retOp = sch->opcodeByMnemonic("ret");
    ASSERT_TRUE(movOp.has_value());
    ASSERT_TRUE(retOp.has_value());
    LirBuilder b{*sch};
    b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const rax = makePhysicalReg(0, LirRegClass::GPR);
    // `None`-kind operands exist in default-constructed LirOperand —
    // emit one explicitly to pin the `_` sigil.
    std::array<LirOperand, 2> ops{LirOperand{}, LirOperand::makeImmInt32(0)};
    b.addInst(*movOp, rax, ops);
    b.addReturn(*retOp, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    LirTextContext ctx;
    std::string const text = emitLir(lir, *sch, ctx, rep);
    EXPECT_NE(text.find("mov _, #0"), std::string::npos)
        << "None operand should render as `_`";
}

TEST(LirText, EmitterAlwaysEmitsPayloadAndFlags) {
    auto sch = shippedX86();
    auto const frameLoadOp = sch->opcodeByMnemonic(sch->frameLoadMnemonic());
    auto const retOp       = sch->opcodeByMnemonic("ret");
    ASSERT_TRUE(frameLoadOp.has_value());
    ASSERT_TRUE(retOp.has_value());
    LirBuilder b{*sch};
    b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const result = makePhysicalReg(0, LirRegClass::GPR);
    b.addInst(*frameLoadOp, result, std::span<LirOperand const>{},
              /*payload=*/3, /*flags=*/2);
    b.addReturn(*retOp, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    LirTextContext ctx;
    std::string const text = emitLir(lir, *sch, ctx, rep);
    EXPECT_NE(text.find("payload=3 flags=2"), std::string::npos)
        << "non-zero payload + flags should be rendered together";
}

TEST(LirText, EmitterEmitsZeroPayloadAndFlagsLosslessly) {
    // `TargetCondCode::Eq == 0` — a CondBr with Eq has payload=0 that
    // MUST round-trip, hence unconditional payload emission. Same
    // discipline for flags.
    auto sch = shippedX86();
    auto const movOp = sch->opcodeByMnemonic("mov");
    auto const retOp = sch->opcodeByMnemonic("ret");
    ASSERT_TRUE(movOp.has_value());
    ASSERT_TRUE(retOp.has_value());
    Lir const lir = buildOneInstFunction(*sch, *movOp, *retOp);

    DiagnosticReporter rep;
    LirTextContext ctx;
    std::string const text = emitLir(lir, *sch, ctx, rep);
    EXPECT_NE(text.find("payload=0 flags=0"), std::string::npos)
        << "zero payload + flags must still be emitted (CondCode::Eq=0 round-trip)";
}

TEST(LirText, EmitterMultiFunctionModuleRendersEach) {
    auto sch = shippedX86();
    auto const movOp = sch->opcodeByMnemonic("mov");
    auto const retOp = sch->opcodeByMnemonic("ret");
    ASSERT_TRUE(movOp.has_value());
    ASSERT_TRUE(retOp.has_value());
    LirBuilder b{*sch};
    // function 1
    b.addFunction(SymbolId{1});
    LirBlockId const e1 = b.createBlock();
    b.beginBlock(e1);
    LirReg const rax = makePhysicalReg(0, LirRegClass::GPR);
    std::array<LirOperand, 1> mov1{LirOperand::makeImmInt32(1)};
    b.addInst(*movOp, rax, mov1);
    std::array<LirOperand, 1> ret1{LirOperand::makeReg(rax)};
    b.addReturn(*retOp, ret1);
    // function 2
    b.addFunction(SymbolId{2});
    LirBlockId const e2 = b.createBlock();
    b.beginBlock(e2);
    std::array<LirOperand, 1> mov2{LirOperand::makeImmInt32(2)};
    b.addInst(*movOp, rax, mov2);
    std::array<LirOperand, 1> ret2{LirOperand::makeReg(rax)};
    b.addReturn(*retOp, ret2);
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    LirTextContext ctx;
    std::string const text = emitLir(lir, *sch, ctx, rep);
    EXPECT_NE(text.find("function %1"), std::string::npos);
    EXPECT_NE(text.find("function %2"), std::string::npos);
    EXPECT_NE(text.find("mov #1"), std::string::npos);
    EXPECT_NE(text.find("mov #2"), std::string::npos);
}

TEST(LirText, EmitterSymbolsSectionRespectsCtx) {
    auto sch = shippedX86();
    auto const movOp = sch->opcodeByMnemonic("mov");
    auto const retOp = sch->opcodeByMnemonic("ret");
    Lir const lir = buildOneInstFunction(*sch, *movOp, *retOp);

    DiagnosticReporter rep;
    std::vector<std::string> const names{"", "main"};  // slot 0 invalid, slot 1 named
    LirTextContext ctx{};
    ctx.symbolNames = std::span<std::string const>{names};
    std::string const text = emitLir(lir, *sch, ctx, rep);
    // Inline name on the function header (renderSymbol fix).
    EXPECT_NE(text.find("function %1 \"main\" {"), std::string::npos)
        << "function header must inline the symbol name when ctx supplies it";
    // Symbols section also carries the named entry.
    EXPECT_NE(text.find("  %1 \"main\"\n"), std::string::npos);
}

TEST(LirText, EmitterAutoDeclaresOutOfRangeSymbolInPreamble) {
    auto sch = shippedX86();
    auto const movOp = sch->opcodeByMnemonic("mov");
    auto const retOp = sch->opcodeByMnemonic("ret");
    // Use symbol id 5 — beyond a 2-element ctx.symbolNames vector.
    LirBuilder b{*sch};
    b.addFunction(SymbolId{5});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const rax = makePhysicalReg(0, LirRegClass::GPR);
    std::array<LirOperand, 1> ops{LirOperand::makeImmInt32(0)};
    b.addInst(*movOp, rax, ops);
    b.addReturn(*retOp, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    std::vector<std::string> const names{"", "main"};  // only 0..1
    LirTextContext ctx{};
    ctx.symbolNames = std::span<std::string const>{names};
    std::string const text = emitLir(lir, *sch, ctx, rep);
    EXPECT_NE(text.find("%5 <synthetic>"), std::string::npos)
        << "symbol referenced past symbolNames must be auto-declared in preamble "
           "so cycle-2 parser never sees an unbound `%N`";
}

TEST(LirText, EmitterWarnsOnOutOfRangePhysReg) {
    auto sch = shippedX86();
    auto const movOp = sch->opcodeByMnemonic("mov");
    auto const retOp = sch->opcodeByMnemonic("ret");
    LirBuilder b{*sch};
    b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    // Forge an out-of-range phys ordinal — far beyond x86_64's 33-entry
    // register table. The emitter must surface this as a warning, NOT
    // silently produce a `phys#9999` token cycle-2 has to special-case.
    LirReg const bogus = makePhysicalReg(9999, LirRegClass::GPR);
    std::array<LirOperand, 1> ops{LirOperand::makeImmInt32(0)};
    b.addInst(*movOp, bogus, ops);
    b.addReturn(*retOp, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    LirTextContext ctx;
    std::string const text = emitLir(lir, *sch, ctx, rep);
    EXPECT_NE(text.find("phys#9999"), std::string::npos);
    bool warned = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::L_PhysRegOrdinalOutOfRange) warned = true;
    }
    EXPECT_TRUE(warned)
        << "OOR phys ordinal must emit L_PhysRegOrdinalOutOfRange (NOT the "
           "lowering-coverage code — register-table integrity is a distinct "
           "failure class)";
}

TEST(LirText, EmitterRendersLiteralPoolBodyWithCoreTag) {
    auto sch = shippedX86();
    auto const movOp = sch->opcodeByMnemonic("mov");
    auto const retOp = sch->opcodeByMnemonic("ret");
    LirBuilder b{*sch};
    // Pool entries must be appended BEFORE the function is opened so
    // the literal pool persists through the rest of the build.
    LirLiteralValue intLit;
    intLit.value = std::int64_t{0xDEADBEEFLL};
    intLit.core  = TypeKind::I64;
    std::uint32_t const intIdx = b.literalPoolAdd(std::move(intLit));

    LirAggregateValue agg;
    LirLiteralValue f0;
    f0.value = std::int64_t{1};
    f0.core  = TypeKind::I64;
    LirLiteralValue f1;
    f1.value = std::string{"hi"};
    f1.core  = TypeKind::Array;  // Char array stand-in
    agg.fields.push_back(std::move(f0));
    agg.fields.push_back(std::move(f1));
    LirLiteralValue aggLit;
    aggLit.value = std::move(agg);
    aggLit.core  = TypeKind::Struct;
    std::uint32_t const aggIdx = b.literalPoolAdd(std::move(aggLit));

    b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const rax = makePhysicalReg(0, LirRegClass::GPR);
    std::array<LirOperand, 1> ops{LirOperand::makeLiteralIndex(intIdx)};
    b.addInst(*movOp, rax, ops);
    b.addReturn(*retOp, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    LirTextContext ctx;
    std::string const text = emitLir(lir, *sch, ctx, rep);
    EXPECT_NE(text.find(std::format("lit#{} = i64 3735928559 core I64", intIdx)),
              std::string::npos)
        << "int literal pool entry must round-trip value + core TypeKind tag";
    EXPECT_NE(
        text.find(std::format(
            "lit#{} = agg [i64 1 core I64, str \"hi\" core Array] core Struct",
            aggIdx)),
        std::string::npos)
        << "aggregate literal must render fields recursively (NOT `<aggregate>`) "
           "with the core tag on every field AND the outer entry";
    EXPECT_NE(text.find(std::format("mov lit#{}", intIdx)), std::string::npos)
        << "LiteralIndex operand should render as `lit#<N>`";
}
