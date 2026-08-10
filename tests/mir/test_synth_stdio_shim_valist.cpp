// D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3) — `synthesizeStdioShim`'s va_list ARM
// SELECTION, pinned at the OPERAND the synthesized call actually forwards.
//
// WHY THIS FILE EXISTS SEPARATELY FROM THE SynthStdioShim SUITE IN
// `test_mir_merge.cpp`. That suite pins the shim's OPERAND CONTRACT — for each of
// the five recipes, the callee symbol, the operand count, and every argument by
// position. This one pins a different thing entirely: WHICH va_list leaf the body
// anchors `ap` on, which is a per-TARGET correctness fork with no shape
// consequence at all — both arms produce a structurally identical function.
// tests/mir's stated convention is one executable per file for ctest parallelism
// and per-file failure isolation, and a decision site this sharp earns its own.
//
// ★ AND THE FAMILY'S NEGATIVE SPACE LIVES HERE TOO (TF-C112). Every recipe's
// va-leaf question is WHICH leaf — except `vfprintf`, whose question is WHETHER,
// and the answer is none: C 7.21.6.8 gives it a DECLARED `va_list ap` parameter,
// so the shim forwards `Arg 2` verbatim under a NON-variadic signature. A
// regression to `vaStart(2)`/`vsig` would both re-derive `ap` from a frame with no
// varargs in it AND hand `lir_callconv` a prologue-spill signal for a function
// that receives none. Same decision site, opposite answer — so it is pinned beside
// its siblings rather than somewhere the next reader of this fork will not look.
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
//
// ★ RED-ON-DISABLE FOR THE TF-C112 ADDITIONS, each mutation applied to
// src/mir/merge/synth_stdio_shim.cpp ONE AT A TIME, both mir stdio binaries re-run,
// then restored (src verified byte-identical by `git hash-object` afterwards):
//   * `kIobStdout = 1 -> 2`  -> HomePayloadTracksEachRecipesNamedArgCount, on the iob
//     index ("Which is: (2)" vs "(1)").
//   * printf keeps calling `__acrt_iob_func(1)` but forwards `nullP()` as `_Stream`
//     -> the SAME test, on the CHAIN assertion (`printfOps[2].v == iobCall->v`, 28 vs
//     25). This is the one that matters: the old bare `has_value()` on the accessor
//     call stayed green through it, because the accessor really was still there.
//   * `vfprintf` sig -> `vsig`  -> VfprintfEmitsNoVaLeafAndKeepsANonVariadicSignature,
//     on `fnIsVariadic`.
//   * `vfprintf` `Arg 2` -> `vaStart(2)`  -> the same test, on the `ap` opcode AND on
//     both zero-leaf assertions.
//   * `sscanf` re-pointed at the `__stdio_common_vsprintf` core  -> the payload test's
//     sscanf row, which MISSES its core (`ap.has_value()` false) rather than matching
//     the wrong one — the two cores share one TypeId, so nothing else can see it.
//   * `fprintf`'s `vaStart(2)` -> `vaStart(1)`  -> the payload test's fprintf row (1
//     vs 2), which is exactly what the single-recipe form could not have caught.
// And the negative control: the sprintf `buf`/`fmt` transposition and the `_Options`,
// `_BufferCount` and `_Locale` mutations leave THIS file green, as they should — they
// are operand-contract faults, and test_mir_merge.cpp reds on each of them.

#include "core/types/aggregate_layout.hpp"      // VaListStrategy
#include "core/types/arg_payload.hpp"           // Arg payload is PACKED (position<<16)|ordinal
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
#include <tuple>
#include <unordered_map>
#include <variant>
#include <vector>

using namespace dss;

