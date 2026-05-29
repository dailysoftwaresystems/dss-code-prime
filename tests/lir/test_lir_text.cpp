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
    EXPECT_NE(text.find("target x86_64 version \""), std::string::npos)
        << "preamble must carry both the target name AND its semantic version "
           "(D-ML8-1.2 fold — version pinned so cross-bump load is rejected)";
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
    EXPECT_NE(text.find("%v.1:gpr = mov #7"), std::string::npos)
        << "virtual reg id 1 should render as `%v.1:gpr` (class-tagged for "
           "lossless round-trip of FPR/VR vregs)";
    EXPECT_NE(text.find("ret %v.1:gpr"), std::string::npos);
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
    // Synthetic form is the bare `  %N\n` line (no name string).
    EXPECT_NE(text.find("  %7\n"), std::string::npos)
        << "out-of-ctx symbol id must be auto-declared in symbols { } as bare %N "
           "(synthetic form — no string after the handle)";
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
    EXPECT_NE(text.find("  %5\n"), std::string::npos)
        << "symbol referenced past symbolNames must be auto-declared in preamble "
           "as a bare `%N` synthetic entry so cycle-2 parser never sees an "
           "unbound symbol";
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

// ═════════════════════════════════════════════════════════════════════
// Parser + round-trip tests (ML8 cycle 2 — D-ML8-1.1 fold).
// Contract:
//   emitLir(parseLir(emitLir(m))->lir) == emitLir(m)   (byte-identical)
// ═════════════════════════════════════════════════════════════════════

namespace {

// Wrapper that emits → parses → re-emits and asserts byte-identity.
// Returns the first-emit text so the caller can additionally check
// shape (specific format pins).
[[nodiscard]] std::string
roundTripOrFail(Lir const& lir, TargetSchema const& sch,
                LirTextContext const& ctx, char const* what) {
    DiagnosticReporter rep1, rep2, rep3;
    std::string const text1 = emitLir(lir, sch, ctx, rep1);
    auto result = parseLir(text1, sch, rep2);
    EXPECT_TRUE(result->ok)
        << "round-trip parse failed for " << what
        << " — first-emit text follows:\n" << text1;
    // Build a NEW ctx for the re-emit using the parsed symbol-name
    // table — same shape the parser surfaces to its consumer.
    LirTextContext ctxRe{};
    ctxRe.symbolNames = std::span<std::string const>{result->symbolNames};
    std::string const text2 = emitLir(result->lir, sch, ctxRe, rep3);
    EXPECT_EQ(text1, text2)
        << "round-trip text drift for " << what
        << "\n--- first emit ---\n" << text1
        << "\n--- second emit ---\n" << text2;
    return text1;
}

} // namespace

TEST(LirTextRoundTrip, EmptyModule) {
    auto sch = shippedX86();
    LirBuilder b{*sch};
    Lir empty = std::move(b).finish();
    LirTextContext ctx;
    (void)roundTripOrFail(empty, *sch, ctx, "empty module");
}

TEST(LirTextRoundTrip, SingleFunctionAllPhysicalRegs) {
    auto sch = shippedX86();
    auto const movOp = sch->opcodeByMnemonic("mov");
    auto const retOp = sch->opcodeByMnemonic("ret");
    Lir const lir = buildOneInstFunction(*sch, *movOp, *retOp);
    std::vector<std::string> const names{"", "main"};
    LirTextContext ctx{};
    ctx.symbolNames = std::span<std::string const>{names};
    (void)roundTripOrFail(lir, *sch, ctx, "single-fn phys regs + symbol name");
}

TEST(LirTextRoundTrip, VirtualRegsRoundTrip) {
    auto sch = shippedX86();
    auto const movOp = sch->opcodeByMnemonic("mov");
    auto const retOp = sch->opcodeByMnemonic("ret");
    LirBuilder b{*sch};
    b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const v1 = b.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 1> mov{LirOperand::makeImmInt32(7)};
    b.addInst(*movOp, v1, mov);
    std::array<LirOperand, 1> ret{LirOperand::makeReg(v1)};
    b.addReturn(*retOp, ret);
    Lir lir = std::move(b).finish();
    LirTextContext ctx;
    (void)roundTripOrFail(lir, *sch, ctx, "vreg %v.1");
}

