// D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3) — `synthesizeStdioShim`'s va_list ARM
// SELECTION, pinned at the OPERAND the synthesized call actually forwards.
//
// WHY THIS FILE EXISTS SEPARATELY FROM THE SynthStdioShim SUITE IN
// `test_mir_merge.cpp`. That suite pins the shim's SHAPE (a defined variadic
// function, one call through the module's already-imported UCRT core, no minted
// import). This one pins a different thing entirely: WHICH va_list leaf the body
// anchors `ap` on, which is a per-TARGET correctness fork with no shape
// consequence at all — both arms produce a structurally identical function.
// tests/mir's stated convention is one executable per file for ctest parallelism
// and per-file failure isolation, and a decision site this sharp earns its own.
//
// ★ THE MISCOMPILE THIS GUARDS. `synthesizeStdioShim` used to gate on
// `strategy == HomogeneousPointer` and then emit `VaHomeArgAreaAddr`
// unconditionally. HomogeneousPointer does NOT imply Win64: `apple_arm64`
// declares the SAME strategy with `variadicUsesOverflowBase: true`, and there
// the correct leaf is `VaOverflowArgAreaAddr` with NO payload. Apple arm64 has
// no home area — every vararg is stacked — so the home leaf would anchor `ap` at
// NAMED-ARGUMENT storage and the shim would forward the wrong bytes. That
// compiles, links, loads and runs; it just prints garbage. The fix threads the
// whole `VaListLayout` down so the shim branches on the same field
// `src/mir/lowering/hir_to_mir.cpp` reads — one source of truth, two readers.
//
// ★ AND WHY A LOWERING TEST IS NOT THIS TEST. Every pre-existing
// `variadicUsesOverflowBase` assertion under tests/ pins the LOWERING path
// (`hir_to_mir`, the user-written `va_start`). `synth_stdio_shim` is an
// INDEPENDENT second decision site reading the same config field. A test that
// covers only the first stays green through a total regression of the second —
// the "still passes when the implementation is silently broken" shape the bar
// rejects. Measured before this file was written: no test anywhere exercised the
// shim's overflow-base arm.
//
// ★ AND WHY THE ASSERTIONS ARE OPERAND-LEVEL, NOT OPCODE COUNTS. "the body
// contains one VaHomeArgAreaAddr" is inert the moment anything else in the
// function emits that opcode, and it says nothing about whether the CALL uses
// it. These tests locate the synthesized call BY ITS CALLEE SYMBOL and assert
// the identity of the instruction sitting in its `ap` argument slot — `ap` is
// the final parameter of every `__stdio_common_v*` core. A shim that emitted the
// right leaf and then forwarded a different value would pass a count and fail
// here.
//
// RED-ON-DISABLE, demonstrated by actually breaking it (TF-C111): replacing the
// `vaLayout->variadicUsesOverflowBase ? ... : ...` fork in
// src/mir/merge/synth_stdio_shim.cpp with the unconditional `VaHomeArgAreaAddr`
// it used to be reds `OverflowBaseArmAnchorsApOnTheOverflowBase` on the `ap`
// operand's opcode, with the home arm and every other test still green.

#include "core/types/aggregate_layout.hpp"      // VaListStrategy
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"         // VaListLayout
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_lattice.hpp"
#include "mir/merge/synth_stdio_shim.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"
#include "mir/mir_verifier.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace dss;

