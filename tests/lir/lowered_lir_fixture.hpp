#pragma once

// Shared test fixture: lower a snippet of c source all the way
// to LIR, threading each phase's diagnostics. Used by liveness +
// regalloc tests to exercise the substrate end-to-end without
// re-rolling the pipeline boilerplate in each TU.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/target_schema.hpp"
#include "hir/attributes/ffi_metadata.hpp"
#include "hir/hir.hpp"
#include "hir/hir_attrs.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "lir/lir.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/lowering/hir_to_mir.hpp"
#include "mir/mir.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace dss::test_support {

struct LoweredLir {
    SemanticModel                    model;
    std::unique_ptr<CstToHirResult>  hir;
    DiagnosticReporter               hirReporter;
    HirToMirResult                   mir;
    DiagnosticReporter               mirReporter;
    std::shared_ptr<TargetSchema>    target;
    DiagnosticReporter               lirReporter;
    MirToLirResult                   lir;
};

// ★★★ WHAT THE CALLER CLAIMS ABOUT THIS LOWERING —
// D-LIR-TEST-FRONT-END-LOWERS-A-MANY-ARG-CALL-TO-NOTHING-SO-PINS-MEASURE-ZERO.
//
// This fixture used to return a `LoweredLir` and say nothing about it. Three
// reporters and three `ok` flags travelled inside the struct and NO caller read
// the first two tiers, so a source the pipeline REFUSED came back looking
// exactly like a source it lowered — and every assertion written about the
// result was then an assertion about a mutilated module. That is what made
// `tests/lir` pins vacuous, and the enum is how a pin now states which outcome
// it is asserting so the fixture can hold it to it IN BOTH DIRECTIONS.
enum class LoweringExpectation : std::uint8_t {
    // The default and the overwhelming majority: this source must lower CLEAN
    // through every tier, and no function body may vanish on the way down.
    Lowers,
    // A NEGATIVE pin: some tier MUST refuse this source. Asserted, not merely
    // tolerated — a `Refuses` source that lowers clean fails, because a refusal
    // pin that stops refusing is the regression it exists to catch.
    Refuses,
};

namespace fixture_detail {

// Every Error-severity diagnostic on one tier's reporter, rendered by SYMBOLIC
// CODE NAME and prose. The code name is the load-bearing half: it is what a
// reader greps for, and what tells a `Refuses` pin whether the refusal it got
// is the refusal it meant.
[[nodiscard]] inline std::string
renderTierErrors(char const* tier, DiagnosticReporter const& reporter) {
    std::string out;
    for (auto const& d : reporter.all()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        out += "\n    ";
        out += tier;
        out += " [";
        out += diagnosticCodeName(d.code);
        out += "] ";
        out += d.actual;
    }
    return out;
}

// The MIR / LIR function carrying `symbol`, or the invalid id when the tier
// dropped it. Both tiers key a function by the SymbolId the source declared, so
// the join is exact and survives any reordering either lowering does.
[[nodiscard]] inline MirFuncId mirFuncForSymbol(Mir const& mir, SymbolId symbol) {
    for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i) {
        auto const f = mir.funcAt(i);
        if (mir.funcSymbol(f) == symbol) return f;
    }
    return InvalidMirFunc;
}

[[nodiscard]] inline LirFuncId lirFuncForSymbol(Lir const& lir, SymbolId symbol) {
    for (std::uint32_t i = 0; i < lir.moduleFuncCount(); ++i) {
        auto const f = lir.funcAt(i);
        if (lir.funcSymbol(f) == symbol) return f;
    }
    return InvalidLirFunc;
}

[[nodiscard]] inline std::uint32_t
lirFuncInstCount(Lir const& lir, LirFuncId f) {
    std::uint32_t n = 0;
    for (std::uint32_t b = 0; b < lir.funcBlockCount(f); ++b)
        n += lir.blockInstCount(lir.funcBlockAt(f, b));
    return n;
}