namespace {

// Pre-minted symbol ids. The shim symbols are the ones stdio.json tags with
// `synthesize`; the core ids are ordinary descriptor imports.
constexpr std::uint32_t kSprintfSym  = 10;
constexpr std::uint32_t kPrintfSym   = 11;
constexpr std::uint32_t kFprintfSym  = 12;
constexpr std::uint32_t kVfprintfSym = 13;
constexpr std::uint32_t kSscanfSym   = 14;
constexpr std::uint32_t kSnprintfSym = 15;
constexpr std::uint32_t kMainSym     = 100;

constexpr std::uint32_t kCoreVsprintfSym = 20;
constexpr std::uint32_t kCoreVfprintfSym = 21;
constexpr std::uint32_t kAcrtIobFuncSym  = 22;
constexpr std::uint32_t kCoreVsscanfSym  = 23;

// `__acrt_iob_func(0/1/2)` == stdin/stdout/stderr (corecrt_wstdio.h). Restated from
// the UCRT contract rather than read out of the pass, whose constant is a file-local
// `constexpr` in an anonymous namespace — importing it would let any future change to
// it approve itself.
constexpr std::int64_t kIobStdoutIndex = 1;

MirLiteralValue i32Lit(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

// Each shim's C declaration, so the scaffold below references it at its REAL arity and
// variadicity. `vfprintf` is the odd one — three DECLARED parameters and NOT variadic
// (C 7.21.6.8) — and spelling that here keeps `main`'s reference a plausible call
// rather than a stub that only happens to verify.
//
// `sizeParamIndex` exists for `snprintf` alone: it is the only recipe with a parameter
// that is NOT a pointer (`size_t n` at index 1). The scaffold has to spell that
// honestly rather than pass a pointer in the slot, because the pass DEFINES the shim
// with a `u64` there and the MirVerifier every test below runs compares the definition
// against this reference — a `ptr` here would red on a scaffold defect and read as a
// pass defect.
constexpr std::uint32_t kNoSizeParam = 0xFFFFFFFFu;
struct ShimDecl { std::uint32_t fixedArgc; bool variadic; std::uint32_t sizeParamIndex; };
ShimDecl declOf(std::uint32_t sym) {
    switch (sym) {
    case kPrintfSym:   return {1, true,  kNoSizeParam};  // printf(fmt, ...)
    case kVfprintfSym: return {3, false, kNoSizeParam};  // vfprintf(stream, fmt, ap)
    case kSnprintfSym: return {3, true,  1};             // snprintf(buf, size_t n, fmt, ...)
    default:           return {2, true,  kNoSizeParam};  // fprintf / sprintf / sscanf
    }
}

// The caller-side scaffold: one `main` that references each shim symbol through a
// GlobalAddr against a NOT-yet-defined callee — the shape the CST->HIR seam leaves
// behind for a `synthesize`-tagged descriptor row.
Mir buildCaller(TypeInterner& in, std::vector<std::uint32_t> const& shimSyms) {
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const u64 = in.primitive(TypeKind::U64);
    TypeId const pCh = in.pointer(in.primitive(TypeKind::Char));

    MirBuilder mb;
    mb.addFunction(in.fnSig({}, i32, CallConv::CcMS64), SymbolId{kMainSym});
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    MirInstId const buf = mb.addInst(MirOpcode::Alloca, {}, pCh, 64);
    MirLiteralValue nLit;
    nLit.value = 16;
    nLit.core  = TypeKind::U64;
    MirInstId const nArg = mb.addConst(nLit, u64);
    for (std::uint32_t sym : shimSyms) {
        ShimDecl const d = declOf(sym);
        std::vector<TypeId> params(d.fixedArgc, pCh);
        if (d.sizeParamIndex != kNoSizeParam) params[d.sizeParamIndex] = u64;
        TypeId const shimSig = in.fnSig(params, i32, CallConv::CcMS64, d.variadic);
        std::vector<MirInstId> co{mb.addGlobalAddr(SymbolId{sym}, in.pointer(shimSig))};
        for (std::uint32_t i = 0; i < d.fixedArgc; ++i)
            co.push_back(i == d.sizeParamIndex ? nArg : buf);
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
            make(kCoreVsscanfSym, "__stdio_common_vsscanf"),
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
// The CALL ITSELF, so a caller can pin an operand CHAIN (`printf`'s `_Stream` slot must
// hold THIS instruction's result) and not merely a value read out of it.
std::optional<MirInstId> coreCallOf(Mir const& mir, MirFuncId fn, std::uint32_t coreSymV) {
    for (std::uint32_t bi = 0; bi < mir.funcBlockCount(fn); ++bi) {
        MirBlockId const b = mir.funcBlockAt(fn, bi);
        for (std::uint32_t i = 0; i < mir.blockInstCount(b); ++i) {
            MirInstId const id = mir.blockInstAt(b, i);
            if (mir.instOpcode(id) != MirOpcode::Call) continue;
            auto const ops = mir.instOperands(id);
            if (ops.empty()) continue;
            if (mir.instOpcode(ops[0]) != MirOpcode::GlobalAddr) continue;
            if (mir.globalAddrSymbol(ops[0]).v == coreSymV) return id;
        }
    }
    return std::nullopt;
}

std::optional<MirInstId> apOperandOfCoreCall(Mir const& mir, MirFuncId fn,
                                             std::uint32_t coreSymV,
                                             std::size_t* operandCountOut = nullptr) {
    auto const call = coreCallOf(mir, fn, coreSymV);
    if (!call.has_value()) return std::nullopt;
    auto const ops = mir.instOperands(*call);
    if (operandCountOut != nullptr) *operandCountOut = ops.size();
    return ops.back();
}

// Read a Const operand's integer literal. Returns nullopt for a non-Const or a
// non-integer arm rather than reading it — `Mir::constLiteralIndex` aborts LOUD on the
// wrong opcode, which would take the whole binary down instead of failing one
// assertion.
std::optional<std::int64_t> constI64(Mir const& mir, MirInstId id) {
    if (mir.instOpcode(id) != MirOpcode::Const) return std::nullopt;
    MirLiteralValue const& lit = mir.literalValue(mir.constLiteralIndex(id));
    auto const* v = std::get_if<std::int64_t>(&lit.value);
    return v == nullptr ? std::nullopt : std::optional<std::int64_t>{*v};
}

// ── POSITIONAL OPERAND PROBES (TF-C119) ──────────────────────────────────────
//
// ★ WHY THESE EXIST HERE AT ALL, and why "not a Const" is not good enough. MEASURED
// (TF-C112, recorded at tests/mir/test_mir_merge.cpp:3216-3223 with the rule at :3226):
// transposing `buf` and `fmt` in the SPRINTF arm passed every assertion in BOTH stdio
// test binaries AND the MirVerifier, because both operands are `char*` and the only
// thing separating them is their POSITION. The rule that measurement produced is
// "EVERY operand of EVERY arm is pinned BY POSITION" — and the snprintf arm was the
// first to break it: `_Buffer` (`Arg 0`), `_Format` (`Arg 2`) and `_Locale` were
// unasserted here, so the identical transposition was invisible in this arm.
//
// They check the OPCODE first and only then read the opcode-specific payload:
// `Mir::instPayload` / `Mir::constLiteralIndex` abort LOUD on a wrong opcode, so a bare
// `EXPECT_EQ(opcode, …)` followed by a payload read would take the whole binary down on
// the first mismatch instead of failing one assertion and letting the rest report.
testing::AssertionResult isArgAt(Mir const& mir, MirInstId op, std::uint32_t ordinal) {
    if (mir.instOpcode(op) != MirOpcode::Arg)
        return testing::AssertionFailure()
               << "slot holds opcode #" << static_cast<int>(mir.instOpcode(op))
               << ", not the parameter `Arg " << ordinal << "`";
    // An `Arg` payload is the PACKED (position<<16)|ordinal of arg_payload.hpp, decoded
    // rather than compared raw: a raw `== ordinal` would pass only by the accident that
    // ms_x64 makes ordinal == position, and would silently start meaning something else
    // on a CC where the two diverge.
    std::uint32_t const got = arg_payload::ordinal(mir.instPayload(op));
    if (got != ordinal)
        return testing::AssertionFailure()
               << "slot holds parameter `Arg " << got << "`, want `Arg " << ordinal
               << "` — the arm forwarded the WRONG PARAMETER into this slot (a "
                  "transposition; the candidates here are all pointers, so no type "
                  "check at any tier can see it)";
    std::uint32_t const pos = arg_payload::position(mir.instPayload(op));
    if (pos != ordinal)
        return testing::AssertionFailure()
               << "`Arg " << ordinal << "` records flat call-operand position " << pos;
    return testing::AssertionSuccess();
}

// `_Locale = NULL` (the ambient locale) — a null POINTER const, NOT an integer zero.
// The literal CORE is what separates the two, and they are otherwise the same value.
testing::AssertionResult isNullPointerConst(Mir const& mir, MirInstId op) {
    if (mir.instOpcode(op) != MirOpcode::Const)
        return testing::AssertionFailure()
               << "slot holds opcode #" << static_cast<int>(mir.instOpcode(op))
               << ", not a Const (want the NULL `_Locale`)";
    MirLiteralValue const& lit = mir.literalValue(mir.constLiteralIndex(op));
    auto const* got = std::get_if<std::int64_t>(&lit.value);
    if (got == nullptr)
        return testing::AssertionFailure() << "Const does not carry an integer literal";
    if (*got != 0)
        return testing::AssertionFailure()
               << "Const is " << *got << ", want 0 — a NON-null `_Locale` hands the "
                  "core some other locale object";
    if (lit.core != TypeKind::Ptr)
        return testing::AssertionFailure()
               << "Const core is #" << static_cast<int>(lit.core) << ", want #"
               << static_cast<int>(TypeKind::Ptr)
               << " — an integer zero in the `_Locale` slot is a DIFFERENT operand "
                  "from a null pointer, and only the core tells them apart";
    return testing::AssertionSuccess();
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

// The overflow leaf is payload-free BECAUSE THE BASE HAS NO HOME BLOCK, not because
// `sprintf` happens to be the recipe under test above. Re-run on `printf`, whose named
// count is 1 rather than 2: a fork that reached the overflow leaf but still stamped the
// recipe's named-arg count onto it — the home-arm reflex — would show a DIFFERENT wrong
// payload per recipe, and a single-recipe test cannot distinguish "always 0" from
// "always the same number that happened to be 0".
TEST(SynthStdioShimVaListArm, OverflowLeafIsPayloadFreeForEveryRecipeNamedCount) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildCaller(in, {kSprintfSym, kPrintfSym});

    std::unordered_map<std::uint32_t, std::string> const recipes{
        {kSprintfSym, "sprintf"}, {kPrintfSym, "printf"}};
    std::vector<ExternImport> const externs = stdioCoreImports();
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeStdioShim(mir, in, recipes, homogeneousPointer(true),
                                    externs, rep));
    ASSERT_FALSE(rep.hasErrors());

    for (auto const& [shimSym, coreSym, named] :
         std::vector<std::tuple<std::uint32_t, std::uint32_t, char const*>>{
             {kSprintfSym, kCoreVsprintfSym, "sprintf — 2 named args"},
             {kPrintfSym, kCoreVfprintfSym, "printf — 1 named arg"}}) {
        SCOPED_TRACE(named);
        auto const shim = findFuncBySymbol(mir, shimSym);
        ASSERT_TRUE(shim.has_value());
        auto const ap = apOperandOfCoreCall(mir, *shim, coreSym);
        ASSERT_TRUE(ap.has_value());
        ASSERT_EQ(mir.instOpcode(*ap), MirOpcode::VaOverflowArgAreaAddr);
        EXPECT_EQ(mir.instPayload(*ap), 0u)
            << "payload-free on EVERY recipe — the count belongs to the home base only";
        EXPECT_EQ(countOpcode(mir, *shim, MirOpcode::VaHomeArgAreaAddr), 0u);
    }

    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep));
}

// The home leaf's payload must TRACK EACH RECIPE's named-arg count rather than be
// a constant that happens to match `sprintf`. `printf` takes one fixed parameter
// (fmt) while `fprintf`, `sprintf` and `sscanf` take two; ALL FOUR leaf-emitting
// recipes are synthesized in ONE pass here, so a hardcoded 2 reds on `printf` and a
// hardcoded 1 reds on the other three.
//
// `printf`'s body also carries a SECOND call — the `__acrt_iob_func(1)` stream
// fetch — which is exactly why the probe selects its call by callee symbol.
TEST(SynthStdioShimVaListArm, HomePayloadTracksEachRecipesNamedArgCount) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildCaller(in, {kSprintfSym, kPrintfSym, kFprintfSym, kSscanfSym});