namespace {

// Pre-minted symbol ids. The shim symbols are the ones stdio.json tags with
// `synthesize`; the core ids are ordinary descriptor imports.
constexpr std::uint32_t kSprintfSym = 10;
constexpr std::uint32_t kPrintfSym  = 11;
constexpr std::uint32_t kMainSym    = 100;

constexpr std::uint32_t kCoreVsprintfSym = 20;
constexpr std::uint32_t kCoreVfprintfSym = 21;
constexpr std::uint32_t kAcrtIobFuncSym  = 22;

MirLiteralValue i32Lit(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

// The caller-side scaffold: one `main` that references each shim symbol through a
// GlobalAddr against a NOT-yet-defined callee — the shape the CST->HIR seam leaves
// behind for a `synthesize`-tagged descriptor row.
Mir buildCaller(TypeInterner& in, std::vector<std::uint32_t> const& shimSyms) {
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const pCh = in.pointer(in.primitive(TypeKind::Char));
    std::array<TypeId, 2> const sp{pCh, pCh};
    TypeId const variadicSig = in.fnSig(sp, i32, CallConv::CcMS64, /*isVariadic=*/true);

    MirBuilder mb;
    mb.addFunction(in.fnSig({}, i32, CallConv::CcMS64), SymbolId{kMainSym});
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    MirInstId const buf = mb.addInst(MirOpcode::Alloca, {}, pCh, 64);
    for (std::uint32_t sym : shimSyms) {
        MirInstId const ga = mb.addGlobalAddr(SymbolId{sym}, in.pointer(variadicSig));
        std::array<MirInstId, 3> const co{ga, buf, buf};
        mb.addInst(MirOpcode::Call, co, i32);
    }
    mb.addReturn(mb.addConst(i32Lit(0), i32));
    return std::move(mb).finish();
}

// The UCRT cores as ORDINARY descriptor imports — what stdio.json's pe rows
// produce. `printf` needs the iob accessor as well (`printf` IS `fprintf` to
// `__acrt_iob_func(1)`), so both recipes' prerequisites are present in one list.
std::vector<ExternImport> stdioCoreImports() {
    auto make = [](std::uint32_t sym, char const* name) {
        ExternImport e;
        e.symbol      = SymbolId{sym};
        e.mangledName = name;
        e.libraryPath = "ucrtbase.dll";
        e.isData      = false;
        return e;
    };
    return {make(kCoreVsprintfSym, "__stdio_common_vsprintf"),
            make(kCoreVfprintfSym, "__stdio_common_vfprintf"),
            make(kAcrtIobFuncSym, "__acrt_iob_func")};
}

std::optional<MirFuncId> findFuncBySymbol(Mir const& mir, std::uint32_t symV) {
    for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i) {
        MirFuncId const f = mir.funcAt(i);
        if (mir.funcSymbol(f).v == symV) return f;
    }
    return std::nullopt;
}

// ★ THE OPERAND-LEVEL PROBE, and the whole point of this file. Locate the Call
// whose CALLEE is a GlobalAddr to `coreSymV` and hand back its LAST operand.
//
// `ap` is the final parameter of every `__stdio_common_v*` core
// (`(_Options, ..., _Locale, _ArgList)`), so the last operand IS the forwarded
// va_list — asserted rather than assumed by the arity check each caller makes.
// Selecting the call by CALLEE matters: `printf`'s body contains TWO calls (the
// `__acrt_iob_func(1)` stream fetch and the core), and "the first call in the
// block" would silently probe the wrong one.
std::optional<MirInstId> apOperandOfCoreCall(Mir const& mir, MirFuncId fn,
                                             std::uint32_t coreSymV,
                                             std::size_t* operandCountOut = nullptr) {
    for (std::uint32_t bi = 0; bi < mir.funcBlockCount(fn); ++bi) {
        MirBlockId const b = mir.funcBlockAt(fn, bi);
        for (std::uint32_t i = 0; i < mir.blockInstCount(b); ++i) {
            MirInstId const id = mir.blockInstAt(b, i);
            if (mir.instOpcode(id) != MirOpcode::Call) continue;
            auto const ops = mir.instOperands(id);
            if (ops.empty()) continue;
            if (mir.instOpcode(ops[0]) != MirOpcode::GlobalAddr) continue;
            if (mir.globalAddrSymbol(ops[0]).v != coreSymV) continue;
            if (operandCountOut != nullptr) *operandCountOut = ops.size();
            return ops.back();
        }
    }
    return std::nullopt;
}

std::uint32_t countOpcode(Mir const& mir, MirFuncId fn, MirOpcode op) {
    std::uint32_t n = 0;
    for (std::uint32_t bi = 0; bi < mir.funcBlockCount(fn); ++bi) {
        MirBlockId const b = mir.funcBlockAt(fn, bi);
        for (std::uint32_t i = 0; i < mir.blockInstCount(b); ++i)
            if (mir.instOpcode(mir.blockInstAt(b, i)) == op) ++n;
    }
    return n;
}

// The two HomogeneousPointer targets, differing ONLY in the field under test.
// `namedArgSlotBytes` is filled in so the layout is a plausible whole rather than
// a one-field stub — the pass must key on `variadicUsesOverflowBase` and nothing
// else about the shape.
VaListLayout homogeneousPointer(bool overflowBase) {
    VaListLayout l;
    l.strategy                 = VaListStrategy::HomogeneousPointer;
    l.namedArgSlotBytes        = 8;
    l.variadicUsesOverflowBase = overflowBase;
    return l;
}

}  // namespace

