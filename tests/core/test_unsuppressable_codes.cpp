#include "core/types/diagnostic_reporter.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/unsuppressable_codes.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>

// D-FF2-UNSUPP closed-table pins.
//
// PINS the contract that severity-gating codes (architectural exclusions,
// pending-plan announcements, lowering / verifier / linker invariants)
// bypass SILENCING mutations (--suppress drops + overrides demotion)
// through DiagnosticReporter::applyPolicy. Elevation
// (--warnings-as-errors) is INTENTIONALLY allowed — it strengthens the
// signal rather than defeating the gate (eb2c6c7 refinement). Without
// these pins, a regression that drops the `isUnsuppressable` gate
// would silently re-open the silent-drop surface every such code was
// introduced to close — exactly the bug class D-FF2-UNSUPP closes.

using namespace dss;

namespace {

ParseDiagnostic makeDiag(DiagnosticCode code,
                         DiagnosticSeverity sev = DiagnosticSeverity::Error) {
    ParseDiagnostic d;
    d.code     = code;
    d.severity = sev;
    d.buffer   = BufferId{1};
    d.span     = SourceSpan::of(0, 1);
    d.actual   = "x";
    return d;
}

} // namespace

TEST(UnsuppressableCodes, MembershipIncludesCoreArchitecturalCodes) {
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::H_ExternHasInitializer));
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::D_TargetAbiModelUnsupportedByDriver));
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::F_FfiIngestAbiModelUnsupported));
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::F_FfiIngestEmptyCanonical));
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::F_ShippedHeaderNotFound))
        << "FF11: a missing system header `#include <h>` must be unsuppressable "
           "(suppressing it would compile a program calling an undeclared symbol)";
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::F_ShippedLibDescriptorMalformed))
        << "v0.0.2 V2-2: a malformed shipped-lib JSON descriptor must be "
           "unsuppressable (suppressing it would silently drop shipped externs)";
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::F_ShippedLibUnsupportedType))
        << "v0.0.2 V2-2: a shipped-lib signature that fails to decode must be "
           "unsuppressable (suppressing it would silently drop the import)";
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::F_ShippedHeaderUnavailableForTarget))
        << "p18 c8: a header unavailable on the active target's object-format "
           "must be unsuppressable (suppressing it would resume semantic "
           "analysis and inject the header's surfaces on the WRONG platform — "
           "the wrong-platform silent miscompile this gate closes)";
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::F_ShippedStructVariantAmbiguous))
        << "plan 25: >1 per-target struct `variants` matching the active target "
           "must be unsuppressable (suppressing it would re-open the silent "
           "'pick the first' wrong-layout surface — e.g. an under-specified "
           "when:{arch:\"x86_64\"} matching both x86_64-elf and x86_64-pe → the "
           "linux struct layout used on windows)";
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::A_ImmediateOperandOutOfRange))
        << "v0.0.2 V2-1: an immediate/offset too wide for its fixed32 slot "
           "must be unsuppressable (suppressing it would silently truncate to "
           "a WRONG machine-code constant — a wrong syscall number / frame "
           "size / stack slot)";
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::S_IncompleteTypeMember))
        << "c24 D-CSUBSET-SELF-REFERENTIAL-STRUCT: a DIRECT (non-pointer) member "
           "of an incomplete composite (a struct-by-value cycle, e.g. "
           "`struct N { struct N n; }`) must be unsuppressable — suppressing it "
           "would let the member fold its size to 0 (the incomplete composite has "
           "no layout), a silent wrong-bytes layout. Named pin (ListSelfConsistent "
           "would still pass if this member were dropped from the table).";
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::P_PreprocessorErrorDirective))
        << "TF-C70 D-CPP-ERROR-WARNING (C23 6.10.5): `#error` is the author's "
           "OWN declaration that this configuration must not be built. It is "
           "reached only when its conditional group is live, so suppressing it "
           "would compile exactly the configuration the source says is invalid "
           "— and silently, since nothing downstream re-reports it. Named pin "
           "(ListSelfConsistent would still pass if this member were dropped "
           "from the table).";
    EXPECT_FALSE(isUnsuppressable(DiagnosticCode::P_PreprocessorWarningDirective))
        << "TF-C70 (C23 6.10.6): `#warning` is advisory and translation "
           "continues, so it MUST stay suppressable — the opposite of its "
           "`#error` sibling. Pinning the asymmetry keeps a future 'both "
           "preprocessor directive codes belong in the table' edit honest.";
    // The WARNING code is deliberately NOT in the closed table, so
    // `ListSelfConsistent`'s "no member names Unknown" sweep never reaches it —
    // and `diagnosticCodeName`'s switch has no `default:`, so a MISSING case
    // falls through to the "Unknown" sentinel silently rather than failing to
    // compile. (The ERROR code is covered for free by that sweep, via the
    // table.) This is the only pin on the warning code's name.
    EXPECT_NE(
        std::string{
            diagnosticCodeName(DiagnosticCode::P_PreprocessorWarningDirective)},
        "Unknown")
        << "P_PreprocessorWarningDirective has no `diagnosticCodeName` case — "
           "the code would render as the Unknown sentinel in every diagnostic, "
           "--suppress name, and log line";
    EXPECT_FALSE(isUnsuppressable(DiagnosticCode::None))
        << "None must never be a member (guards the array-size-bumped-without-"
           "adding-the-entry bug at runtime too)";
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::D_PlanNotLanded));
}

TEST(UnsuppressableCodes, BothH2SplitArmsAreUnsuppressable) {
    // Post-fold #11 type-design CRITICAL pin: H2 split (post-fold #9)
    // introduced `H_ExternDeclMalformed` alongside the pre-existing
    // `H_UnsupportedLoweringForKind` arm in `lowerExternDecl`. Both
    // terminate lowering with `return errorNode(node)` + gate ok via
    // errorCount. Pre-fix the new arm was missing from the closed-
    // table, silently re-opening half the gate's coverage for any
    // future grammar admitting parse-recovery shapes that reach
    // lowering. This pin ensures the H2 split's two arms travel
    // together — any future re-split must add the new code here too.
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::H_UnsupportedLoweringForKind))
        << "engine-config arm: --suppress would let an extern decl "
           "with no kindByChild config silently fall through to "
           "makeExternGlobal, re-opening the D-FF2-3 silent-drop";
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::H_ExternDeclMalformed))
        << "parse-recovery arm: --suppress would let a malformed "
           "extern decl (incomplete CST) silently fall through, "
           "same silent-drop surface as the engine-config arm";
}

TEST(UnsuppressableCodes, EntryPointResolvesToExternIsUnsuppressable) {
    // D-LK10-ENTRY-EXTERN-ENTRY-DIAG: extern-as-entry is a schema
    // misconfiguration that would produce a runnable binary whose
    // `_start` jumps to an unresolved IAT stub address — SEGV at
    // process entry with no diagnostic trail. Suppressing this code
    // would let the linker emit such a binary silently. The
    // `ListSelfConsistent` test below iterates the closed table and
    // proves every member is unsuppressable, but a refactor that
    // dropped THIS member from the table would still pass that test
    // (the table shrinks consistently). Named pin closes the
    // membership-by-name regression (test-analyzer severity 8 on the
    // 3rd-order audit of 39897eb).
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::K_EntryPointResolvesToExtern))
        << "suppressing this would silently re-open the SEGV-at-"
           "process-entry surface this code was added to close";
}