    std::unordered_map<std::uint32_t, std::string> const recipes{
        {kSprintfSym, "sprintf"},
        {kPrintfSym, "printf"},
        {kFprintfSym, "fprintf"},
        {kSscanfSym, "sscanf"}};
    std::vector<ExternImport> const externs = stdioCoreImports();
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeStdioShim(mir, in, recipes, homogeneousPointer(false),
                                    externs, rep));
    ASSERT_FALSE(rep.hasErrors());

    // (shim symbol, ITS core, named-arg count) — each arm resolved through the core it
    // is supposed to use, so a mis-wire is a MISS here rather than a silent match. The
    // sprintf/sscanf pair matters most: the two cores share one six-parameter TypeId, so
    // swapping them type-checks everywhere.
    struct Arm { std::uint32_t shim; std::uint32_t core; std::uint32_t named;
                 char const* why; };
    for (Arm const& a : std::vector<Arm>{
             {kSprintfSym, kCoreVsprintfSym, 2, "sprintf(buf, fmt, ...) — 2 named"},
             {kPrintfSym, kCoreVfprintfSym, 1, "printf(fmt, ...) — 1 named"},
             {kFprintfSym, kCoreVfprintfSym, 2, "fprintf(stream, fmt, ...) — 2 named"},
             {kSscanfSym, kCoreVsscanfSym, 2, "sscanf(buf, fmt, ...) — 2 named"}}) {
        SCOPED_TRACE(a.why);
        auto const shim = findFuncBySymbol(mir, a.shim);
        ASSERT_TRUE(shim.has_value());
        auto const ap = apOperandOfCoreCall(mir, *shim, a.core);
        ASSERT_TRUE(ap.has_value())
            << "this arm must forward through ITS OWN core — not another arm's";
        ASSERT_EQ(mir.instOpcode(*ap), MirOpcode::VaHomeArgAreaAddr);
        EXPECT_EQ(mir.instPayload(*ap), a.named);
    }

    // ★ THE STREAM FETCH, PINNED AS A CHAIN. This assertion used to be a bare
    // `has_value()` on the accessor call — it held the iob INDEX operand in its hand and
    // threw it away, so `kIobStdout = 1 -> 2` (every `printf` silently writing to
    // STDERR) passed it. Measured, TF-C112. Now: the index must be 1, AND printf's core
    // call must take its `_Stream` from THAT call's result — a shim that fetched stdout
    // and then passed something else would satisfy the weaker form.
    auto const printfShim = findFuncBySymbol(mir, kPrintfSym);
    ASSERT_TRUE(printfShim.has_value());
    auto const iobCall = coreCallOf(mir, *printfShim, kAcrtIobFuncSym);
    ASSERT_TRUE(iobCall.has_value())
        << "printf's body must also call __acrt_iob_func for the stdout stream — if "
           "it does not, the callee-selecting probe above is not discriminating "
           "anything and this file's operand assertions are weaker than they look";
    auto const iobOps = mir.instOperands(*iobCall);
    ASSERT_EQ(iobOps.size(), 2u) << "__acrt_iob_func takes exactly one argument";
    EXPECT_EQ(constI64(mir, iobOps[1]), std::optional<std::int64_t>{kIobStdoutIndex})
        << "★ printf IS fprintf to STDOUT — __acrt_iob_func(1). Index 2 is stderr, and "
           "nothing at any tier can tell the difference: it compiles, links, loads, "
           "runs, and puts all of printf's output on the wrong stream";

    auto const printfCall = coreCallOf(mir, *printfShim, kCoreVfprintfSym);
    ASSERT_TRUE(printfCall.has_value())
        << "printf must forward through __stdio_common_vfprintf, not the sprintf core";
    auto const printfOps = mir.instOperands(*printfCall);
    ASSERT_EQ(printfOps.size(), 6u);
    EXPECT_EQ(printfOps[2].v, iobCall->v)
        << "the core's `_Stream` argument must BE the accessor call's result";

    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep));
}

