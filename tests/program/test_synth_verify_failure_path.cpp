// D-MIR-SYNTH-PASSES-UNVERIFIED-ON-SINGLE-CU-PATH — the BEHAVIOURAL half of the
// post-synthesis verify: what `verifySynthesizedModule` DOES when the module it
// is handed is broken.
//
// ── ⚠ WHY THIS EXISTS: THE FUNCTION SHIPPED WITH NO CALLER IN tests/ ────────
// ✔MEASURED by cycle P63's independent review: `grep -rn 'verifySynthesizedModule'
// tests/` returned two hits and BOTH were string literals — the name passed to the
// source-order guard, and a comment in a CMakeLists. Nothing CALLED it. So the
// row's closure rested on two facts that are both true and both beside the point:
// the `MirVerifier`'s marker rule fires on a stale marker (pinned in tests/mir),
// and the two call sites exist and are positioned (pinned by
// `program/test_synth_verify_seam_guard`). What had no pin at all was the seam
// FUNCTION's own contract — that a failing verify becomes a REPORTED compile
// failure, carrying the `I_VerifierFailure` tier diagnostic ON TOP OF the specific
// invariant the verifier already named. Its message enumerates the four synthesis
// passes it stands behind; had that list been wrong, nothing would have noticed.
//
// ── WHAT IS PINNED, AND IN WHICH DIRECTION ─────────────────────────────────
// Two arms over ONE module, so the difference between them is exactly the defect:
//
//   CONTROL  a well-formed module verifies — returns TRUE and reports NOTHING.
//            Without it, "the broken module was refused" is equally consistent
//            with "this verifier refuses everything", which pins nothing.
//   BROKEN   the SAME module with ONE `StructCfMarker` falsified on a reachable,
//            non-entry block — returns FALSE, and the reporter carries BOTH
//            `I_StructCfMismatch` (the rule that broke) AND `I_VerifierFailure`
//            (the tier that stands behind it). The two together are the contract:
//            the verifier names the RULE, this function names the TIER, and a
//            reader learns that a SYNTHESIZED body produced it rather than the
//            optimizer or the front end.
//
// ★ THE FALSIFICATION IS READ, NEVER ASSUMED. The canonical marker is derived
// first (`rederiveStructCfMarkers`), then read off the block, then replaced with a
// value chosen to differ from whatever was read. So the mismatch is certain rather
// than incidental, and no future change to the derivation can quietly turn this
// into a module that was never broken.
//
// ★ NOTHING IS ADDED TO `src/`. The row prescribed a test-only lever inside the
// SEH pass that would hand-build a wrong-arity call; that is an ADD-direction
// mutant and this cycle forbids those, so the defect is injected into a module
// THIS TEST OWNS instead. `test_synth_verify_seam_guard`'s header records the
// declination and the reasoning in full.
//
// HOST-INDEPENDENT: nothing is compiled and nothing is run — the subject is a
// hand-built `Mir` and one exported driver-tier function.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_struct_markers.hpp"
#include "program/compile_pipeline.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

using namespace dss;