// ── ARM 1: Win64 (`variadicUsesOverflowBase = false`) ────────────────────────
//
// The named args are SPILLED to a home block contiguous with the overflow area,
// so the first vararg is just past them: `&home[namedArgCount]`. The leaf must be
// `VaHomeArgAreaAddr`, its payload must be the recipe's real named-arg count, and
// the overflow leaf must be ABSENT — a body carrying both would mean the fork
// emitted a dead instruction and the arm choice was decided somewhere else.
TEST(SynthStdioShimVaListArm, HomeBaseArmAnchorsApOnTheHomeArea) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildCaller(in, {kSprintfSym});

    std::unordered_map<std::uint32_t, std::string> const recipes{{kSprintfSym, "sprintf"}};
    std::vector<ExternImport> const externs = stdioCoreImports();
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeStdioShim(mir, in, recipes, homogeneousPointer(false),
                                    externs, rep));
    ASSERT_FALSE(rep.hasErrors());

    auto const shim = findFuncBySymbol(mir, kSprintfSym);
    ASSERT_TRUE(shim.has_value());

    std::size_t operandCount = 0;
    auto const ap = apOperandOfCoreCall(mir, *shim, kCoreVsprintfSym, &operandCount);
    ASSERT_TRUE(ap.has_value())
        << "no Call to __stdio_common_vsprintf found — the probe is looking at the "
           "wrong callee, which would make every assertion below vacuous";
    EXPECT_EQ(operandCount, std::size_t{7})
        << "callee + (_Options, buf, _BufferCount, fmt, _Locale, _ArgList); `ap` is "
           "the LAST argument, and this arity is what makes ops.back() mean `ap`";

    EXPECT_EQ(mir.instOpcode(*ap), MirOpcode::VaHomeArgAreaAddr)
        << "on a HomogeneousPointer target with variadicUsesOverflowBase=false "
           "(Win64) the forwarded va_list MUST be the HOME-area leaf. Anchoring it "
           "anywhere else points `ap` at the wrong stack region and the shim "
           "forwards garbage — with no diagnostic, at run time only.";
    EXPECT_EQ(mir.instPayload(*ap), 2u)
        << "the home leaf is &home[namedArgCount] and sprintf's FIXED arity is "
           "(buf, fmt) == 2; a wrong count silently skips or re-reads a slot";

    EXPECT_EQ(countOpcode(mir, *shim, MirOpcode::VaOverflowArgAreaAddr), 0u)
        << "the overflow leaf must not appear on this arm at all — if both leaves "
           "are emitted, the fork is not deciding anything";
    EXPECT_EQ(countOpcode(mir, *shim, MirOpcode::VaHomeArgAreaAddr), 1u);

    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep));
}