// ── THE ARM WITH A THIRD NAMED PARAMETER, AND THE ONLY ONE WITH A TAIL ───────
//
// `snprintf` (TF-C119) is the family's structural outlier twice over, and both
// deviations are the silent kind.
//
// (1) IT IS THE FIRST RECIPE WITH THREE NAMED ARGS. Every leaf-emitting sibling has
// one or two, so `vaStart(2)` — the value four of the five arms pass — is a plausible
// copy-paste that anchors `ap` on the FORMAT POINTER's home slot instead of one past
// it. The core would then read the format string itself as the first `%d`. That is a
// wrong-output miscompile with no diagnostic at any tier, and it is why the payload is
// pinned here beside its siblings rather than trusted to the runtime witness alone.
//
// (2) IT IS THE ONLY RECIPE IN THIS FAMILY THAT IS NOT A SINGLE BLOCK. Its body ends
// with the UCRT header's `_Result < 0 ? -1 : _Result` clamp (`ucrt/stdio.h:1443`),
// which MIR has no Select opcode for and so spells as a compare + CondBr + two
// returning blocks — the `thrd_join` shape. ★ THIS TEST IS THE CLAMP'S ONLY WITNESS,
// and that is stated rather than glossed: on every return value measured to date the
// clamp is an IDENTITY (the core's only negative is exactly -1), so NO runtime test
// can distinguish its presence from its absence. It is defensive fidelity to the
// header, and structural fidelity is therefore the only thing that CAN be asserted
// about it. Delete the tail and this reds; nothing else in the tree would.
//
// The two non-va operands that carry snprintf's whole meaning are pinned here too,
// because they are what separate it from `sprintf` — same core, same TypeId, same
// operand count, so nothing else can tell a mixed-up pair apart:
//   * `_Options` MUST be 2 (STANDARD_SNPRINTF_BEHAVIOR). 1 is sprintf's legacy bit and
//     yields -1 on truncation instead of the C99 would-be length.
//   * `_BufferCount` MUST be the shim's own `Arg 1`, NOT a constant. The `(size_t)-1`
//     sentinel its siblings pass would let the core write past the caller's buffer.
//
// ★★ AND EVERY OTHER OPERAND IS PINNED BY POSITION TOO, which it was not until TF-C119.
// The rule comes from a MEASUREMENT, not from tidiness: transposing `buf` and `fmt` in
// the SPRINTF arm passed every assertion in both stdio test binaries and the
// MirVerifier (tests/mir/test_mir_merge.cpp:3216-3226) — both are `char*`, so only the
// POSITION distinguishes them. This arm had `_Options` and `_BufferCount` pinned but
// `_Buffer` (`Arg 0`), `_Format` (`Arg 2`) and `_Locale` bare, so the very same
// transposition was invisible HERE while being caught two files over. snprintf makes it
// worse than sprintf, not better: with a REAL `_BufferCount` the core writes `n` bytes
// into whatever the `_Buffer` slot holds, so a transposition writes formatted output
// over the caller's FORMAT STRING — through a bound the format string never agreed to.
TEST(SynthStdioShimVaListArm, SnprintfForwardsThreeNamedArgsAndClampsItsResult) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildCaller(in, {kSnprintfSym});

    std::unordered_map<std::uint32_t, std::string> const recipes{
        {kSnprintfSym, "snprintf"}};
    std::vector<ExternImport> const externs = stdioCoreImports();
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeStdioShim(mir, in, recipes, homogeneousPointer(false),
                                    externs, rep));
    ASSERT_FALSE(rep.hasErrors());

    auto const shim = findFuncBySymbol(mir, kSnprintfSym);
    ASSERT_TRUE(shim.has_value()) << "snprintf must be a synthesized definition";

    // It reuses SPRINTF's core — there is no `__stdio_common_vsnprintf` in ucrtbase to
    // reach for, so resolving through the vsprintf core is the correct wiring and not a
    // mis-wire. (A recipe that reached for a nonexistent core would fail loud in the
    // pass, which is the behaviour `MissingCoreFailsLoud` covers.)
    std::size_t operandCount = 0;
    auto const ap = apOperandOfCoreCall(mir, *shim, kCoreVsprintfSym, &operandCount);
    ASSERT_TRUE(ap.has_value())
        << "snprintf must forward through __stdio_common_vsprintf";
    EXPECT_EQ(operandCount, 7u)
        << "callee + the buffered core's six parameters "
           "(_Options, _Buffer, _BufferCount, _Format, _Locale, _ArgList)";

    ASSERT_EQ(mir.instOpcode(*ap), MirOpcode::VaHomeArgAreaAddr);
    EXPECT_EQ(mir.instPayload(*ap), 3u)
        << "★ snprintf(buf, n, fmt, ...) has THREE named args. A payload of 2 — the "
           "value four of the five sibling arms pass — anchors `ap` on the FORMAT "
           "pointer's home slot, and the core formats the format string as its first "
           "conversion. Compiles, links, loads, runs, prints garbage";

    auto const call = coreCallOf(mir, *shim, kCoreVsprintfSym);
    ASSERT_TRUE(call.has_value());
    auto const ops = mir.instOperands(*call);
    ASSERT_EQ(ops.size(), 7u);

    // ── EVERY OPERAND, BY POSITION ──
    // __stdio_common_vsprintf(_Options, _Buffer, _BufferCount, _Format, _Locale, _ArgList)
    EXPECT_EQ(constI64(mir, ops[1]), std::optional<std::int64_t>{2})
        << "_Options must be STANDARD_SNPRINTF_BEHAVIOR (1<<1). 1 is sprintf's "
           "LEGACY_VSPRINTF_NULL_TERMINATION, under which truncation returns -1 "
           "instead of C99's would-be length — measured, and silent";

    EXPECT_TRUE(isArgAt(mir, ops[2], 0))
        << "★ _Buffer is snprintf's `Arg 0`. If this reads `Arg 2`, the arm transposed "
           "buf and fmt: the shim formats the destination as a control string and "
           "writes `n` bytes of the result over the caller's FORMAT STRING. Both are "
           "char*, so no type check at any tier sees it — MEASURED on the sprintf arm, "
           "which passed both stdio test binaries and the MirVerifier under exactly "
           "this swap";

    // `_BufferCount` is the shim's OWN second parameter, not a literal. Asserted as an
    // Arg with index 1 rather than merely "not a Const": a shim that forwarded `Arg 0`
    // (the buffer pointer) as the count would also fail a not-a-Const check while being
    // a completely different bug.
    EXPECT_TRUE(isArgAt(mir, ops[3], 1))
        << "★ _BufferCount must be the caller's REAL n (`Arg 1`), not 0 (the buffer) "
           "and not 2 (the format). Its siblings pass the (size_t)-1 unbounded "
           "sentinel, which here would let the core write past the caller's buffer — "
           "memory corruption, not wrong text";
    EXPECT_FALSE(constI64(mir, ops[3]).has_value())
        << "a constant in the _BufferCount slot IS the unbounded-sentinel regression";

    EXPECT_TRUE(isArgAt(mir, ops[4], 2))
        << "★ _Format is snprintf's `Arg 2` — the other half of the transposition pair, "
           "and the operand that makes `vaStart(3)` mean what it says";
    EXPECT_TRUE(isNullPointerConst(mir, ops[5]))
        << "_Locale must be a NULL pointer (the ambient locale), the same value every "
           "sibling arm passes — an integer zero here is a different operand with the "
           "same digits";
    EXPECT_EQ(ops[6].v, ap->v)
        << "_ArgList is the LAST operand, which is what makes `apOperandOfCoreCall`'s "
           "ops.back() probe above mean `ap` at all";

    // ── THE CLAMP TAIL, the part with no runtime witness ──
    EXPECT_EQ(mir.funcBlockCount(*shim), 3u)
        << "★ entry + the two arms of `r < 0 ? -1 : r`. Every OTHER stdio recipe is a "
           "single block; collapsing this one to a bare `return r` drops the header's "
           "normalization and reds ONLY here";
    EXPECT_EQ(countOpcode(mir, *shim, MirOpcode::ICmpSlt), 1u)
        << "the clamp's predicate is a SIGNED less-than against 0 — an unsigned "
           "compare would make every negative result look large and positive";
    EXPECT_EQ(countOpcode(mir, *shim, MirOpcode::CondBr), 1u);

    // The negative arm must return the literal -1, not the raw result: that is the
    // whole content of the normalization, and a CondBr into two identical `return r`
    // blocks would satisfy every structural count above.
    bool sawMinusOne = false;
    for (std::uint32_t bi = 0; bi < mir.funcBlockCount(*shim); ++bi) {
        MirBlockId const b = mir.funcBlockAt(*shim, bi);
        for (std::uint32_t i = 0; i < mir.blockInstCount(b); ++i) {
            MirInstId const id = mir.blockInstAt(b, i);
            if (mir.instOpcode(id) != MirOpcode::Return) continue;
            auto const rops = mir.instOperands(id);
            if (rops.size() == 1 && constI64(mir, rops[0])
                == std::optional<std::int64_t>{-1})
                sawMinusOne = true;
        }
    }
    EXPECT_TRUE(sawMinusOne)
        << "one arm must `return -1`; two arms both returning the core's result would "
           "pass the block/CondBr counts while normalizing nothing";

    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep));
}