TEST(UnsuppressableCodes, RegularDiagnosticsRemainSuppressable) {
    // Stylistic / non-gating codes MUST stay suppressable so --suppress
    // remains a useful policy tool for the codes it was designed for.
    EXPECT_FALSE(isUnsuppressable(DiagnosticCode::P_UnexpectedToken));
    EXPECT_FALSE(isUnsuppressable(DiagnosticCode::P_DeprecatedSyntax));
    EXPECT_FALSE(isUnsuppressable(DiagnosticCode::P_AmbiguousToken));
}

TEST(UnsuppressableCodes, AutoInferenceRejectsAreUnsuppressable) {
    // FC17.5 (D-CSUBSET-AUTO-TYPE-INFERENCE, C23 6.7.9): the ★C3 backfill
    // seam — the inference arm is the ONLY tier that types these symbols at
    // Pass 1.5, and Pass 2's decl arm BACKFILLS `rec.type =
    // initializer-type` for any still-unresolved declarator-mode symbol. A
    // SUPPRESSED rejection would therefore silently adopt the initializer's
    // type and compile the very form the constraint forbids (`static x=5;`
    // as implicit-int, multi-declarator auto, a NullptrT-typed object headed
    // for the 0xA014 MIR tripwire). Named per-code pins (ListSelfConsistent
    // would still pass if any member were dropped from the table).
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::S_AutoRequiresSingleDeclarator));
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::S_AutoRequiresPlainIdentifier));
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::S_AutoRequiresInitializer));
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::S_AutoInferenceInvalid));
}

TEST(UnsuppressableCodes, ThreadLocalRejectsAreUnsuppressable) {
    // TLS C1 (D-CSUBSET-THREAD-LOCAL, C11/C23 6.7.1 + 6.6p9): every
    // thread-storage constraint violation ships wrong STORAGE bytes when
    // suppressed, not a missed lint — the block-scope form would lower as a
    // per-call automatic, the redeclaration mismatch would bind half the
    // accesses to the wrong storage, and the address-constant form would
    // emit an abs64 whose resolved value is a link-time tpoff bit-cast into
    // a data slot (the arc's CRIT-1 silent garbage pointer). Named per-code
    // pins (ListSelfConsistent would still pass if any member were dropped
    // from the table).
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::S_ThreadLocalOnFunction));
    EXPECT_TRUE(isUnsuppressable(
        DiagnosticCode::S_ThreadLocalRequiresStaticOrExtern));
    EXPECT_TRUE(isUnsuppressable(
        DiagnosticCode::S_ThreadLocalRedeclarationMismatch));
    EXPECT_TRUE(isUnsuppressable(
        DiagnosticCode::S_ThreadLocalAddressNotConstant));
    EXPECT_TRUE(isUnsuppressable(
        DiagnosticCode::S_ThreadLocalInvalidCombination));
    // VLA C1a (D-CSUBSET-VLA): the VLA constraint codes + the MIR verifier
    // invariant + the C1a→C1b LIR fail-loud. Suppressing the static-storage form
    // would carry a runtime-sized vlaArray into the static→global lowering; and the
    // LIR boundary, suppressed, would silently lower a runtime alloca as a 1-slot
    // scalar (MINOR-3). Named per-code pins.
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::S_VlaWithStaticStorage));
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::S_VlaSizeNotInteger));
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::I_VlaAllocaOperandInvalid));
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::L_VlaDynamicAllocaUnsupported));
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::L_VlaNonLeafFrameUnsupported));
    // S_VlaMultiDimUnsupported (0xE052) is deliberately NOT here: VLA C3 lifted all
    // four of its reject sites, so it has no emit site and an unemittable code
    // cannot be suppressed. Pinned as a NEGATIVE below rather than merely dropped,
    // so re-adding the dead row reds a test instead of passing unnoticed.
    EXPECT_FALSE(isUnsuppressable(DiagnosticCode::S_VlaMultiDimUnsupported))
        << "0xE052 was RETIRED at VLA C3 (`int a[n][m]` / `int a[5][n]` / "
           "`int a[n][5]` all compile rc=0). A retired code must LEAVE the closed "
           "table — membership is what made it read as load-bearing.";
}

// Inline-asm operand binding (D-CSUBSET-INLINE-ASM-OPERANDS +
// D-CSUBSET-INLINE-ASM-TEXT + D-ASM-DUPLICATE-SYMBOLIC-NAME-BINDS-THE-WRONG-OPERAND,
// 0xE065..0xE06C). All EIGHT are members on PRONG (1): each refuses a
// construct that has a SECOND CANDIDATE LOWERING a silenced compiler would take
// unannounced — an unbound operand, an alternative the binder chose itself, a
// full-width register where a narrow view was written, a dropped clobber, a
// vanished template, a placeholder dropped or emitted raw, and — the eighth,
// added by cycle P20 — one of the two bindings a repeated symbolic name
// introduces, discarded by a first-match lookup.
// ★ THE EIGHTH IS THE ONLY ONE WHOSE SUPPRESSED BEHAVIOUR WAS **MEASURED**
// RATHER THAN REASONED: ✔2026-08-19, before the refusal existed, `[out] "=r"(r),
// [v] "=r"(d) : [v] "r"(a)` compiled rc=0 at debug AND release through the
// shipped CLI and returned the OUTPUT's value where the input's was written.
// Named per-code pins, because `ListSelfConsistent` would still pass if any one
// row were dropped from the table.
//
// ★ THE RATIONALE IS ASSERTED NON-EMPTY PER CODE, not just membership. `why` is
// the text `D_SuppressRequestIgnored` shows the operator whose --suppress is
// being refused; a member with an empty one refuses SILENTLY, which is the
// shape this whole table exists to forbid. The consteval
// `kUnsuppressableEntriesAllExplainThemselves` catches an empty literal at
// build time — this catches a row wired to the wrong constant, which that check
// cannot see.
TEST(UnsuppressableCodes, InlineAsmOperandBindingCodesAreUnsuppressable) {
    int checked = 0;
    for (auto const code :
         {DiagnosticCode::S_InlineAsmConstraintLetterUndeclared,
          DiagnosticCode::S_InlineAsmConstraintUnsupportedForm,
          DiagnosticCode::S_InlineAsmOperandModifierUnsupported,
          DiagnosticCode::S_InlineAsmClobberUnknown,
          DiagnosticCode::S_InlineAsmTemplateUnparsable,
          DiagnosticCode::S_InlineAsmPlaceholderOutOfRange,
          DiagnosticCode::S_InlineAsmPlaceholderInBasicTemplate,
          DiagnosticCode::S_InlineAsmDuplicateSymbolicName}) {
        EXPECT_TRUE(isUnsuppressable(code))
            << diagnosticCodeName(code)
            << ": suppressed, the operand binding silently takes its second "
               "candidate lowering and the build ships wrong bytes green";
        EXPECT_FALSE(unsuppressableRationale(code).empty())
            << diagnosticCodeName(code)
            << " is a member with no recorded reason, so a --suppress request "
               "naming it would be refused with an empty explanation";

        // ...and the refusal is a live reporter behaviour, not just a
        // predicate: the user names the code and the diagnostic lands anyway,
        // still carrying its text and still counting toward the exit gate.
        DiagnosticReporter::Config cfg;
        cfg.policy.suppress.insert(code);
        DiagnosticReporter r{cfg};
        auto d   = makeDiag(code);
        d.actual = "constraint letter 'a' is not declared by target 'arm64'";
        r.report(std::move(d));

        ASSERT_EQ(r.all().size(), 1u)
            << diagnosticCodeName(code) << " was silenced by --suppress";
        EXPECT_EQ(r.all()[0].actual,
                  "constraint letter 'a' is not declared by target 'arm64'")
            << "the delivered diagnostic must still NAME the offending "
               "construct";
        EXPECT_EQ(r.errorCount(), 1u)
            << "and must still count toward the exit-code gate";
        ++checked;
    }
    // Non-vacuity: a counter incremented in the loop, not the list's length.
    EXPECT_EQ(checked, 8);

    // ★ THE NEGATIVE THAT KEEPS THE EIGHT HONEST. One code of this arc is
    // deliberately NOT a member, and it is the same one P1 left out:
    // `volatile volatile` suppressed compiles to exactly what `volatile`
    // means, so there is no second candidate reading to pick silently. Pinned
    // rather than merely omitted, so a future batch-sweep that adds "the
    // inline-asm codes" as a family reds here instead of quietly loosening the
    // criterion the other seven were admitted under.
    EXPECT_FALSE(isUnsuppressable(DiagnosticCode::S_InlineAsmDuplicateQualifier))
        << "0xE064 became a closed-table member. Membership is decided on the "
           "suppression criterion per code, not per arc: a repeated qualifier "
           "is redundant, not ambiguous, so suppressing it ships no wrong "
           "bytes and hides no build failure. Read its block in "
           "`parse_diagnostic.hpp` before changing this.";
    // ⚠ AND THE PAIR IS THE POINT: a repeated QUALIFIER stays suppressible while
    // a repeated NAME (0xE06C) does not. The two look like one family and are
    // not — `volatile volatile` has ONE candidate reading, a name used twice has
    // exactly two and the compiler picks one in silence. Both pins in one test
    // is what stops the next sweep collapsing them.
}

