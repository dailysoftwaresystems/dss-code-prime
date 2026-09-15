// ★★ [[D-MIR-DYLIB-SELF-CALL-BYPASSES-WEAK-COALESCING]] — the OPTIMIZER half of
// gate rule 2, and the half whose absence was MEASURED rather than reasoned.
//
// The row's call-site routing sends a shared library's call to its own
// preemptible definition through the LOADER instead of branching to the local
// body. That routing is DEFEATED, in the release arm only, if the inliner has
// already spliced the body away — the reference survives as a PLT stub nobody
// reaches while the caller carries a baked-in answer. ✔MEASURED exactly that
// before this gate arm existed: with the routing alone, `--config=release` on a
// DSS-built `.so` folded a STRONG global callee to a constant INSIDE its caller
// while `call <st@plt>` sat beside it unused, so the debug and release arms of
// one source answered differently.
//
// GATE RULE 2 HAS TWO INDEPENDENT HALVES and the pass only ever had the first:
//   (a) LINK-TIME, universal — a WEAK definition may be replaced by a strong
//       one of the same name when the objects are linked. Pinned by
//       `Inlining.WeakCalleeIsNotInlined` in test_inlining.cpp; NOT restated
//       here, so a change that broke it would redden THAT test and not be
//       masked by this file.
//   (b) LOAD-TIME, declared — in an artifact whose loader may hand the process
//       another image's body for this name. That is what this file pins, and
//       the arms below are the four cells of the decision, not one happy path.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/optimizer.hpp"
#include "opt/passes/inlining.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

using namespace dss;

namespace {

constexpr std::uint32_t kCalleeSym = 50u;
constexpr std::uint32_t kCallerSym = 100u;

// `int callee(void) { return 7; }` + `int caller(void) { return callee(); }`,
// with the CALLEE's binding and visibility as the only variables. The callee is
// one instruction, so nothing but the gate can refuse the splice.
[[nodiscard]] Mir buildModule(TypeInterner& interner, SymbolBinding binding,
                              SymbolVisibility visibility) {
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig(std::span<TypeId const>{}, i32,
                                        CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{kCalleeSym}, binding, visibility);
    {
        MirBlockId const bb = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(bb);
        MirLiteralValue lv;
        lv.value = std::int64_t{7};
        lv.core  = TypeKind::I32;
        mb.addReturn(mb.addConst(lv, i32));
    }
    mb.addFunction(fnSig, SymbolId{kCallerSym}, SymbolBinding::Global,
                   SymbolVisibility::Default);
    {
        MirBlockId const bb = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(bb);
        MirInstId const addr = mb.addGlobalAddr(SymbolId{kCalleeSym}, fnSig);
        std::array<MirInstId, 1> ops{addr};
        mb.addReturn(mb.addInst(MirOpcode::Call, ops, i32));
    }
    return std::move(mb).finish();
}

// Run the pass through the LEDGER overload — the one the engine uses, and the
// only one that carries the declared set.
[[nodiscard]] std::uint32_t
inlinedCallCount(Mir& mir, TypeInterner const& interner,
                 DiagnosticReporter& rep,
                 std::vector<SymbolBinding> const& preemptible) {
    opt::passes::InlineGrowthLedger ledger;
    auto const r = opt::passes::runInlining(
        mir, interner, rep, opt::kMaxInlineThreshold,
        opt::kDefaultInlineCallerGrowthPercent, ledger,
        /*maintainMarkers=*/true,
        std::span<SymbolBinding const>{preemptible});
    EXPECT_TRUE(r.ok);
    return r.callsInlined;
}

} // namespace

// DIRECTION 1 — the defect. A STRONG GLOBAL callee at default visibility is
// interposable in an ELF shared object (gcc 13.3.0 and clang 18.1.3 both emit
// `call <st@plt>` for it there), so its body is not necessarily the body that
// runs and it must not be spliced.
// RED-ON-DISABLE: delete the `definitionIsPreemptible` arm of gate rule 2 in
// `inlineLegalityGate` → callsInlined becomes 1 and this fails.
TEST(PreemptibleDefinitionInlining, ADeclaredPreemptibleGlobalCalleeIsRefused) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildModule(interner, SymbolBinding::Global,
                          SymbolVisibility::Default);
    DiagnosticReporter rep;
    EXPECT_EQ(inlinedCallCount(mir, interner, rep,
                               {SymbolBinding::Global, SymbolBinding::Weak}),
              0u)
        << "in an artifact whose loader may replace this definition, splicing "
           "its body bakes in an answer the process may not agree with — the "
           "same miscompile as inlining a WEAK body, one binding over";
}

// DIRECTION 2 — the SCOPE, and the arm that keeps the set declared rather than
// hard-coded. Under the Mach-O dylib declaration (`["weak"]`) the same STRONG
// callee IS inlinable, because dyld's two-level namespace binds a strong dylib
// definition locally. A change that folded the two ecosystems' answers into one
// rule passes DIRECTION 1 and fails here.
TEST(PreemptibleDefinitionInlining,
     AGlobalCalleeOutsideTheDeclarationIsStillInlined) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildModule(interner, SymbolBinding::Global,
                          SymbolVisibility::Default);
    DiagnosticReporter rep;
    EXPECT_EQ(inlinedCallCount(mir, interner, rep, {SymbolBinding::Weak}), 1u)
        << "`global` is not in this format's declaration, so nothing at load "
           "time can replace the body and the splice stays legal";
}

// DIRECTION 3 — VISIBILITY is a precondition no declaration can widen. A
// hidden-visibility callee is in no image's dynamic export set, so no loader
// can replace it — and both references keep inlining such a callee in the very
// `.so` whose default-visibility callees they refuse to bind locally.
TEST(PreemptibleDefinitionInlining, AHiddenCalleeIsStillInlined) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildModule(interner, SymbolBinding::Global,
                          SymbolVisibility::Hidden);
    DiagnosticReporter rep;
    EXPECT_EQ(inlinedCallCount(mir, interner, rep,
                               {SymbolBinding::Global, SymbolBinding::Weak}),
              1u)
        << "hidden visibility is the source's own opt-out from interposition; "
           "refusing to inline it would cost every such helper a call for a "
           "replacement that cannot happen";
}

// DIRECTION 4 — the BACK-COMPAT arm. Every format that declares nothing (every
// relocatable object, every static library, every main-executable flavour) must
// inline exactly as before. This is the direction a reader would not think to
// check and the one every shipped artifact depends on.
TEST(PreemptibleDefinitionInlining, NoDeclarationLeavesInliningUnchanged) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildModule(interner, SymbolBinding::Global,
                          SymbolVisibility::Default);
    DiagnosticReporter rep;
    EXPECT_EQ(inlinedCallCount(mir, interner, rep, /*preemptible=*/{}), 1u);
}