// ★★★ THE REFUSAL — the closing half of
// D-LIR-TEST-FRONT-END-LOWERS-A-MANY-ARG-CALL-TO-NOTHING-SO-PINS-MEASURE-ZERO.
//
// TWO independent checks, because the fixture can lose a body in two ways and
// only ONE of them was the measured defect:
//
//   (1) A TIER REFUSED AND THE FIXTURE THREW THE REFUSAL AWAY. ✔MEASURED, and
//       it is the whole of the recorded blast radius: a C source carrying a
//       function PROTOTYPE lowers its `ExternFunction` node against a NULL
//       `ffiMap`, HIR→MIR emits `H_UnsupportedLoweringForKind` ("`mangledName`
//       is missing from the HirAttribute<FfiMetadata> side-table"), the extern
//       row is dropped, and the CALL to it then fails a second time as an
//       "HIR Ref to unbound symbol". `HirToMirResult.ok` goes FALSE and
//       `mirReporter` carries both errors — the front end is LOUD. The fixture
//       returned all of it and no caller looked, so the caller then asserted
//       over whatever survived. ⇒ the silence was never the front end's.
//
//   (2) A BODY VANISHED WITH NOTHING SAID. Not observed today, and checked
//       anyway: a future lowering that drops a function without reporting is
//       exactly the shape (1) can no longer hide, and it must not become the
//       new way to read green. Every HIR `Function` (a DEFINITION — HIR gives a
//       body-less declaration the distinct `ExternFunction` kind) must reach
//       MIR with an entry block and LIR with at least one instruction.
//
// ⚠ THE PREDICATE IS "A DEFINITION SURVIVED", NOT "IT PRODUCED ENOUGH
// INSTRUCTIONS", and the difference is what keeps a legitimately-empty body
// green. `void f(void) {}` lowers to one `ret`, and `void f(void) { return; }`
// to one `ret` as well — any threshold above 1 false-reds on real, correct C.
// A count threshold would ALSO have missed both witnessed cases (the 80-argument
// call kept 1 instruction, the spilling function kept 6), which is why the
// refusal is keyed on the REPORTED refusal rather than on a census of what
// survived it.
inline void enforceLoweringExpectation(LoweredLir const& out,
                                       LoweringExpectation expect) {
    bool const refused = out.hirReporter.hasErrors() || !out.hir->ok ||
                         out.mirReporter.hasErrors() || !out.mir.ok ||
                         out.lirReporter.hasErrors() || !out.lir.ok;

    if (expect == LoweringExpectation::Refuses) {
        if (!refused)
            ADD_FAILURE()
                << "lowerCToLir: the pin declared LoweringExpectation::Refuses "
                   "and every tier ACCEPTED this source (hir.ok, mir.ok and "
                   "lir.ok all true, zero errors on all three reporters). A "
                   "refusal pin that stopped refusing is the regression it "
                   "exists to catch — re-read the pin before relaxing it.";
        return;
    }

    if (refused) {
        ADD_FAILURE()
            << "lowerCToLir: A TIER REFUSED THIS SOURCE AND THE PIN IS "
               "ASSERTING OVER WHAT SURVIVED THE REFUSAL.\n"
               "  hir.ok=" << out.hir->ok
            << " errors=" << out.hirReporter.errorCount()
            << " | mir.ok=" << out.mir.ok
            << " errors=" << out.mirReporter.errorCount()
            << " | lir.ok=" << out.lir.ok
            << " errors=" << out.lirReporter.errorCount()
            << renderTierErrors("hir", out.hirReporter)
            << renderTierErrors("mir", out.mirReporter)
            << renderTierErrors("lir", out.lirReporter)
            << "\n  Anything this pin asserts about the result is an assertion "
               "about a MUTILATED module: the refused construct is simply "
               "absent, so a 'no bad instruction appears' check passes for the "
               "wrong reason. Either drive a source this tier lowers, rebuild "
               "the pin on hand-constructed MIR, or — if the refusal IS the "
               "subject — pass LoweringExpectation::Refuses and assert the "
               "diagnostic CODE.\n"
               "  Row: "
               "D-LIR-TEST-FRONT-END-LOWERS-A-MANY-ARG-CALL-TO-NOTHING-SO-PINS-MEASURE-ZERO";
        return;
    }

    // (2) Nothing was reported — so nothing may have gone missing either.
    auto const& hir = out.hir->hir;
    if (hir.empty()) return;
    for (auto const decl : hir.moduleDecls(hir.root())) {
        if (hir.kind(decl) != HirKind::Function) continue;
        SymbolId const symbol = hir.functionSymbol(decl);
        auto const mirFunc = mirFuncForSymbol(out.mir.mir, symbol);
        if (mirFunc == InvalidMirFunc ||
            out.mir.mir.funcBlockCount(mirFunc) == 0) {
            ADD_FAILURE()
                << "lowerCToLir: HIR carries a function DEFINITION (symbol "
                << symbol.v
                << ") that HIR->MIR dropped, with mir.ok true and NO "
                   "diagnostic. A lowering that produces nothing must say so. "
                   "Row: "
                   "D-LIR-TEST-FRONT-END-LOWERS-A-MANY-ARG-CALL-TO-NOTHING-SO-PINS-MEASURE-ZERO";
            continue;
        }
        auto const lirFunc = lirFuncForSymbol(out.lir.lir, symbol);
        if (lirFunc == InvalidLirFunc ||
            lirFuncInstCount(out.lir.lir, lirFunc) == 0) {
            ADD_FAILURE()
                << "lowerCToLir: MIR carries a function body (symbol "
                << symbol.v
                << ") that MIR->LIR lowered to NO instructions, with lir.ok "
                   "true and NO diagnostic. Every pin over this function is "
                   "asserting against an empty instruction list. Row: "
                   "D-LIR-TEST-FRONT-END-LOWERS-A-MANY-ARG-CALL-TO-NOTHING-SO-PINS-MEASURE-ZERO";
        }
    }
}

} // namespace fixture_detail