// ── ARM 2: Apple arm64 (`variadicUsesOverflowBase = true`) ───────────────────
//
// THE ARM THAT HAD NO COVERAGE. Apple arm64 declares HomogeneousPointer too, but
// has NO home area: `variadicArgsAlwaysStack` forces every vararg onto the stack,
// so the first vararg IS the overflow base and the named args stay in registers.
// The leaf must be `VaOverflowArgAreaAddr`, and it must be PAYLOAD-FREE — there is
// no slot count to skip, and a payload here would be a home-arm reflex left
// behind.
TEST(SynthStdioShimVaListArm, OverflowBaseArmAnchorsApOnTheOverflowBase) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildCaller(in, {kSprintfSym});

    std::unordered_map<std::uint32_t, std::string> const recipes{{kSprintfSym, "sprintf"}};
    std::vector<ExternImport> const externs = stdioCoreImports();
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeStdioShim(mir, in, recipes, homogeneousPointer(true),
                                    externs, rep));
    ASSERT_FALSE(rep.hasErrors());

    auto const shim = findFuncBySymbol(mir, kSprintfSym);
    ASSERT_TRUE(shim.has_value());

    std::size_t operandCount = 0;
    auto const ap = apOperandOfCoreCall(mir, *shim, kCoreVsprintfSym, &operandCount);
    ASSERT_TRUE(ap.has_value());
    EXPECT_EQ(operandCount, std::size_t{7});

    EXPECT_EQ(mir.instOpcode(*ap), MirOpcode::VaOverflowArgAreaAddr)
        << "★ THE REGRESSION THIS TEST EXISTS FOR. variadicUsesOverflowBase=true "
           "(apple_arm64) is STILL HomogeneousPointer, so a strategy-only gate lets "
           "it through and then emits the Win64 home leaf. Apple arm64 has no home "
           "area, so that anchors `ap` at NAMED-ARGUMENT storage: the shim forwards "
           "the wrong bytes, and it compiles, links, loads and runs. The only "
           "symptom is wrong output.";
    EXPECT_EQ(mir.instPayload(*ap), 0u)
        << "the overflow leaf is payload-FREE: there is no home block to skip past, "
           "so a named-arg count here is a leftover from the other arm";

    EXPECT_EQ(countOpcode(mir, *shim, MirOpcode::VaHomeArgAreaAddr), 0u)
        << "the home leaf must not appear on an overflow-base target at all";
    EXPECT_EQ(countOpcode(mir, *shim, MirOpcode::VaOverflowArgAreaAddr), 1u);

    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep));
}

// The home leaf's payload must TRACK EACH RECIPE's named-arg count rather than be
// a constant that happens to match `sprintf`. `printf` takes one fixed parameter
// (fmt) and `sprintf` two (buf, fmt); both are synthesized in ONE pass here, so a
// hardcoded 2 reds on `printf` and a hardcoded 1 reds on `sprintf`.
//
// `printf`'s body also carries a SECOND call — the `__acrt_iob_func(1)` stream
// fetch — which is exactly why the probe selects its call by callee symbol.
TEST(SynthStdioShimVaListArm, HomePayloadTracksEachRecipesNamedArgCount) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildCaller(in, {kSprintfSym, kPrintfSym});

    std::unordered_map<std::uint32_t, std::string> const recipes{
        {kSprintfSym, "sprintf"}, {kPrintfSym, "printf"}};
    std::vector<ExternImport> const externs = stdioCoreImports();
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeStdioShim(mir, in, recipes, homogeneousPointer(false),
                                    externs, rep));
    ASSERT_FALSE(rep.hasErrors());

    auto const sprintfShim = findFuncBySymbol(mir, kSprintfSym);
    auto const printfShim  = findFuncBySymbol(mir, kPrintfSym);
    ASSERT_TRUE(sprintfShim.has_value());
    ASSERT_TRUE(printfShim.has_value());

    auto const sprintfAp = apOperandOfCoreCall(mir, *sprintfShim, kCoreVsprintfSym);
    ASSERT_TRUE(sprintfAp.has_value());
    EXPECT_EQ(mir.instOpcode(*sprintfAp), MirOpcode::VaHomeArgAreaAddr);
    EXPECT_EQ(mir.instPayload(*sprintfAp), 2u) << "sprintf(buf, fmt, ...) — 2 named";

    auto const printfAp = apOperandOfCoreCall(mir, *printfShim, kCoreVfprintfSym);
    ASSERT_TRUE(printfAp.has_value())
        << "printf must forward through __stdio_common_vfprintf, not the sprintf core";
    EXPECT_EQ(mir.instOpcode(*printfAp), MirOpcode::VaHomeArgAreaAddr);
    EXPECT_EQ(mir.instPayload(*printfAp), 1u) << "printf(fmt, ...) — 1 named";

    // The probe's discrimination is itself load-bearing: printf's body really does
    // hold a second call, so "the first Call in the block" would have been wrong.
    EXPECT_TRUE(apOperandOfCoreCall(mir, *printfShim, kAcrtIobFuncSym).has_value())
        << "printf's body must also call __acrt_iob_func for the stdout stream — if "
           "it does not, the callee-selecting probe above is not discriminating "
           "anything and this file's operand assertions are weaker than they look";

    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep));
}