// ── THE ARM WITH NO LEAF AT ALL ──────────────────────────────────────────────
//
// `vfprintf` is the family's negative space, and this file is where that belongs:
// every other recipe's correctness question is WHICH leaf, and this one's is
// WHETHER — the answer being no. C 7.21.6.8 gives `vfprintf` a DECLARED `va_list
// ap` parameter that already points at the CALLER's first unnamed argument, so the
// shim forwards `Arg 2` verbatim.
//
// ★ A REGRESSION TO `vaStart(2)` WOULD BREAK TWO THINGS AT ONCE, both silently:
//   * `ap` would be re-derived from THIS frame — which has no varargs in it — so it
//     would point just past the shim's own three named args instead of at the
//     caller's list, and the core would format arbitrary stack;
//   * the leaf is ALSO lir_callconv's prologue-spill signal, so a non-variadic
//     function would acquire a home-area spill it never needs, and (with `vsig`) a
//     variadic declaration nothing ever fills.
// Neither shows up as a diagnostic at any tier. Both are pinned here, positively
// (the `ap` slot IS `Arg 2`) and negatively (neither leaf appears anywhere).
TEST(SynthStdioShimVaListArm, VfprintfEmitsNoVaLeafAndKeepsANonVariadicSignature) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildCaller(in, {kVfprintfSym});

    std::unordered_map<std::uint32_t, std::string> const recipes{
        {kVfprintfSym, "vfprintf"}};
    std::vector<ExternImport> const externs = stdioCoreImports();
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeStdioShim(mir, in, recipes, homogeneousPointer(false),
                                    externs, rep));
    ASSERT_FALSE(rep.hasErrors());

    auto const shim = findFuncBySymbol(mir, kVfprintfSym);
    ASSERT_TRUE(shim.has_value()) << "vfprintf must be a synthesized definition";

    TypeId const shimSig = mir.funcSignature(*shim);
    EXPECT_FALSE(in.fnIsVariadic(shimSig))
        << "★ vfprintf is NOT variadic — `ap` is a declared parameter. A variadic "
           "signature here declares a vararg frame that nothing ever fills";
    EXPECT_EQ(in.fnParams(shimSig).size(), 3u)
        << "vfprintf's arity is (stream, fmt, ap) — all three DECLARED";

    std::size_t operandCount = 0;
    auto const ap = apOperandOfCoreCall(mir, *shim, kCoreVfprintfSym, &operandCount);
    ASSERT_TRUE(ap.has_value())
        << "vfprintf must forward through the STREAM core __stdio_common_vfprintf";
    EXPECT_EQ(operandCount, std::size_t{6})
        << "callee + (_Options, _Stream, _Format, _Locale, _ArgList)";

    ASSERT_EQ(mir.instOpcode(*ap), MirOpcode::Arg)
        << "★ the `ap` slot must hold a DECLARED PARAMETER, never a va leaf";
    EXPECT_EQ(mir.argIndex(*ap), 2u)
        << "and specifically parameter 2 — forwarding `Arg 0` (the stream) or `Arg 1` "
           "(the format) as the va_list is the same class of silent wrong-bytes bug";

    EXPECT_EQ(countOpcode(mir, *shim, MirOpcode::VaHomeArgAreaAddr), 0u)
        << "★ NO home leaf: it would re-derive `ap` from a frame with no varargs, and "
           "hand lir_callconv a prologue-spill signal for a non-variadic function";
    EXPECT_EQ(countOpcode(mir, *shim, MirOpcode::VaOverflowArgAreaAddr), 0u)
        << "and no overflow leaf either — the arm emits NEITHER, on EITHER base";

    // Same refusal on the other HomogeneousPointer base: `vfprintf` has no va leaf to
    // choose, so `variadicUsesOverflowBase` must change NOTHING about this body.
    TypeInterner in2{CompilationUnitId{1}};
    Mir mir2 = buildCaller(in2, {kVfprintfSym});
    DiagnosticReporter rep2;
    ASSERT_TRUE(synthesizeStdioShim(mir2, in2, recipes, homogeneousPointer(true),
                                    stdioCoreImports(), rep2));
    auto const shim2 = findFuncBySymbol(mir2, kVfprintfSym);
    ASSERT_TRUE(shim2.has_value());
    EXPECT_EQ(countOpcode(mir2, *shim2, MirOpcode::VaOverflowArgAreaAddr), 0u)
        << "the overflow-base target must not grow a leaf the arm does not want";
    EXPECT_EQ(countOpcode(mir2, *shim2, MirOpcode::VaHomeArgAreaAddr), 0u);
    auto const ap2 = apOperandOfCoreCall(mir2, *shim2, kCoreVfprintfSym);
    ASSERT_TRUE(ap2.has_value());
    ASSERT_EQ(mir2.instOpcode(*ap2), MirOpcode::Arg);
    EXPECT_EQ(mir2.argIndex(*ap2), 2u);

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