// Schema-injected overload (D-OPT-REGALLOC-EXCLUSION-BUFFER closure,
// 2026-06-11): a test exercising substrate behavior against a
// MUTATED target schema (the tests/test_support/mutate_target_schema
// substrate) hands the pre-built schema in directly; the c →
// HIR → MIR half of the pipeline is target-independent, so only the
// MIR→LIR step consumes it.
[[nodiscard]] inline LoweredLir
lowerCToLir(std::string src, std::shared_ptr<TargetSchema> target,
                  std::uint16_t mirCcIndex = 0,
                  LoweringExpectation expect = LoweringExpectation::Lowers) {
    auto loaded = GrammarSchema::loadShipped("c");
    if (!loaded) { ADD_FAILURE() << "loadShipped(c) failed"; std::abort(); }
    UnitBuilder builder{*loaded, DiagnosticBudget::libraryDefault()};
    builder.addInMemory(std::move(src), "<mem>");
    auto cu    = std::make_shared<CompilationUnit>(std::move(builder).finish());
    if (target == nullptr) {
        ADD_FAILURE() << "lowerCToLir: null target schema";
        std::abort();
    }
    // FC12b (D-FC12B-WIN64-VARIADIC-CALLEE): thread the SELECTED CC's va_list
    // strategy into analyze() so the `va_list` TYPE matches the ABI (SysV
    // __va_list_tag[1] vs Win64 char*) — mirrors compile_pipeline.cpp. `mirCcIndex`
    // selects which CC drives BOTH the analyze() strategy AND the MIR config below,
    // so a test can lower a variadic source under ms_x64 (cc 1) end-to-end.
    std::optional<VaListStrategy> vaStrategy;
    if (auto const* cc = target->callingConvention(mirCcIndex);
        cc != nullptr && cc->vaListLayout.has_value()) {
        vaStrategy = cc->vaListLayout->strategy;
    }
    // ★★ INLINE-ASM P5: THE TARGET IS THREADED INTO `analyze`, AND WITHOUT IT AN
    // ASM OPERAND CANNOT REACH LIR AT ALL. A GNU constraint LETTER is a MACHINE
    // fact — `"r"` means whatever the processor's `asmConstraints` facet says —
    // so with no target in scope `HirInlineAsmOperand::regClassResolved` stays
    // false and `lowerInlineAsm` REFUSES rather than guessing a register bank
    // (`… has no resolved register class`, D-CSUBSET-INLINE-ASM-OPERANDS).
    // ⚠ THIS IS THE SAME OMISSION `test_mir_lowering_c.cpp`'s harness had
    // one tier up, and it hides the FEATURE, not just the fixture: every asm
    // source lowered here failed at MIR, so nothing downstream was ever reached.
    // `compile_pipeline.cpp` passes it; a harness that does not cannot test asm.
    // The schema is held by `LoweredLir::target`, which outlives the model —
    // `analyze` takes it NON-OWNING.
    auto model = analyze(cu, DiagnosticBudget::libraryDefault(),
                         DataModel::Lp64, std::nullopt, vaStrategy, std::nullopt,
                         std::nullopt, LongDoubleFormat::None, target.get());
    DiagnosticReporter hirReporter;
    auto hir = lowerToHir(model, hirReporter);
    DiagnosticReporter mirReporter;
    MirLoweringConfig mirCfg;
    mirCfg.globalsAllowFloat = (*loaded)->hirLowering().globalsConstEval.allowFloat;
    // Thread the SELECTED CC's by-value aggregate + va_list params into MIR
    // lowering, mirroring compile_pipeline.cpp — so a struct-by-value OR a
    // variadic-callee (va_start/va_arg) source lowers through THIS fixture too
    // (mirCcIndex 0 = the target's primary convention: sysv_amd64 on x86_64,
    // aapcs64 on arm64; 1 = ms_x64 on x86_64 for the Win64 variadic pins).
    if (auto const* cc = target->callingConvention(mirCcIndex)) {
        mirCfg.aggregateLayout           = target->aggregateLayout();
        mirCfg.aggregateLayoutLoaded     = target->aggregateLayoutLoaded();
        mirCfg.aggregateClassification   = cc->aggregateClassification;
        mirCfg.aggregateMaxRegBytes      = cc->aggregateMaxRegBytes;
        mirCfg.aggregateSretViaHiddenArg = !cc->indirectResultRegister.has_value();
        mirCfg.argSlotAligned            = cc->slotAligned;
        mirCfg.argGprCount               =
            static_cast<std::uint32_t>(cc->argGprs.size());
        mirCfg.argFprCount               =
            static_cast<std::uint32_t>(cc->argFprs.size());
        mirCfg.aggregateStackExhaustsRegisters =
            cc->aggregateStackExhaustsRegisters;
        // D-CODEGEN-APPLE-ARM64-STACK-ARGS-NOT-NATURALLY-PACKED: the stacked-arg
        // packing rules decide `va_start`'s overflow base for a callee whose named
        // params overflow the arg registers. ⚠ THREADED HERE AS PART OF ADDING THE
        // FIELD, not as a follow-up — the docblock below records this fixture
        // silently reading a default THREE times (TF-C78 / TF-C81 / TF-C92) and
        // every test concluding the feature was absent. A default here would make
        // the Apple `va_start` pin PASS against slot packing.
        mirCfg.stackArgPacking           = cc->stackArgPacking;
        mirCfg.vaListLayout              = cc->vaListLayout;
    }
    // D-CSUBSET-VLA (C1b): thread the captured VLA size-expr map so a `int a[n]`
    // source lowers its runtime-operand Alloca through THIS fixture too (else the
    // VLA has no bound → fail loud). The intervening maps stay nullptr (defaults).
    //
    // ★★★ `inlineAsmPool` IS THREADED, AND WITHOUT IT NO LIR TEST CAN SEE AN
    // `__asm__` AT ALL (D-TEST-LIR-FIXTURE-BLIND-TO-INLINE-ASM).
    // `lowerToMir`'s own docblock is explicit that `nullptr` is NOT "no asm" — a
    // descriptor-carrying statement reached with no pool is a loud
    // `H_UnsupportedLoweringForKind`, because the alternative (inventing an
    // operand-less, clobber-less descriptor) is the silent miscompile the P5 arc
    // exists to prevent. So every source with an asm statement failed at MIR here
    // and never reached the tier this directory tests.
    // ⚠ THIS IS THE SAME MISS `test_mir_lowering_c.cpp` RECORDS HITTING
    // THREE TIMES ONE TIER UP (TF-C78 / TF-C81 / TF-C92: a map threaded through
    // the product and through `compile_pipeline.cpp` while every unit test read a
    // nullptr and saw the feature as absent). Treat updating this call as PART OF
    // adding a map, never as a follow-up.
    // ★★★ THE FFI MAP, AND WHY IT IS BUILT HERE RATHER THAN PASSED `nullptr`
    // (D-LIR-TEST-FRONT-END-LOWERS-A-MANY-ARG-CALL-TO-NOTHING-SO-PINS-MEASURE-ZERO).
    //
    // ⚠⚠ THIS ARGUMENT IS THE ONE MAP IN THIS CALL WHOSE ABSENCE IS NOT A
    // DEFAULT. Every other `nullptr` below means "the source declared no such
    // attribute", which is true of most sources and harmless. `ffiMap == nullptr`
    // means the HIR→MIR extern pre-pass finds NO `FfiMetadata` for an
    // `ExternFunction` node and REFUSES it — `H_UnsupportedLoweringForKind`,
    // "`mangledName` is missing from the HirAttribute<FfiMetadata> side-table …
    // tests must attach the attribute manually until then". The extern row is
    // then absent, so the CALL to it fails a second time as an "HIR Ref to
    // unbound symbol", and the whole call statement disappears from MIR.
    //
    // ✔MEASURED, and it is the mechanism behind the row above: `double g(…);
    // double f(void){ return g(…); }` reached LIR as ONE instruction with
    // `mir.ok` FALSE and two MIR errors nobody read — for an 80-argument call
    // AND for a 1-argument one. The trigger was never argument count or stack
    // pressure; it is the mere PRESENCE OF A PROTOTYPE. The control proves it:
    // the same 80 doubles as PARAMETERS of a definition (no prototype, no
    // extern node) lower to 82 instructions, clean, on the same fixture.
    //
    // Each row is bound as a bare cross-TU reference — its own canonical name
    // and `noLibraryBinding` (C 6.2.2p5, the `HirExternRecord` contract) — which
    // is both what the source actually said and the only FORMAT-AGNOSTIC choice
    // available here: an import LIBRARY is selected per object format, and this
    // fixture is parameterized by TARGET only. `compile_pipeline.cpp` folds the
    // per-format `libraryOverride` map instead, because there the format is in
    // scope; nothing at this tier consumes the library, so nothing is lost.
    HirFfiMap ffiMap{hir->hir};
    for (auto const& rec : hir->externDecls) {
        FfiMetadata md;
        md.mangledName      = rec.canonicalName;
        md.noLibraryBinding = true;
        md.version          = rec.version;
        md.isEagerImport    = rec.isEagerImport;
        ffiMap.set(rec.node, std::move(md));
    }
    HirToMirResult mir = lowerToMir(hir->hir, hir->literalPool,
                                    model.lattice().interner(), mirReporter,
                                    &hir->sourceMap, mirCfg, &ffiMap,
                                    /*linkageMap=*/nullptr, /*mutabilityMap=*/nullptr,
                                    /*volatileMap=*/nullptr, /*alignmentMap=*/nullptr,
                                    /*threadLocalMap=*/nullptr,
                                    &hir->vlaSizeExprBySymbol,
                                    /*sizeofVlaSymMap=*/nullptr,
                                    /*typedefVlaOriginMap=*/nullptr,
                                    /*synthRecipeMap=*/nullptr,
                                    /*returnsTwiceMap=*/nullptr,
                                    /*noInlineMap=*/nullptr,
                                    /*alwaysInlineMap=*/nullptr,
                                    /*noOptimizeMap=*/nullptr,
                                    /*noSanitizeThreadMap=*/nullptr,
                                    &hir->inlineAsmPool,
                                    /*inlineDefinitionMap=*/nullptr,
                                    // D-C-LABEL-ADDRESS-IN-A-STATIC-INITIALIZER-REFUSED:
                                    // without it a `&&label` in a static initializer is
                                    // REFUSED at MIR and never reaches this tier at all
                                    // — the exact shape of the miss the block above
                                    // records hitting three times.
                                    &hir->enclosingFunctionMap);
    DiagnosticReporter lirReporter;
    MirToLirResult lir = lowerToLir(mir.mir, *target,
                                    model.lattice().interner(),
                                    lirReporter);
    LoweredLir out{std::move(model), std::move(hir), std::move(hirReporter),
                   std::move(mir), std::move(mirReporter), std::move(target),
                   std::move(lirReporter), std::move(lir)};
    // ★★★ THE FIXTURE NOW STATES ITS OWN VERDICT. Everything above this line
    // ran exactly as it always did; what changed is that the result no longer
    // leaves here unexamined. See `fixture_detail::enforceLoweringExpectation`.
    fixture_detail::enforceLoweringExpectation(out, expect);
    return out;
}

// `targetName` defaults to "x86_64" (the historic behavior — every
// pre-existing caller is unaffected). A test that needs the lowering
// against a different target's calling convention (e.g. ARM64's
// AAPCS64 link-register frame discipline) passes the target name
// explicitly; the whole lower->LIR pipeline is target-parameterized.
[[nodiscard]] inline LoweredLir
lowerCToLir(std::string src, std::string targetName = "x86_64",
                  std::uint16_t mirCcIndex = 0,
                  LoweringExpectation expect = LoweringExpectation::Lowers) {
    auto target = TargetSchema::loadShipped(targetName);
    if (!target) {
        ADD_FAILURE() << "loadShipped(" << targetName << ") failed";
        std::abort();
    }
    return lowerCToLir(std::move(src), std::move(*target), mirCcIndex, expect);
}

} // namespace dss::test_support