// ─────────────────────────────────────────────────────────────────────────────
// THE BUILD-HOOK CODES: ADMITTED ON THE SUPPRESSION CRITERION, RE-EXAMINED.
//
// ★ WHY THIS IS RE-ARGUED RATHER THAN INHERITED. When 0xD017/0xD018 joined
// (AP5, 2026-08-12) the rationale gave TWO reasons, and one of them was that
// membership also buys immunity from the reporter's diagnostic cap. That is a
// side-channel: it makes the cap's behaviour a reason to widen a table whose
// criterion is about `--suppress`, and the criterion is what gets loosened to
// pay for it. Delivery now has its own property (`DiagnosticDelivery`), so the
// cap half is answered elsewhere and membership has to stand on suppression
// alone.
//
// ✔ IT DOES, ON PRONG (2) OF THE TABLE'S CRITERION — "the build fails with
// nothing said". MEASURED at `program/build_scripts.cpp`: `runBuildScripts`
// returns false whether or not its report survived, and the driver turns that
// into `return 1`. So no wrong artifact ships (prong (1) does not apply, and
// claiming it would be the loosening this cycle removed), but suppression
// converts a documented VISIBILITY control into a mute-the-failure control:
// a pre-build hook fails before any compilation begins, so there is no other
// diagnostic in the stream and stderr is empty EXACTLY. That is a purer
// instance of prong (2) than the S_* members that established it.
TEST(UnsuppressableCodes, BuildHookFailuresAreRefusedBySuppressAndKeepTheirText) {
    for (auto const code : {DiagnosticCode::D_ScriptSpawnFailed,
                            DiagnosticCode::D_ScriptExitedNonZero}) {
        EXPECT_TRUE(isUnsuppressable(code))
            << diagnosticCodeName(code)
            << ": suppressing a build-hook failure leaves a non-zero exit "
               "with nothing on stderr saying which hook failed";

        // The refusal is observable at the reporter, which is where it
        // matters: the user named the code and the diagnostic lands anyway.
        DiagnosticReporter::Config cfg;
        cfg.policy.suppress.insert(code);
        DiagnosticReporter r{cfg};
        auto d   = makeDiag(code);
        d.actual = "build script 'python' ran and exited with status 3";
        r.report(std::move(d));

        ASSERT_EQ(r.all().size(), 1u)
            << diagnosticCodeName(code) << " was silenced by --suppress";
        // ★ THE TEXT, not just the code. A code with its prose stripped is
        // the same empty-explanation shape by a different route — the
        // operator still cannot tell which hook failed or why.
        EXPECT_EQ(r.all()[0].actual,
                  "build script 'python' ran and exited with status 3")
            << "the delivered diagnostic must still NAME what failed";
        EXPECT_EQ(r.errorCount(), 1u)
            << "and must still count toward the exit-code gate";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// THE FOUR `dependsOn` GIT CODES — ✅ LANDED 2026-08-14, AND THIS PIN TURNED
// AROUND WITH THEM.
//
// It used to be the NEGATIVE (`DependsOnGitCodesStaySuppressableUntilTheyAre-
// Emitted`), asserting all four were NOT members. That was correct while none
// of them had an emit site: `EveryMemberHasAnEmitSiteOrIsMarkedRetired` below
// reds on a member nothing emits, so the recorded verdicts — judged
// individually on 2026-08-13, all four QUALIFYING — could not be acted on. The
// AP6 lane landed the sites in `program/dependency_cache.cpp` and the rows in
// the same change, so the deferral is over and the assertion is the other way
// round.
//
// ⚠ IT IS NOT MERELY DELETED, and that matters. The reason to keep a pin here
// is unchanged in kind: a future batch-sweep could remove all four (or, worse,
// three of four) and direction A below would go QUIET rather than red, because
// a non-member with an emit site is a perfectly ordinary thing. Naming them on
// the membership side is what makes the removal of a row visible.
TEST(UnsuppressableCodes, DependsOnGitCodesAreRefusedBySuppressAndKeepTheirText) {
    for (auto const code : {DiagnosticCode::D_DependencyGitNotFound,
                            DiagnosticCode::D_DependencyGitAcquireFailed,
                            DiagnosticCode::D_DependencyGitFetchFallback,
                            DiagnosticCode::D_DependencyGitNameCollision}) {
        EXPECT_TRUE(isUnsuppressable(code))
            << diagnosticCodeName(code)
            << " left the closed table. Three of these four are prong (2) — "
               "the build stops and this diagnostic is the only statement of "
               "why — and D_DependencyGitFetchFallback is prong (1): the build "
               "PROCEEDS on sources it could not refresh, so suppressing it "
               "ships an artifact from a revision the operator did not choose, "
               "green. Read the verdict block beside the D_Script* rows in "
               "`unsuppressable_codes.cpp` before removing a row.";

        // ...and "unsuppressable" is a live behaviour, not just a predicate:
        // the user names the code and the diagnostic lands anyway, WITH its
        // text. A code delivered with its prose stripped is the same
        // empty-explanation shape by a different route.
        DiagnosticReporter::Config cfg;
        cfg.policy.suppress.insert(code);
        DiagnosticReporter r{cfg};
        auto d   = makeDiag(code, DiagnosticSeverity::Info);
        d.actual = "git dependency 'https://example.invalid/bar.git'";
        r.report(std::move(d));

        ASSERT_EQ(r.all().size(), 1u)
            << diagnosticCodeName(code) << " was silenced by --suppress";
        EXPECT_EQ(r.all()[0].actual,
                  "git dependency 'https://example.invalid/bar.git'")
            << "the delivered diagnostic must still NAME the dependency";
    }
}

// ★ THE ONE ASYMMETRY IN THE FAMILY, PINNED SEPARATELY BECAUSE IT IS THE ONE A
// "TIDY-UP" WOULD BREAK. `D_DependencyGitFetchFallback` is Info by written
// decision: `applyPolicy`'s `--warnings-as-errors` arm promotes every Warning
// code-agnostically, so at Warning severity this notice would fail every
// offline build — the exact outcome it exists to prevent. Membership does NOT
// exempt a code from that elevation (it gates silencing only), so the severity
// at the emit site is the whole defence, and this asserts that being a member
// did not change it.
TEST(UnsuppressableCodes, FetchFallbackMembershipDoesNotMakeItFailAWarningsAsErrorsBuild) {
    ASSERT_TRUE(isUnsuppressable(DiagnosticCode::D_DependencyGitFetchFallback));

    DiagnosticReporter::Config cfg;
    cfg.policy.warningsAsErrors = true;
    DiagnosticReporter r{cfg};
    r.report(makeDiag(DiagnosticCode::D_DependencyGitFetchFallback,
                      DiagnosticSeverity::Info));

    ASSERT_EQ(r.all().size(), 1u);
    EXPECT_EQ(r.all()[0].severity, DiagnosticSeverity::Info)
        << "the elevation arm must leave Info alone; promoting it would make "
           "every --warnings-as-errors build fail the moment the network did";
    EXPECT_EQ(r.errorCount(), 0u)
        << "and the offline build must still be a passing build";
}

// The split the whole cycle is about, pinned as a property of the two
// mechanisms rather than of any one code: membership must not be reachable as
// a way to obtain cap-immunity, because the property that grants cap-immunity
// exists on its own and grants nothing else.
TEST(UnsuppressableCodes, DeliveryAndSuppressionAreIndependentAxes) {
    // Delivery without membership: a plain suppressable code, guaranteed.
    ASSERT_FALSE(isUnsuppressable(DiagnosticCode::P_DeprecatedSyntax));
    {
        DiagnosticReporter::Config cfg;
        cfg.maxDiagnostics = 1;
        cfg.dedupWindow    = 0;
        DiagnosticReporter r{cfg};
        r.report(makeDiag(DiagnosticCode::P_UnexpectedToken));
        auto filler = makeDiag(DiagnosticCode::P_UnknownToken);
        filler.span = SourceSpan::of(5, 6);
        r.report(std::move(filler));
        ASSERT_TRUE(r.hitCap());

        auto d     = makeDiag(DiagnosticCode::P_DeprecatedSyntax,
                              DiagnosticSeverity::Info);
        d.span     = SourceSpan::of(9, 10);
        d.delivery = DiagnosticDelivery::Guaranteed;
        r.report(std::move(d));

        bool landed = false;
        for (auto const& kept : r.all()) {
            landed = landed || kept.code == DiagnosticCode::P_DeprecatedSyntax;
        }
        EXPECT_TRUE(landed)
            << "cap-immunity must be obtainable WITHOUT joining the closed "
               "table — that reachability is what made membership a "
               "side-channel for a property it never carried";
    }
    // Membership without a delivery request: the axes do not need each
    // other in either direction.
    {
        DiagnosticReporter::Config cfg;
        cfg.policy.suppress.insert(DiagnosticCode::H_ExternHasInitializer);
        DiagnosticReporter r{cfg};
        auto d = makeDiag(DiagnosticCode::H_ExternHasInitializer);
        ASSERT_EQ(d.delivery, DiagnosticDelivery::Capped);
        r.report(std::move(d));
        EXPECT_EQ(r.all().size(), 1u)
            << "a member is still un-silenceable with delivery left at its "
               "default; membership answers suppression on its own";
    }
}

TEST(UnsuppressableCodes, ListSelfConsistent) {
    // Every member of the public closed-table view must report
    // unsuppressable; no member duplicated; every entry must be a
    // real enumerated DiagnosticCode (not a static_cast from a stray
    // integer that slipped past type-checking); no member listed at
    // the "Unknown" sentinel.
    auto const codes = unsuppressableCodes();
    EXPECT_GT(codes.size(), 0u);
    std::unordered_set<DiagnosticCode> seen;
    for (auto const& e : codes) {
        auto const c = e.code;
        EXPECT_TRUE(isUnsuppressable(c))
            << "code " << diagnosticCodeName(c)
            << " is in the closed-table list but isUnsuppressable returns false";
        EXPECT_TRUE(seen.insert(c).second)
            << "duplicate entry for " << diagnosticCodeName(c);
        EXPECT_STRNE(diagnosticCodeName(c).data(), "Unknown")
            << "code value 0x" << std::hex << static_cast<unsigned>(c)
            << " does not name a real DiagnosticCode (sentinel reached)";
    }
}

TEST(Reporter, SuppressIsIgnoredForUnsuppressableCode) {
    // The signature pin: a user setting --suppress=H_ExternHasInitializer
    // does NOT silence the diagnostic. Closes the silent-drop surface
    // where the suppress flag would otherwise re-open the failure
    // mode the H_ExternHasInitializer fold permanently closes.
    DiagnosticReporter::Config cfg;
    cfg.policy.suppress.insert(DiagnosticCode::H_ExternHasInitializer);
    DiagnosticReporter r{cfg};
    r.report(makeDiag(DiagnosticCode::H_ExternHasInitializer));
    EXPECT_EQ(r.all().size(), 1u)
        << "unsuppressable code must reach `all_` despite --suppress";
    EXPECT_EQ(r.errorCount(), 1u)
        << "errorCount must reflect the unsuppressable error so "
           "downstream exit-code gates fire correctly";
}

TEST(Reporter, OverrideCannotDemoteUnsuppressableCodeBelowError) {
    // The suppress check is the headline, but `overrides` is a parallel
    // attack surface: a user could demote H_ExternHasInitializer to
    // Warning, then errorCount() would return 0 even though the
    // diagnostic IS in `all_`. Bypassing overrides on unsuppressable
    // codes closes that variant.
    DiagnosticReporter::Config cfg;
    cfg.policy.overrides[DiagnosticCode::H_ExternHasInitializer]
        = DiagnosticSeverity::Warning;
    DiagnosticReporter r{cfg};
    r.report(makeDiag(DiagnosticCode::H_ExternHasInitializer,
                      DiagnosticSeverity::Error));
    ASSERT_EQ(r.all().size(), 1u);
    EXPECT_EQ(r.all()[0].severity, DiagnosticSeverity::Error)
        << "overrides must not demote an unsuppressable code; "
           "applyPolicy short-circuits before overrides apply";
    EXPECT_EQ(r.errorCount(), 1u);
}

TEST(Reporter, UnsuppressableBypassesGlobalCap) {
    // Post-fold #11 silent-failure F1 CRITICAL: pre-fold a noisy parse
    // hitting `maxDiagnostics` would mask every subsequent
    // unsuppressable code. With the F1 fix, unsuppressable codes
    // bypass the global cap entirely — they always reach `all_` so
    // `errorCount()` correctly reflects the severity-gating diagnostic.
    DiagnosticReporter::Config cfg;
    cfg.maxDiagnostics = 1;
    DiagnosticReporter r{cfg};
    // Fill the cap with regular suppressable errors — 1st lands, 2nd
    // trips the cap marker. (cap check fires when `all_.size() >=
    // maxDiagnostics` on the SECOND report.)
    r.report(makeDiag(DiagnosticCode::P_UnexpectedToken));
    auto d2 = makeDiag(DiagnosticCode::P_UnknownToken);
    d2.span = SourceSpan::of(5, 6);  // distinct from d1 so dedup doesn't collapse
    r.report(std::move(d2));
    ASSERT_TRUE(r.hitCap())
        << "test setup: cap must be hit before the unsuppressable report";
    // Unsuppressable code MUST still land despite hitCap_.
    r.report(makeDiag(DiagnosticCode::H_ExternHasInitializer));
    bool found = false;
    for (auto const& d : r.all()) {
        if (d.code == DiagnosticCode::H_ExternHasInitializer) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found)
        << "unsuppressable code must bypass the global cap; pre-F1 "
           "the `if (hitCap_) return;` short-circuit would have eaten "
           "this report";
}

TEST(Reporter, UnsuppressableBypassesPerCodeCap) {
    // Post-fold #11 F1: per-code coalescing must not silently drop a
    // 2nd / 3rd / ... unsuppressable diagnostic. 12 I_NotDominated
    // violations in a module against maxPerCode=10 must all surface.
    DiagnosticReporter::Config cfg;
    cfg.maxPerCode = 1;
    DiagnosticReporter r{cfg};
    for (int i = 0; i < 5; ++i) {
        auto d = makeDiag(DiagnosticCode::I_NotDominated);
        d.span = SourceSpan::of(static_cast<ByteOffset>(i),
                                static_cast<ByteOffset>(i + 1));
        r.report(std::move(d));
    }
    EXPECT_EQ(r.all().size(), 5u)
        << "all 5 unsuppressable diagnostics must reach `all_` "
           "despite maxPerCode=1; pre-F1 would have coalesced to 1";
}

TEST(Reporter, UnsuppressableBypassesDedupWindow) {
    // Post-fold #11 F1: dedup window is policy too. Two structurally
    // identical unsuppressable diagnostics in the dedup window MUST
    // both land — every instance gates ok.
    DiagnosticReporter::Config cfg;
    cfg.dedupWindow = 4;  // default
    DiagnosticReporter r{cfg};
    auto d1 = makeDiag(DiagnosticCode::K_ImageWriteShort);
    auto d2 = makeDiag(DiagnosticCode::K_ImageWriteShort);
    // Same code, same buffer, same span, same actual — dedup would
    // normally collapse these.
    r.report(std::move(d1));
    r.report(std::move(d2));
    EXPECT_EQ(r.all().size(), 2u)
        << "dedup must not collapse unsuppressable diagnostics; "
           "the second image-write failure carries distinct signal";
}

TEST(Reporter, WarningsAsErrorsElevatesUnsuppressableWarning) {
    // 2nd-order audit refinement on commit 0bd2f41 (2026-06-01):
    // F_BinaryReaderPartialCorruption is a Warning-severity
    // unsuppressable code. The original "skip all policy mutation"
    // semantic was too broad — strict-mode operators legitimately
    // want Warning-severity unsuppressable codes promoted to Error
    // so they increment errorCount. Refined: the unsuppressable gate
    // bypasses SILENCING (suppress + overrides demotion) but NOT
    // elevation (warningsAsErrors). Closes the conflict between
    // post-fold #14's "warningsAsErrors elevates" intent and
    // commit 0bd2f41's addition to kUnsuppressableCodes.
    DiagnosticReporter::Config cfg;
    cfg.policy.warningsAsErrors = true;
    DiagnosticReporter r{cfg};
    auto d = makeDiag(DiagnosticCode::F_BinaryReaderPartialCorruption,
                      DiagnosticSeverity::Warning);
    r.report(std::move(d));
    ASSERT_EQ(r.all().size(), 1u);
    EXPECT_EQ(r.all()[0].severity, DiagnosticSeverity::Error)
        << "warningsAsErrors MUST promote unsuppressable Warning "
           "to Error so strict-mode operators get fail-loud exit "
           "code on partial-corruption signals";
}

TEST(Reporter, SuppressIsIgnoredForUnsuppressableWarning) {
    // eb2c6c7 audit fold (test-analyzer Finding E): the pre-existing
    // SuppressIsIgnoredForUnsuppressableCode pin uses an Error-severity
    // member (H_ExternHasInitializer). Pin the same silencing-bypass
    // contract for a Warning-severity unsuppressable member
    // (F_BinaryReaderPartialCorruption) so a future regression that
    // splits the gate by severity ("warnings can be suppressed because
    // they're advisory") is caught.
    DiagnosticReporter::Config cfg;
    cfg.policy.suppress.insert(DiagnosticCode::F_BinaryReaderPartialCorruption);
    DiagnosticReporter r{cfg};
    r.report(makeDiag(DiagnosticCode::F_BinaryReaderPartialCorruption,
                      DiagnosticSeverity::Warning));
    EXPECT_EQ(r.all().size(), 1u)
        << "Warning-severity unsuppressable code must reach `all_` "
           "despite --suppress; silencing bypass applies regardless of "
           "producer severity";
}

TEST(Reporter, OverrideCannotDemoteUnsuppressableWarning) {
    // eb2c6c7 audit fold (test-analyzer Finding E): symmetric pin for
    // the overrides arm of the silencing-bypass contract. Demoting a
    // Warning-severity unsuppressable code to Info would silently
    // ablate exit-code semantics under --warnings-as-errors (Info
    // diagnostics don't trip the WAE elevation gate). The bypass MUST
    // hold regardless of producer severity.
    DiagnosticReporter::Config cfg;
    cfg.policy.overrides[DiagnosticCode::F_BinaryReaderPartialCorruption]
        = DiagnosticSeverity::Info;
    DiagnosticReporter r{cfg};
    r.report(makeDiag(DiagnosticCode::F_BinaryReaderPartialCorruption,
                      DiagnosticSeverity::Warning));
    ASSERT_EQ(r.all().size(), 1u);
    EXPECT_EQ(r.all()[0].severity, DiagnosticSeverity::Warning)
        << "overrides demotion (Warning→Info) must be blocked on "
           "unsuppressable codes; severity must remain at producer level";
}

TEST(Reporter, UnsuppressableHonorsWarningsAsErrorsEvenWhenSuppressed) {
    // eb2c6c7 audit fold (test-analyzer Finding F MEDIUM, folded
    // because cheap): pin BOTH halves of the silencing-vs-elevation
    // refinement in one fixture. A combined policy of (a) suppress on
    // the code AND (b) warningsAsErrors must produce a single Error-
    // severity diagnostic in `all_`. A regression that re-applies
    // silencing under WAE would collapse all_ to size 0; a regression
    // that drops WAE elevation under suppress would leave severity at
    // Warning. Both half-failures are caught here.
    DiagnosticReporter::Config cfg;
    cfg.policy.suppress.insert(DiagnosticCode::F_BinaryReaderPartialCorruption);
    cfg.policy.warningsAsErrors = true;
    DiagnosticReporter r{cfg};
    r.report(makeDiag(DiagnosticCode::F_BinaryReaderPartialCorruption,
                      DiagnosticSeverity::Warning));
    ASSERT_EQ(r.all().size(), 1u)
        << "silencing-bypass: --suppress must not drop the diagnostic";
    EXPECT_EQ(r.all()[0].severity, DiagnosticSeverity::Error)
        << "elevation-honored: --warnings-as-errors must promote the "
           "Warning to Error even when suppress is also set";
    EXPECT_EQ(r.errorCount(), 1u);
}

TEST(Reporter, ForceReportRoutesUnsuppressableThroughApplyPolicyBypass) {
    // Post-fold #13 code-review fold: the prior test ("...BypassNotJustCap")
    // was duplicative — `forceReport` bypasses cap unconditionally for
    // EVERY code (any suppressable code would also have landed). The
    // load-bearing convergence: when an unsuppressable code is ALSO in
    // the user's --suppress set, `forceReport` MUST still land it via
    // applyPolicy's unsuppressable short-circuit. Pre-fix any future
    // "forceReport really forces everything past suppress" shortcut
    // would still satisfy this test — the contract pinned is the
    // unsuppressable predicate is consulted by BOTH entry points.
    DiagnosticReporter::Config cfg;
    cfg.policy.suppress.insert(DiagnosticCode::K_SymbolUndefined);
    DiagnosticReporter r{cfg};
    // forceReport an unsuppressable code that's ALSO suppressed —
    // the unsuppressable gate (inside applyPolicy) wins.
    r.forceReport(makeDiag(DiagnosticCode::K_SymbolUndefined));
    ASSERT_EQ(r.all().size(), 1u)
        << "forceReport routed through applyPolicy must honor "
           "the unsuppressable short-circuit even when the code "
           "is in --suppress; without it the diagnostic would drop";
    EXPECT_EQ(r.all()[0].code, DiagnosticCode::K_SymbolUndefined);
}

TEST(Reporter, ForceReportStillDropsRegularSuppressedCode) {
    // Companion to the above: forceReport bypasses CAPS but does
    // still consult applyPolicy. A normal suppressable code that's
    // in the suppress set must still be dropped under forceReport.
    // Pre-fix any forceReport-shortcut around applyPolicy would
    // silently emit suppressed codes — a regression class we pin.
    DiagnosticReporter::Config cfg;
    cfg.policy.suppress.insert(DiagnosticCode::P_DeprecatedSyntax);
    DiagnosticReporter r{cfg};
    r.forceReport(makeDiag(DiagnosticCode::P_DeprecatedSyntax,
                           DiagnosticSeverity::Warning));
    EXPECT_EQ(r.all().size(), 0u)
        << "forceReport routes through applyPolicy; suppressed normal "
           "codes still drop";
}

TEST(Reporter, NormalCodeSuppressedAlongsideUnsuppressableInSameReporter) {
    // Co-existence: suppressing a regular code on the same reporter must
    // still drop the regular code while leaving the unsuppressable one
    // through. Pins that the two policies don't interfere.
    DiagnosticReporter::Config cfg;
    cfg.policy.suppress.insert(DiagnosticCode::P_DeprecatedSyntax);
    cfg.policy.suppress.insert(DiagnosticCode::H_ExternHasInitializer);
    DiagnosticReporter r{cfg};
    r.report(makeDiag(DiagnosticCode::P_DeprecatedSyntax,
                      DiagnosticSeverity::Warning));
    r.report(makeDiag(DiagnosticCode::H_ExternHasInitializer));
    ASSERT_EQ(r.all().size(), 1u);
    EXPECT_EQ(r.all()[0].code, DiagnosticCode::H_ExternHasInitializer);
}

// ─────────────────────────────────────────────────────────────────────────────
// THE CLASS PROPERTY: no DEAD code may sit in the closed table.
//
// WHY A PROPERTY AND NOT MORE PINS. Every `EXPECT_TRUE(isUnsuppressable(X))`
// above is an INSTANCE, and an instance list cannot fail — it only ever asserts
// about names someone thought to type. The table had accumulated FIVE members
// whose emit sites were deleted cycles earlier (`F_FfiNoImportLibraryForFormat`,
// `F_FfiResolveLibrarySymbolAbsent`, `S_BitIntWidthAboveC1Limit`,
// `S_BitIntWideMulDivUnsupported`, `S_VlaMultiDimUnsupported`), and no instance
// pin could have found them because each one's pin said TRUE and stayed true.
// Two of the five ALSO carried doc comments still describing the retired surface
// as live, and one of those stale comments produced a wrong C23 conformance
// claim that only a probe caught. Membership in a closed table of
// silent-miscompile guards is precisely what makes a dead name read as
// load-bearing to the next reader, so the table is where the property belongs.
//
// THE PROPERTY, in three directions that together admit no silent absence:
//   A. a member that is NOT marked RETIRED must have a real emit site in `src/`
//   B. a code marked RETIRED must NOT be a member (an unemittable code cannot be
//      suppressed — the `S_VolatilePointeeNotSupported` 0xE025 precedent)
//   C. a code marked RETIRED must have NO emit site
// C is what makes A un-defeatable: without it, the cheapest way to green a
// failing A is to staple a RETIRED marker onto a LIVE code, and the marker would
// then be the lie instead of the row. The escape hatch is explicit, sits AT the
// declaration, and is itself checked.
//
// ★ THE SCAN IS WHOLE-FILE, NOT LINE-BY-LINE, AND THAT IS LOAD-BEARING.
// clang-format wraps long emit sites after the `::` —
//     d.code = DiagnosticCode::
//         S_EntryShapeNotDeclared;
// — so a per-line matcher reported FIVE genuinely-live codes as dead (MEASURED
// 2026-08-10: S_EntryShapeNotDeclared, S_TypeNameDeclaratorNotAbstract,
// S_InvalidEnumUnderlyingType, S_EnumeratorValueOutOfRange,
// L_IndirectCalleeClobberedByArgSetup). Whitespace after `::` must be skipped.
//
// Comments are BLANKED before matching, because a code named in prose is not an
// emit site — `hir_to_mir.cpp` and `semantic_analyzer.cpp` both discuss retired
// codes by name. String literals are deliberately NOT stripped: MEASURED, doing
// so changes neither the reference count (735) nor the outcome, so the extra
// machinery would be untested weight.
//
// ⚠ KNOWN LIMIT, STATED SO A FUTURE FALSE RED IS DIAGNOSED AND NOT PAPERED OVER
// [[D-TEST-UNSUPPRESSABLE-EMIT-SITE-SCAN-BLIND-TO-CONFIG-DECLARED-CODES]].
// A diagnostic can also be emitted WITHOUT any `DiagnosticCode::` reference, by
// naming the code as a STRING in language config and letting the name→code
// resolver in `grammar_schema_json.cpp` bind it (MEASURED: `S_StaticStorageInForInit`
// is declared that way in two `src/dss-config/` rows). This scan cannot see that
// path. Today it costs nothing — MEASURED 2026-08-12, all 141 members have a
// direct `DiagnosticCode::` site, so no member relies on it — but the day a
// config-only-emitted code JOINS this table, direction A will red on a code that
// is genuinely live. ★ THE FIX IS TO TEACH THE SCANNER THE CONFIG PATH (also walk
// `src/dss-config/**.json` for the code name as a VALUE, never inside a
// `$comment`), NOT to mark a live code RETIRED — direction C exists precisely to
// make that shortcut fail. Widening the matcher was not done pre-emptively
// because an unreachable arm is an untested arm; the anchor carries the trigger.
namespace {

namespace fs = std::filesystem;

// The three files that MENTION every code by construction (the enum, the
// name-mapping switch, the closed table). Scanning them would make every code
// look emitted, which is the one way this property could silently pass.
bool isDeclarationFile(fs::path const& p) {
    auto const name = p.filename().string();
    if (name == "parse_diagnostic.hpp" || name == "parse_diagnostic.cpp"
        || name == "unsuppressable_codes.cpp")
        return p.parent_path().filename().string() == "types";
    return false;
}

// Replace every comment body with spaces, preserving newlines and total length.
std::string blankComments(std::string_view src) {
    std::string out;
    out.reserve(src.size());
    bool inBlock = false;
    bool inLine  = false;
    for (std::size_t i = 0; i < src.size();) {
        char const c   = src[i];
        auto const two = src.substr(i, 2);
        if (inBlock) {
            if (two == "*/") { inBlock = false; out += "  "; i += 2; continue; }
            out += (c == '\n') ? '\n' : ' ';
            ++i;
            continue;
        }
        if (inLine) {
            if (c == '\n') { inLine = false; out += '\n'; ++i; continue; }
            out += ' ';
            ++i;
            continue;
        }
        if (two == "/*") { inBlock = true; out += "  "; i += 2; continue; }
        if (two == "//") { inLine = true; out += "  "; i += 2; continue; }
        out += c;
        ++i;
    }
    return out;
}

struct EmitScan {
    std::size_t filesScanned = 0;
    std::size_t totalRefs    = 0;
    // name -> first file that emits it (CONTENT, so a failure names the site)
    std::map<std::string, std::string> siteOf;
};

// One pass per file: find each `DiagnosticCode::`, skip whitespace, read the
// identifier. No regex — this runs over ~400 files in a Debug build.
EmitScan scanForEmitSites(fs::path const& srcRoot) {
    EmitScan scan;
    std::string const kQual = "DiagnosticCode::";
    for (auto const& entry : fs::recursive_directory_iterator(srcRoot)) {
        if (!entry.is_regular_file()) continue;
        auto const ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".cc") continue;
        if (isDeclarationFile(entry.path())) continue;
        ++scan.filesScanned;
        std::ifstream in{entry.path(), std::ios::binary};
        if (!in) continue;
        std::string const raw{std::istreambuf_iterator<char>{in},
                              std::istreambuf_iterator<char>{}};
        if (raw.find(kQual) == std::string::npos) continue;
        std::string const text = blankComments(raw);
        for (std::size_t pos = text.find(kQual); pos != std::string::npos;
             pos             = text.find(kQual, pos + 1)) {
            std::size_t j = pos + kQual.size();
            while (j < text.size()
                   && (text[j] == ' ' || text[j] == '\t' || text[j] == '\r'
                       || text[j] == '\n'))
                ++j;
            std::size_t e = j;
            while (e < text.size()
                   && (std::isalnum(static_cast<unsigned char>(text[e])) != 0
                       || text[e] == '_'))
                ++e;
            if (e == j) continue;
            ++scan.totalRefs;
            scan.siteOf.emplace(
                text.substr(j, e - j),
                fs::relative(entry.path(), srcRoot.parent_path()).string());
        }
    }
    return scan;
}

struct MarkerScan {
    std::size_t declLines = 0;
    std::set<std::string> retired;
};

// Parse `parse_diagnostic.hpp` for `<Name> = 0xNNN,` declaration lines and pick
// out those whose TRAILING comment carries the RETIRED marker. The marker lives
// at the declaration — "justified at the site" — not in a list a reader of the
// enum would never find.
MarkerScan scanForRetiredMarkers(fs::path const& hpp) {
    MarkerScan out;
    std::ifstream in{hpp};
    std::string line;
    while (std::getline(in, line)) {
        std::size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        std::size_t const nameStart = i;
        while (i < line.size()
               && (std::isalnum(static_cast<unsigned char>(line[i])) != 0
                   || line[i] == '_'))
            ++i;
        if (i == nameStart) continue;
        std::string const name = line.substr(nameStart, i - nameStart);
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size() || line[i] != '=') continue;
        ++i;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (line.compare(i, 2, "0x") != 0 && line.compare(i, 2, "0X") != 0) continue;
        i += 2;
        std::size_t const digits = i;
        while (i < line.size()
               && std::isxdigit(static_cast<unsigned char>(line[i])) != 0)
            ++i;
        if (i == digits) continue;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size() || line[i] != ',') continue;
        ++out.declLines;
        std::size_t const slashes = line.find("//", i);
        if (slashes != std::string::npos
            && line.find("RETIRED", slashes) != std::string::npos)
            out.retired.insert(name);
    }
    return out;
}

} // namespace