// ── THE THIRD STRATEGY ───────────────────────────────────────────────────────
//
// `Aapcs64DualCursor` (linux arm64) is refused EXPLICITLY, and that word is the
// whole point of this block — it was not always true.
//
// ★ THE HISTORY IS THE LESSON, so it is recorded rather than tidied away. When
// these tests were first written the refusal was INCIDENTAL: the pass tested
// `strategy != HomogeneousPointer`, an exclusion, and this comment said so. The
// fragility was concrete, not stylistic — the natural edit when a SysV arm
// eventually lands is `if (strategy == SysVRegisterSave) { … } else { … }`, and
// under an exclusion test `Aapcs64DualCursor` then rides silently into the
// homogeneous arm: the shim hands the callee a bare pointer as `ap` where AAPCS64
// expects the ADDRESS OF a five-field `__va_list`, so the callee reads
// `__gr_top`/`__gr_offs` out of arbitrary stack. Wrong output or crash, no
// diagnostic at any stage.
//
// An independent audit caught that the registry row had already been written as
// though the fix had landed when it had not, and the pass now enumerates: a
// `switch` over the closed `VaListStrategy` enum, with `SysVRegisterSave` and
// `Aapcs64DualCursor` as explicit `return false` cases that name the offending
// strategy via `vaListStrategyName()`. That converts the dangerous future edit
// from a silent miscompile into a COMPILE error — the same protection
// `hir_to_mir`'s exhaustive dispatch always had, and precisely why the real
// lowering could never acquire this bug while this pass could.
// ⚠ Do NOT "simplify" that switch back to an inequality: the assertions below
// would stay green through the change, because they pin the refusal that exists
// today, not the shape of the code that produces it.
//
// So the refusal is pinned as BEHAVIOUR: a reported error, a `false` return, and
// — the part a bare `EXPECT_FALSE` would miss — NO half-built definition left in
// the module. A pass that emitted the function and then failed would leave a
// wrong-ABI body for the next stage to consume.
TEST(SynthStdioShimVaListArm, Aapcs64DualCursorIsRefusedLoudlyAndEmitsNothing) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildCaller(in, {kSprintfSym});
    std::uint32_t const before = mir.moduleFuncCount();

    VaListLayout dualCursor;
    dualCursor.strategy          = VaListStrategy::Aapcs64DualCursor;
    dualCursor.namedArgSlotBytes = 8;

    std::unordered_map<std::uint32_t, std::string> const recipes{{kSprintfSym, "sprintf"}};
    std::vector<ExternImport> const externs = stdioCoreImports();
    DiagnosticReporter rep;
    EXPECT_FALSE(synthesizeStdioShim(mir, in, recipes, dualCursor, externs, rep))
        << "an unimplemented va_list strategy MUST be refused, never forwarded "
           "under a model the shim does not implement";
    EXPECT_TRUE(rep.hasErrors())
        << "the refusal must be REPORTED — a bare `false` return with no diagnostic "
           "is a silent failure";
    EXPECT_EQ(mir.moduleFuncCount(), before)
        << "a refused synthesis must leave NO half-built definition behind";
    EXPECT_FALSE(findFuncBySymbol(mir, kSprintfSym).has_value())
        << "sprintf must remain undefined when the strategy is refused";
}

// A CC that declares NO `vaListLayout` at all is the other way the fork can lose
// its input, and it must also be refused rather than defaulted: a defaulted layout
// would make "the target declared nothing" indistinguishable from "the target
// declared SysVRegisterSave", which is an ASSUMED ABI rather than a read one.
TEST(SynthStdioShimVaListArm, AbsentVaListLayoutIsRefusedLoudly) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildCaller(in, {kSprintfSym});
    std::uint32_t const before = mir.moduleFuncCount();

    std::unordered_map<std::uint32_t, std::string> const recipes{{kSprintfSym, "sprintf"}};
    std::vector<ExternImport> const externs = stdioCoreImports();
    DiagnosticReporter rep;
    EXPECT_FALSE(synthesizeStdioShim(mir, in, recipes, std::nullopt, externs, rep));
    EXPECT_TRUE(rep.hasErrors());
    EXPECT_EQ(mir.moduleFuncCount(), before);
}