TEST(LirTextRoundTrip, MultiBlockWithBrAndCondBr) {
    auto sch = shippedX86();
    auto const movOp  = sch->opcodeByMnemonic("mov");
    auto const jmpOp  = sch->opcodeByMnemonic("jmp");
    auto const retOp  = sch->opcodeByMnemonic("ret");
    LirBuilder b{*sch};
    b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    LirBlockId const tail  = b.createBlock();
    b.beginBlock(entry);
    LirReg const rax = makePhysicalReg(0, LirRegClass::GPR);
    std::array<LirOperand, 1> mov{LirOperand::makeImmInt32(5)};
    b.addInst(*movOp, rax, mov);
    b.addBr(*jmpOp, tail);
    b.beginBlock(tail);
    std::array<LirOperand, 1> ret{LirOperand::makeReg(rax)};
    b.addReturn(*retOp, ret);
    Lir lir = std::move(b).finish();
    LirTextContext ctx;
    (void)roundTripOrFail(lir, *sch, ctx, "two-block jmp+ret");
}

TEST(LirTextRoundTrip, AllOperandKindsIncludingNoneAndSymbolRef) {
    auto sch = shippedX86();
    auto const callOp = sch->opcodeByMnemonic("call");
    auto const movOp  = sch->opcodeByMnemonic("mov");
    auto const retOp  = sch->opcodeByMnemonic("ret");
    LirBuilder b{*sch};
    b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const rax = makePhysicalReg(0, LirRegClass::GPR);
    std::array<LirOperand, 2> movOps{LirOperand{}, LirOperand::makeImmInt32(0)};
    b.addInst(*movOp, rax, movOps);
    std::array<LirOperand, 1> callOps{LirOperand::makeSymbolRef(9)};
    b.addInst(*callOp, InvalidLirReg, callOps);
    b.addReturn(*retOp, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();
    LirTextContext ctx;
    std::string const text = roundTripOrFail(lir, *sch, ctx,
                                             "None + SymbolRef + ImmInt");
    EXPECT_NE(text.find("mov _, #0"), std::string::npos);
    EXPECT_NE(text.find("call @9"), std::string::npos);
}

TEST(LirTextRoundTrip, MemBaseAndMemOffsetBothSigns) {
    auto sch = shippedX86();
    auto const loadOp = sch->opcodeByMnemonic("load");
    auto const retOp  = sch->opcodeByMnemonic("ret");
    LirBuilder b{*sch};
    b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const result = makePhysicalReg(0, LirRegClass::GPR);
    LirReg const base   = makePhysicalReg(4, LirRegClass::GPR);
    std::array<LirOperand, 3> ops{
        LirOperand::makeReg(base),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(-24)
    };
    b.addInst(*loadOp, result, ops);
    b.addReturn(*retOp, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();
    LirTextContext ctx;
    (void)roundTripOrFail(lir, *sch, ctx, "load with neg MemOffset");
}

TEST(LirTextRoundTrip, LiteralPoolWithAllVariants) {
    auto sch = shippedX86();
    auto const movOp = sch->opcodeByMnemonic("mov");
    auto const retOp = sch->opcodeByMnemonic("ret");
    LirBuilder b{*sch};
    // Append every literal variant arm to exercise the parser's
    // `parseLiteralValue` switch end-to-end.
    LirLiteralValue p; p.value = std::monostate{};            p.core = TypeKind::Void;
    LirLiteralValue bL; bL.value = true;                       bL.core = TypeKind::Bool;
    LirLiteralValue iL; iL.value = std::int64_t{-9};           iL.core = TypeKind::I64;
    LirLiteralValue uL; uL.value = std::uint64_t{42};          uL.core = TypeKind::U64;
    LirLiteralValue fL; fL.value = 3.5;                        fL.core = TypeKind::F64;
    LirLiteralValue sL; sL.value = std::string{"hi\nthere"};   sL.core = TypeKind::Array;
    LirAggregateValue agg; agg.fields.push_back(iL); agg.fields.push_back(sL);
    LirLiteralValue aL; aL.value = std::move(agg);             aL.core = TypeKind::Struct;
    (void)b.literalPoolAdd(std::move(p));
    (void)b.literalPoolAdd(std::move(bL));
    (void)b.literalPoolAdd(std::move(iL));
    (void)b.literalPoolAdd(std::move(uL));
    (void)b.literalPoolAdd(std::move(fL));
    (void)b.literalPoolAdd(std::move(sL));
    (void)b.literalPoolAdd(std::move(aL));
    // Minimal function so the module isn't empty.
    b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const rax = makePhysicalReg(0, LirRegClass::GPR);
    std::array<LirOperand, 1> mov{LirOperand::makeImmInt32(0)};
    b.addInst(*movOp, rax, mov);
    b.addReturn(*retOp, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();
    LirTextContext ctx;
    (void)roundTripOrFail(lir, *sch, ctx, "all literal variants");
}

TEST(LirTextRoundTrip, NonZeroPayloadAndFlags) {
    auto sch = shippedX86();
    auto const frameLoadOp = sch->opcodeByMnemonic(sch->frameLoadMnemonic());
    auto const retOp       = sch->opcodeByMnemonic("ret");
    LirBuilder b{*sch};
    b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const rax = makePhysicalReg(0, LirRegClass::GPR);
    b.addInst(*frameLoadOp, rax, std::span<LirOperand const>{},
              /*payload=*/7, /*flags=*/3);
    b.addReturn(*retOp, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();
    LirTextContext ctx;
    (void)roundTripOrFail(lir, *sch, ctx, "payload=7 flags=3");
}

TEST(LirTextRoundTrip, MultiFunctionModule) {
    auto sch = shippedX86();
    auto const movOp = sch->opcodeByMnemonic("mov");
    auto const retOp = sch->opcodeByMnemonic("ret");
    LirBuilder b{*sch};
    for (std::uint32_t s = 1; s <= 3; ++s) {
        b.addFunction(SymbolId{s});
        LirBlockId const entry = b.createBlock();
        b.beginBlock(entry);
        LirReg const rax = makePhysicalReg(0, LirRegClass::GPR);
        std::array<LirOperand, 1> mov{
            LirOperand::makeImmInt32(static_cast<std::int32_t>(s))};
        b.addInst(*movOp, rax, mov);
        std::array<LirOperand, 1> ret{LirOperand::makeReg(rax)};
        b.addReturn(*retOp, ret);
    }
    Lir lir = std::move(b).finish();
    std::vector<std::string> const names{"", "alpha", "beta", "gamma"};
    LirTextContext ctx{};
    ctx.symbolNames = std::span<std::string const>{names};
    (void)roundTripOrFail(lir, *sch, ctx, "3 functions, 3 names");
}

TEST(LirTextParser, VersionMismatchEmitsTextVersionMismatch) {
    auto sch = shippedX86();
    std::string const malformed =
        "dsslir 1\n"
        "target x86_64 version \"999.0.0\"\n"
        "symbols {}\n"
        "literal_pool {}\n"
        "module {}\n";
    DiagnosticReporter rep;
    auto result = parseLir(malformed, *sch, rep);
    EXPECT_FALSE(result->ok);
    bool sawMismatch = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::I_TextVersionMismatch) sawMismatch = true;
    }
    EXPECT_TRUE(sawMismatch)
        << "non-empty version mismatch must emit I_TextVersionMismatch";
}

TEST(LirTextParser, TargetNameMismatchEmitsDiagnostic) {
    auto sch = shippedX86();
    std::string const malformed =
        "dsslir 1\n"
        "target arm64 version \"\"\n"
        "symbols {}\n"
        "literal_pool {}\n"
        "module {}\n";
    DiagnosticReporter rep;
    auto result = parseLir(malformed, *sch, rep);
    EXPECT_FALSE(result->ok);
    bool sawMismatch = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::I_TextVersionMismatch) sawMismatch = true;
    }
    EXPECT_TRUE(sawMismatch)
        << "target name mismatch must emit I_TextVersionMismatch (same code family)";
}