TEST(UnsuppressableCodes, EveryMemberHasAnEmitSiteOrIsMarkedRetired) {
    auto const root = dss::test::repoRoot();
    auto const src  = root / "src";
    ASSERT_TRUE(fs::is_directory(src)) << src.string();

    auto const scan  = scanForEmitSites(src);
    auto const marks = scanForRetiredMarkers(src / "core" / "types"
                                            / "parse_diagnostic.hpp");
    auto const codes = unsuppressableCodes();

    // ── PER-DIMENSION FLOORS ────────────────────────────────────────────────
    // Every dimension this property enumerates gets its own floor, because a
    // scan that collapses to zero on ONE axis otherwise reports a serene pass:
    // no files walked, no declaration lines parsed, no markers found, and no
    // references matched each look identical to "everything is fine". This repo
    // has shipped exactly that bug — a guard that passed while scanning
    // nothing. Floors sit below the 2026-08-10 measurements with headroom, so
    // ordinary growth never reds them; they catch a COLLAPSE, not a delta.
    EXPECT_GE(scan.filesScanned, 300u)          // MEASURED 393
        << "the src/ walk collapsed — an empty scan would pass every "
           "emit-site check below vacuously";
    EXPECT_GE(scan.totalRefs, 500u)             // MEASURED 735
        << "the DiagnosticCode:: matcher collapsed — files were opened but "
           "nothing matched, so no code could be proven emitted";
    EXPECT_GE(marks.declLines, 300u)            // MEASURED 345
        << "the parse_diagnostic.hpp enumerator parse collapsed — with no "
           "declaration lines nothing can be classified RETIRED, and "
           "directions B and C assert nothing";
    EXPECT_GE(marks.retired.size(), 5u)         // MEASURED 8
        << "the RETIRED-marker parse collapsed — direction C would then be "
           "vacuous and A's escape hatch unchecked";
    ASSERT_GE(codes.size(), 120u)               // MEASURED 141
        << "the closed table collapsed — nothing left to assert about";

    // ── CONTENT ANCHORS ─────────────────────────────────────────────────────
    // Content, not count: name codes on BOTH sides, so a matcher that silently
    // stops recognising real sites — or starts recognising prose — is caught by
    // identity rather than by an arithmetic total many wrong scans reproduce.
    EXPECT_TRUE(scan.siteOf.contains("S_VlaWithStaticStorage"))
        << "a known-LIVE emit site went unseen — the scanner is broken, not "
           "the table";
    EXPECT_TRUE(scan.siteOf.contains("K_SymbolUndefined"));
    EXPECT_TRUE(scan.siteOf.contains("S_EntryShapeNotDeclared"))
        << "this site is clang-format-wrapped after `DiagnosticCode::`; if it "
           "is missing, the whitespace skip regressed to a line-local match";
    EXPECT_TRUE(marks.retired.contains("S_VlaMultiDimUnsupported"))
        << "0xE052's RETIRED marker vanished from its declaration line";
    EXPECT_FALSE(scan.siteOf.contains("S_VlaMultiDimUnsupported"))
        << "0xE052 acquired an emit site — if a multi-dim VLA shape genuinely "
           "needs refusing again, un-retire the code deliberately (marker off, "
           "table row back, doc comment describing ONLY the refused shape)";

    // ★ THE `dependsOn` VERDICTS, STILL SELF-FIRING — NOW POINTED THE OTHER
    // WAY. This loop used to assert the four git codes had NO emit site, so
    // that the ARRIVAL of one would red and force the recorded verdicts to be
    // acted on. They arrived (AP6 lane D1, `program/dependency_cache.cpp`) and
    // the rows landed in the same change, so the trigger has fired and the
    // property worth holding is the PAIRING it produced: site AND row, both
    // present, for each of the four.
    //
    // ⚠ THIS IS NOT DIRECTION A RESTATED, and the difference is what makes it
    // worth the lines. Direction A quantifies over MEMBERS — it can only catch
    // a member that lost its site. Delete the four rows and direction A goes
    // silent, because a non-member with an emit site is an entirely ordinary
    // thing; delete the four SITES and `DependsOnGitCodesAreRefusedBySuppress-
    // AndKeepTheirText` still passes, because `isUnsuppressable` is a pure
    // table lookup. Only naming both halves together catches either half
    // leaving on its own.
    for (auto const* name : {"D_DependencyGitNotFound",
                             "D_DependencyGitAcquireFailed",
                             "D_DependencyGitFetchFallback",
                             "D_DependencyGitNameCollision"}) {
        EXPECT_TRUE(scan.siteOf.contains(name))
            << name
            << " LOST ITS EMIT SITE while remaining a closed-table member. "
               "That is the five-dead-members bug this whole property exists "
               "for: an unemittable code cannot be suppressed, so the row "
               "asserts nothing while making a dead code read as load-bearing. "
               "Either restore the site or retire the code deliberately "
               "(marker at the declaration, row removed, doc comment corrected)"
               ".";
        auto const code = std::string{name};
        bool       isMember = false;
        for (auto const& [c, why] : codes) {
            if (diagnosticCodeName(c) == code) { isMember = true; break; }
        }
        EXPECT_TRUE(isMember)
            << name
            << " has an emit site but is NO LONGER a closed-table member. The "
               "verdict block beside the D_Script* rows in "
               "`unsuppressable_codes.cpp` records why all four qualify — three "
               "on prong (2) and D_DependencyGitFetchFallback on prong (1), "
               "where suppression ships an artifact built from a revision the "
               "operator did not choose. Removing a row needs that block "
               "rewritten, not just this line deleted.";
    }

    // ── A: a live member must be emittable ──────────────────────────────────
    for (auto const& [c, why] : codes) {
        std::string const name{diagnosticCodeName(c)};
        if (marks.retired.contains(name)) continue;   // direction B reports it
        EXPECT_TRUE(scan.siteOf.contains(name))
            << "unsuppressable code " << name << " (0x" << std::hex
            << static_cast<unsigned>(c) << std::dec << ") has NO emit site in "
               "src/. A closed-table member that nothing emits asserts nothing, "
               "and its membership makes a dead code read as load-bearing. "
               "Either restore the emit site, or RETIRE it: mark the "
               "declaration in parse_diagnostic.hpp with a trailing RETIRED "
               "comment, correct the doc comment to describe only what is "
               "ACTUALLY refused, and drop this row (keep the enum value — "
               "never renumber).";
    }

    // ── B: a retired code must not be a member ──────────────────────────────
    for (auto const& [c, why] : codes) {
        std::string const name{diagnosticCodeName(c)};
        EXPECT_FALSE(marks.retired.contains(name))
            << name << " is marked RETIRED at its declaration yet is still a "
               "member of the closed unsuppressable table. An unemittable code "
               "cannot be suppressed, so the row asserts nothing while making "
               "the code read as load-bearing — remove the row and leave the "
               "rationale in its place (S_VolatilePointeeNotSupported 0xE025 "
               "precedent).";
    }

    // ── C: a retired marker must be TRUE ────────────────────────────────────
    // Without this, direction A is defeatable by stapling a RETIRED marker onto
    // a live code.
    for (auto const& name : marks.retired) {
        auto const it = scan.siteOf.find(name);
        EXPECT_EQ(it, scan.siteOf.end())
            << name << " is marked RETIRED but IS emitted, at "
            << (it == scan.siteOf.end() ? std::string{} : it->second)
            << ". A RETIRED marker is a claim about the code, not a way to "
               "quiet this test — remove the marker and restore the row if the "
               "code is live.";
    }
}