namespace {

// The smallest legal module this seam can be handed: one function, two blocks,
// `entry: br tail` / `tail: return 0`. Two blocks rather than one because the
// falsified block below must be REACHABLE and must not be the entry — the
// verifier skips unreachable blocks (I_UnreachableBlock owns those), and pinning
// the entry's marker would be pinning a special case.
[[nodiscard]] Mir buildTwoBlockModule(TypeInterner& in) {
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const sig = in.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(sig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tail  = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    mb.addBr(tail);
    mb.beginBlock(tail);
    MirLiteralValue zero;
    zero.value = std::int64_t{0};
    zero.core  = TypeKind::I32;
    mb.addReturn(mb.addConst(std::move(zero), i32));
    Mir mir = std::move(mb).finish();
    // The real pipeline hands this seam a module whose markers are already
    // canonical (every producer re-derives at its own site). Mirror that, so the
    // BROKEN arm below is measuring an injected defect and not the fixture's own
    // hand-stamped defaults.
    rederiveStructCfMarkers(mir);
    return mir;
}

[[nodiscard]] bool reportsCode(DiagnosticReporter const& rep, DiagnosticCode code) {
    for (auto const& d : rep.all()) {
        if (d.code == code) return true;
    }
    return false;
}

[[nodiscard]] std::string renderAll(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) {
        out += "\n  [" + std::to_string(static_cast<std::uint16_t>(d.code)) + "] "
             + d.actual;
    }
    return out.empty() ? std::string{"\n  (nothing reported)"} : out;
}

// CONTROL — named, and it is the arm that makes the other one mean something.
TEST(SynthVerifyFailurePath, WellFormedModuleVerifiesAndReportsNothing) {
    TypeInterner       in{CompilationUnitId{1}};
    Mir                mir = buildTwoBlockModule(in);
    DiagnosticReporter rep;
    EXPECT_TRUE(verifySynthesizedModule(mir, in, rep))
        << "a well-formed two-block module must pass the post-synthesis verify — "
           "if it does not, the BROKEN arm below proves nothing about the defect "
           "it injects" << renderAll(rep);
    EXPECT_TRUE(rep.all().empty())
        << "a passing verify must report nothing at all: this seam adds its tier "
           "diagnostic ONLY on failure" << renderAll(rep);
}

TEST(SynthVerifyFailurePath, BrokenModuleIsRefusedNamingBothRuleAndTier) {
    TypeInterner in{CompilationUnitId{1}};
    Mir          mir = buildTwoBlockModule(in);

    auto const func = mir.funcAt(0);
    ASSERT_GE(mir.funcBlockCount(func), 2u);
    MirBlockId const     victim = mir.funcBlockAt(func, 1);
    StructCfMarker const stored = mir.blockMarker(victim);
    StructCfMarker const falsified = (stored == StructCfMarker::LoopHeader)
                                         ? StructCfMarker::IfJoin
                                         : StructCfMarker::LoopHeader;
    ASSERT_NE(stored, falsified);
    mir.setBlockMarker(victim, falsified);

    DiagnosticReporter rep;
    EXPECT_FALSE(verifySynthesizedModule(mir, in, rep))
        << "a module carrying a marker its own verifier rejects must be REFUSED "
           "at the post-synthesis seam — a true return here is the silent "
           "miscompile this row was opened to prevent" << renderAll(rep);

    EXPECT_TRUE(reportsCode(rep, DiagnosticCode::I_StructCfMismatch))
        << "the verifier must still name the RULE that broke — this seam adds to "
           "the verifier's report, it never replaces it" << renderAll(rep);

    EXPECT_TRUE(reportsCode(rep, DiagnosticCode::I_VerifierFailure))
        << "the seam must add its own I_VerifierFailure naming the synthesis "
           "TIER. Without it a reader sees a broken invariant with no indication "
           "that a synthesized body produced it, which is the whole reason this "
           "call is one function and not an open-coded MirVerifier at each seam"
        << renderAll(rep);

    // The tier diagnostic is the compiler's account of WHERE the defect came
    // from, so its wording is a pinned fact rather than prose: it must name the
    // synthesis tier and it must not read as a user error.
    bool tierNamed = false;
    for (auto const& d : rep.all()) {
        if (d.code != DiagnosticCode::I_VerifierFailure) continue;
        tierNamed = d.actual.find("AFTER the synthesis passes") != std::string::npos
                 && d.actual.find("compiler defect") != std::string::npos;
    }
    EXPECT_TRUE(tierNamed)
        << "the I_VerifierFailure text must say the failure is AFTER the "
           "synthesis passes and that it is a compiler defect, never a program "
           "error — that sentence is what tells a user this is not their bug"
        << renderAll(rep);
}

} // namespace