TEST(LirTextParser, UnknownOpcodeMnemonicEmitsTextUnknownName) {
    auto sch = shippedX86();
    // Build malformed text by hand. Use a real header so the parser
    // reaches the inst body.
    std::string const v{sch->version()};
    std::string const malformed = std::format(
        "dsslir 1\n"
        "target {} version \"{}\"\n"
        "symbols {{\n  %1\n}}\n"
        "literal_pool {{}}\n"
        "module {{\n"
        "  function %1 {{\n"
        "    block ^b0 [entry] -> [] {{\n"
        "      rax = bogusopcode #0 ; payload=0 flags=0\n"
        "    }}\n"
        "  }}\n"
        "}}\n",
        sch->name(), v);
    DiagnosticReporter rep;
    auto result = parseLir(malformed, *sch, rep);
    EXPECT_FALSE(result->ok);
    bool sawUnknown = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::I_TextUnknownName) sawUnknown = true;
    }
    EXPECT_TRUE(sawUnknown)
        << "unknown opcode must emit I_TextUnknownName (parser distinguishes "
           "ill-formed structure from unknown vocabulary)";
}

TEST(LirTextRoundTrip, CondBrTwoSuccessorsWithCondCodeEqPayload) {
    // Test-analyzer rating 10 — CondBr round-trip is the dispatch path
    // that combines (a) 2-successor block resolution, (b) `payload`
    // forwarding into `addCondBr`, (c) `flags` forwarding through the
    // new builder signature, and (d) the `payload=0` (= CondCode::Eq)
    // edge case that justified cycle-1's unconditional payload emit.
    auto sch = shippedX86();
    auto const movOp = sch->opcodeByMnemonic("mov");
    auto const cmpOp = sch->opcodeByMnemonic("cmp");
    auto const jccOp = sch->opcodeByMnemonic("jcc");
    auto const retOp = sch->opcodeByMnemonic("ret");
    ASSERT_TRUE(movOp.has_value() && cmpOp.has_value()
             && jccOp.has_value() && retOp.has_value());
    LirBuilder b{*sch};
    b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    LirBlockId const thenB = b.createBlock();
    LirBlockId const elseB = b.createBlock();
    b.beginBlock(entry);
    LirReg const rax = makePhysicalReg(0, LirRegClass::GPR);
    std::array<LirOperand, 1> mov{LirOperand::makeImmInt32(1)};
    b.addInst(*movOp, rax, mov);
    std::array<LirOperand, 2> cmpOps{
        LirOperand::makeReg(rax), LirOperand::makeImmInt32(0)};
    b.addInst(*cmpOp, InvalidLirReg, cmpOps);
    // Eq = 0 — the payload-zero round-trip case.
    b.addCondBr(*jccOp, std::span<LirOperand const>{}, thenB, elseB,
                static_cast<std::uint32_t>(TargetCondCode::Eq));
    b.beginBlock(thenB);
    b.addReturn(*retOp, std::span<LirOperand const>{});
    b.beginBlock(elseB);
    b.addReturn(*retOp, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();
    LirTextContext ctx;
    std::string const text = roundTripOrFail(
        lir, *sch, ctx, "CondBr with CondCode::Eq (payload=0)");
    // CondBr embeds successors in the BLOCK header (`-> [^b1, ^b2]`),
    // NOT in the inst's operand list. Pin the block-header form.
    EXPECT_NE(text.find("block ^b0 [entry] -> [^b1, ^b2]"),
              std::string::npos)
        << "CondBr's 2 successors live on the block header";
    EXPECT_NE(text.find("jcc ; payload=0 flags=0"), std::string::npos)
        << "CondBr inst has zero operand BlockRefs and emits `payload=0` "
           "(CondCode::Eq) lossless";
}

TEST(LirTextRoundTrip, CondBrWithNonZeroPayloadAndFlags) {
    // Companion to the Eq-payload test: pins payload=Ne(=1) + flags=4
    // through the builder's new flags param.
    auto sch = shippedX86();
    auto const cmpOp = sch->opcodeByMnemonic("cmp");
    auto const jccOp = sch->opcodeByMnemonic("jcc");
    auto const retOp = sch->opcodeByMnemonic("ret");
    LirBuilder b{*sch};
    b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    LirBlockId const a = b.createBlock();
    LirBlockId const c = b.createBlock();
    b.beginBlock(entry);
    LirReg const rax = makePhysicalReg(0, LirRegClass::GPR);
    std::array<LirOperand, 2> cmpOps{
        LirOperand::makeReg(rax), LirOperand::makeImmInt32(0)};
    b.addInst(*cmpOp, InvalidLirReg, cmpOps);
    b.addCondBr(*jccOp, std::span<LirOperand const>{}, a, c,
                static_cast<std::uint32_t>(TargetCondCode::Ne),
                /*flags=*/4);
    b.beginBlock(a);
    b.addReturn(*retOp, std::span<LirOperand const>{});
    b.beginBlock(c);
    b.addReturn(*retOp, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();
    LirTextContext ctx;
    std::string const text = roundTripOrFail(
        lir, *sch, ctx, "CondBr Ne + flags=4");
    EXPECT_NE(text.find("payload=1 flags=4"), std::string::npos)
        << "CondBr payload=Ne(1) + flags=4 must thread through builder";
}

TEST(LirTextRoundTrip, VRegIdGapMintingFillsMissingSlots) {
    // Test-analyzer rating 9 — exercise the `lookupOrMintVreg` mint-
    // fill loop. We can't easily construct a sparse vreg sequence via
    // the in-process builder (which mints monotonically) — so use the
    // PARSER path: forge text referencing a high vreg id and parse it
    // through the cap-checked path. After parse, the builder has
    // minted filler vregs all the way up.
    auto sch = shippedX86();
    std::string const v{sch->version()};
    std::string const text = std::format(
        "dsslir 1\n"
        "target {} version \"{}\"\n"
        "symbols {{\n  %1 \"main\"\n}}\n"
        "literal_pool {{}}\n"
        "module {{\n"
        "  function %1 \"main\" {{\n"
        "    block ^b0 [entry] -> [] {{\n"
        "      %v.5:gpr = mov #1 ; payload=0 flags=0\n"
        "      ret %v.5:gpr ; payload=0 flags=0\n"
        "    }}\n"
        "  }}\n"
        "}}\n",
        sch->name(), v);
    DiagnosticReporter rep;
    auto result = parseLir(text, *sch, rep);
    ASSERT_TRUE(result->ok) << "vreg-gap text should parse cleanly";
    // Builder mints 1..5; first 4 are filler, vreg 5 is the real one.
    // Re-emit should normalize to the same `%v.5:gpr` for the live
    // vreg AND not surface the filler (they have no defs/uses).
    DiagnosticReporter rep2;
    LirTextContext ctx2{};
    ctx2.symbolNames = std::span<std::string const>{result->symbolNames};
    std::string const text2 = emitLir(result->lir, *sch, ctx2, rep2);
    EXPECT_NE(text2.find("%v.5:gpr"), std::string::npos);
}

TEST(LirTextParser, VRegIdAboveCapEmitsMalformed) {
    // Companion: vreg id past `kMaxVRegIdPerFunction` must diagnose
    // loudly rather than mint 65535+ wasted vregs.
    auto sch = shippedX86();
    std::string const v{sch->version()};
    std::string const text = std::format(
        "dsslir 1\n"
        "target {} version \"{}\"\n"
        "symbols {{\n  %1 \"main\"\n}}\n"
        "literal_pool {{}}\n"
        "module {{\n"
        "  function %1 \"main\" {{\n"
        "    block ^b0 [entry] -> [] {{\n"
        "      %v.999999:gpr = mov #1 ; payload=0 flags=0\n"
        "      ret ; payload=0 flags=0\n"
        "    }}\n"
        "  }}\n"
        "}}\n",
        sch->name(), v);
    DiagnosticReporter rep;
    auto result = parseLir(text, *sch, rep);
    EXPECT_FALSE(result->ok)
        << "vreg id past kMaxVRegIdPerFunction must reject loudly";
    bool sawCap = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::I_TextMalformed
            && d.actual.find("out of per-function range") != std::string::npos) {
            sawCap = true;
        }
    }
    EXPECT_TRUE(sawCap)
        << "expected per-function vreg cap diagnostic";
}