// P50 (D-CSUBSET-LINKAGE-INTERNAL-EXTERNAL-MISMATCH +
// D-CSUBSET-NORETURN-NON-FUNCTION-OBJECT): the two codes minted in one cycle
// took OPPOSITE suppression polarities, and the asymmetry is the measured
// union boundary, so pin BOTH directions (the #error/#warning pair
// precedent above):
//   - the linkage mismatch is the one static<->non-static ordering gcc,
//     clang AND MSVC all reject; suppressed, the merge keeps one record's
//     storage class and the artifact exports a symbol the program declared
//     internal (the S_ThreadLocalRedeclarationMismatch reason, one axis
//     over) -> MEMBER;
//   - the noreturn warning follows gcc's accept-and-ignore meaning; the
//     build proceeds identically with or without it, so it fails both
//     membership prongs and must stay suppressable (the
//     S_AsmLabelOnAutomaticVariable posture) -> NON-member.
TEST(UnsuppressableCodes, P50LinkagePairPinsBothPolarities) {
    EXPECT_TRUE(
        isUnsuppressable(DiagnosticCode::S_LinkageRedeclarationMismatch))
        << "silencing the C 6.2.2p7 mismatch ships a wrong-linkage artifact "
           "green (int g; static int g; keeps the plain tentative and "
           "EXPORTS a symbol the program declared internal)";
    EXPECT_FALSE(
        isUnsuppressable(DiagnosticCode::S_NoreturnNonFunctionObject))
        << "gcc's own meaning for _Noreturn on a non-function is warn-and-"
           "ignore; hiding the advice changes nothing in the artifact, so "
           "membership would loosen the table's one criterion";
}