TEST(LirTextParser, PanicModeProducesOneDiagnosticPerBadInst) {
    // Test-analyzer rating 8 — two consecutive bad insts must produce
    // exactly 2 `I_TextUnknownName` (panic-mode skips to next inst),
    // not 1 swallowed + cascade and not hundreds.
    auto sch = shippedX86();
    std::string const v{sch->version()};
    std::string const text = std::format(
        "dsslir 1\n"
        "target {} version \"{}\"\n"
        "symbols {{\n  %1 \"main\"\n}}\n"
        "literal_pool {{}}\n"
        "module {{\n"
        "  function %1 \"main\" {{\n"
        "    block ^b0 [entry] -> [] {{\n"
        "      rax = bogus1 #0 ; payload=0 flags=0\n"
        "      rax = bogus2 #0 ; payload=0 flags=0\n"
        "      ret rax ; payload=0 flags=0\n"
        "    }}\n"
        "  }}\n"
        "}}\n",
        sch->name(), v);
    DiagnosticReporter rep;
    auto result = parseLir(text, *sch, rep);
    EXPECT_FALSE(result->ok);
    std::size_t unknownCount = 0;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::I_TextUnknownName
            && d.actual.find("unknown opcode") != std::string::npos) {
            ++unknownCount;
        }
    }
    EXPECT_EQ(unknownCount, 2u)
        << "panic-mode must yield exactly 2 unknown-opcode diagnostics, "
           "not 1 (swallowed) and not many (cascade)";
}

TEST(LirTextRoundTrip, NegativeImmInt) {
    // Test-analyzer rating 7 — emit renders `op.immInt32` directly,
    // producing `#-5` (Hash + Minus + Integer). The parser's Hash arm
    // must stitch the sign back. Round-trip pins this asymmetry.
    auto sch = shippedX86();
    auto const movOp = sch->opcodeByMnemonic("mov");
    auto const retOp = sch->opcodeByMnemonic("ret");
    LirBuilder b{*sch};
    b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const rax = makePhysicalReg(0, LirRegClass::GPR);
    std::array<LirOperand, 1> mov{LirOperand::makeImmInt32(-5)};
    b.addInst(*movOp, rax, mov);
    b.addReturn(*retOp, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();
    LirTextContext ctx;
    std::string const text = roundTripOrFail(lir, *sch, ctx, "ImmInt -5");
    EXPECT_NE(text.find("mov #-5"), std::string::npos);
}

TEST(LirTextRoundTrip, FprVRegRoundTripsWithClassTag) {
    // 3-agent CRITICAL fold pin — FPR vreg must round-trip without
    // silently demoting to GPR. The `%v.N:fpr` form carries the class
    // through the parser.
    auto sch = shippedX86();
    auto const movOp = sch->opcodeByMnemonic("mov");
    auto const retOp = sch->opcodeByMnemonic("ret");
    LirBuilder b{*sch};
    b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const vf = b.newVReg(LirRegClass::FPR);
    std::array<LirOperand, 1> mov{LirOperand::makeImmInt32(0)};
    b.addInst(*movOp, vf, mov);
    b.addReturn(*retOp, std::span<LirOperand const>{});
    Lir lir = std::move(b).finish();
    LirTextContext ctx;
    std::string const text = roundTripOrFail(lir, *sch, ctx, "FPR vreg");
    EXPECT_NE(text.find("%v.1:fpr"), std::string::npos)
        << "FPR vreg must render with the `:fpr` class tag";
}

TEST(LirTextParser, VerifyOnLoadCatchesMemOperandPairingViolation) {
    auto sch = shippedX86();
    // load expects last two ops to be [MemBase, MemOffset]. Forge a
    // malformed text with [Reg, ImmInt] instead — round-trip will then
    // hit `checkMemOperandPairing` (Rule 1) at parse time.
    std::string const v{sch->version()};
    std::string const malformed = std::format(
        "dsslir 1\n"
        "target {} version \"{}\"\n"
        "symbols {{\n  %1\n}}\n"
        "literal_pool {{}}\n"
        "module {{\n"
        "  function %1 {{\n"
        "    block ^b0 [entry] -> [] {{\n"
        "      rax = load rsp, #0, #0 ; payload=0 flags=0\n"
        "      ret rax ; payload=0 flags=0\n"
        "    }}\n"
        "  }}\n"
        "}}\n",
        sch->name(), v);
    DiagnosticReporter rep;
    auto result = parseLir(malformed, *sch, rep);
    EXPECT_FALSE(result->ok);
    bool sawMem = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::L_MemOperandMalformed) sawMem = true;
    }
    EXPECT_TRUE(sawMem)
        << "verify-on-load (LIR-only Rule 1) must catch the malformed "
           "MemBase/MemOffset pairing with the dedicated diagnostic code "
           "L_MemOperandMalformed (was incorrectly reusing "
           "L_UnsupportedLoweringForOpcode before the architect MED fold)";
}

