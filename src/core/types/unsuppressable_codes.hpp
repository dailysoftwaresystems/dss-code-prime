#pragma once

#include "core/export.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace dss {

// One row of the D-FF2-UNSUPP closed table: the protected code, and the
// reason it is protected — AS DATA, not as a comment beside it.
//
// ★ WHY THE RATIONALE IS A FIELD. This table has always required a written
// justification per entry, and always kept it in a comment. That was
// sufficient while the only reader was the next maintainer. It stopped being
// sufficient when `--suppress=<a protected code>` grew a diagnostic
// (`D_SuppressRequestIgnored`): the operator whose request is being refused
// is now a reader too, and a comment cannot be shown to them. Promoting the
// user-facing sentence into the row means the text they see IS the recorded
// justification — the two cannot drift, because there is only one of them.
// This repo's recurring failure is "a comment that records the full fact
// while the code uses half of it"; here the half the code needs is the half
// the user needs, so it stops being a comment.
//
// `why` reads as the tail of "cannot be suppressed: <why>". It is never
// empty — `kUnsuppressableEntriesAllExplainThemselves` in the .cpp is a
// consteval check, so a member added without a reason is a BUILD failure,
// not a blank line in somebody's terminal.
// ★★★ D-DIAG-UNSUPPRESSABLE-FAMILY-UNDECIDED — WHICH PRONG ADMITTED THIS CODE.
//
// The membership rule in `unsuppressable_codes.cpp` has always had two prongs.
// ✔MEASURED 2026-08-26 over the shipped table: all 83 distinct `why` sentences
// classify cleanly under it (75 prong (1), 7 prong (2), 1 genuinely both), so
// the rule is TOTAL over its own table and never needed replacing. But only
// **19 of 166** members were admitted under text that CITED a prong.
//
// ★ THAT GAP IS THE WHOLE DEFECT, AND IT IS NOT THE ONE THE ROW NAMED. The row
// was filed as "the family is UNDECIDED and needs one considered decision".
// The decision existed and was written down. It was simply never RUN against
// the members — and **a written rule that is never applied looks exactly like a
// missing rule**, because both produce a membership nobody can audit. Two
// consecutive cycles making opposite local calls (TF-C86 added a preprocessor
// code, TF-C87 declined to) was read at the time as evidence of a missing
// policy; it was really evidence of an unapplied one.
//
// ⇒ The prong is now a FIELD, not a comment. Same move, for the same reason,
// that `why` was promoted from comment to data one paragraph down: a comment
// can be omitted and nothing notices, whereas an aggregate member cannot be
// left out without the build saying so. Adding a member now forces the author
// to answer "under which prong?" before the file compiles.
enum class MembershipProng : std::uint8_t {
    // Prong (1) — WRONG ARTIFACT SHIPS GREEN. Suppressing it lets a
    // miscompile / wrong-bytes / undefined-extern artifact ship with a
    // successful-looking build. The majority of the table.
    WrongArtifactShipsGreen = 1,
    // Prong (2) — THE BUILD FAILS WITH NOTHING SAID. No wrong bytes ship (the
    // build still fails), but this diagnostic is the ONLY statement of why, so
    // suppressing it leaves a non-zero exit with an empty explanation.
    // ⚠ Narrow BY DESIGN, and the narrowness is what keeps it honest: it
    // requires that the failure STILL HAPPENS. A code whose suppression merely
    // hides advice while the build proceeds fails both prongs and must stay
    // suppressable.
    BuildFailsWithNothingSaid = 2,
    // Both prongs, on different branches of the same code's behaviour. Rare
    // and REAL — not a hedge for a verdict nobody wanted to make. The one
    // shipped instance is `H_ConflictingStringLiteralPrefixes`, whose two
    // outcomes genuinely differ: a mixed-prefix concatenation either fails the
    // build with nothing shown (prong 2) or is typed as a plain narrow array
    // (prong 1).
    Both = 3,
};

// One membership ARGUMENT: the prong it turns on, and the user-visible sentence
// that states it. ★ The unit is the ARGUMENT, not the member — ✔MEASURED, 166
// members share only 83 of these, because a band-wide invariant is one argument
// admitting fourteen codes. Keying the verdict here rather than on the entry
// means a new member reusing an existing reason inherits that reason's prong,
// which is correct: it is the same argument.
struct MembershipReason {
    MembershipProng  prong;
    std::string_view text;
};

struct UnsuppressableEntry {
    DiagnosticCode   code;
    MembershipReason reason;

    // `why`/`prong` stay readable as before. Accessors rather than fields so
    // the 166 entry initializers keep their two-element `{code, kWhyFoo}`
    // shape — the verdict lands on the 83 reasons without touching a single
    // entry line, which is what keeps this diff reviewable and keeps a
    // concurrent append from being eaten by a wholesale rewrite.
    [[nodiscard]] constexpr std::string_view why() const noexcept {
        return reason.text;
    }
    [[nodiscard]] constexpr MembershipProng prong() const noexcept {
        return reason.prong;
    }
};

// D-FF2-UNSUPP: closed-table of DiagnosticCodes whose emission MUST reach the
// reporter regardless of any `--suppress` policy. These codes' emission
// gates `ok` / `errorCount()` / exit-code semantics — suppressing them
// would silently re-open the failure surface they were introduced to
// close.
//
// Pre-fold: `--suppress=H_ExternHasInitializer` would silently drop the
// extern-init reject, the lowering's `ok` would stay false but the
// reporter's `errorCount()` would return 0 (the diagnostic never landed
// in `all_`), and any caller reading `errorCount()` to gate exit code
// would see green — the silent-drop the H_ExternHasInitializer fold was
// meant to permanently close would be re-opened with one CLI flag.
//
// Post-fold (eb2c6c7 refinement): `applyPolicy` consults
// `isUnsuppressable(d.code)` BEFORE the suppress check; unsuppressable
// codes bypass SILENCING mutations (`--suppress` drops + `overrides`
// demotion) but NOT severity ELEVATION (`--warnings-as-errors`) —
// elevation strengthens the signal rather than defeating the gate, so
// strict-mode operators get fail-loud exit codes on Warning-severity
// unsuppressable codes like F_BinaryReaderPartialCorruption.
// Post-fold #11 silent-failure F1: `report()` itself ALSO bypasses cap /
// dedup / maxPerCode / maxDiagnostics for unsuppressable codes — the
// four silent-drop gates around applyPolicy would otherwise re-open the
// surface even when policy correctly let the code through.
//
// ★ THAT BYPASS IS A CONSEQUENCE OF MEMBERSHIP, NOT A SERVICE MEMBERSHIP
// OFFERS (2026-08-13). The two properties a member gets — "the user cannot
// silence this" and "the reporter cannot drop this" — are different
// questions, and for a while this table was the only way to obtain the
// second. That made membership a side-channel: a code needing only
// delivery had to argue a suppression criterion it did not meet, and the
// criterion loosened every time one did. Delivery now has its own property
// on the diagnostic (`DiagnosticDelivery` in `parse_diagnostic.hpp`), which
// `DiagnosticReporter::report` consults through `mustDeliver` alongside
// `isUnsuppressable`. ⇒ A code that needs the cap to leave it alone takes
// `DiagnosticDelivery::Guaranteed` at its emit site and does NOT come here.
// Membership is decided on the suppression criterion in
// `unsuppressable_codes.cpp` and on nothing else.
//
// Membership tiers (informational — the closed-table at the .cpp is
// the single source of truth):
//   - Permanent architectural exclusions / wrong-machine-code
//     surfaces: D_TargetAbiModelUnsupportedByDriver,
//     D_TargetMachineCodeMismatch (D-LK6-8.2 SIGILL),
//     D_TargetAbiModelMismatch (D-LK6-8.2 SIGILL),
//     F_FfiIngestAbiModelUnsupported, F_FfiIngestEmptyCanonical,
//     H_ExternHasInitializer.
//   - Pending-plan announcement (suppressing misleads the user):
//     D_PlanNotLanded.
//   - `dependsOn` GIT ACQUISITION (4 codes, added 2026-08-14 with their
//     emit sites in `program/dependency_cache.cpp`):
//     D_DependencyGitNotFound, D_DependencyGitAcquireFailed,
//     D_DependencyGitNameCollision — each prong (2), the build stops
//     and this line is the only statement of why — plus
//     D_DependencyGitFetchFallback, the one prong (1) of the family:
//     the build PROCEEDS on sources it could not refresh, so
//     suppressing it ships an artifact from a revision the operator
//     did not choose, green.
//   - `dependsOn` GRAPH STRUCTURE (1 code, added 2026-08-14 with the
//     resolver's emit site in `program/dependency_resolver.cpp`):
//     D_DependencyCycle — prong (1), on a CODE-SPECIFIC argument
//     rather than the code-independent one that was rejected. The
//     behaviour its reject forbids is breaking the back edge and
//     proceeding, which makes the resolved dependency set depend on
//     where the walk started; the reject AND its statement are the
//     mechanism. ⚠ Its three siblings (0xD019 / 0xD01B / 0xD01C) are
//     deliberately NOT here — they take
//     `DiagnosticDelivery::Guaranteed` at their emit sites instead,
//     nothing distinguishing them from `D_FileNotFound` or
//     `D_ArtifactProfileFormatMismatch`, both non-members. The AP6
//     codes allocated after them (0xD022..0xD026) get no membership
//     verdict this cycle, per 0xD020's own re-read-against-the-landed-
//     site rule.
//   - Lowering / verifier structural invariants (cannot reach
//     codegen): H_UnsupportedLoweringForKind, H_ExternDeclMalformed,
//     H_VerifierFailure, H_TypeUnresolved + ALL 12 I_* MIR-verifier
//     codes (frozen-module structural / SSA / dominance invariants).
//   - Linker fail-loud (image refused / undefined extern / image-
//     write contract violation / format-walker invariants):
//     K_SymbolUndefined, K_ImageNotOk, K_ImageEmpty + 4
//     K_ImageWrite* codes (LK10 contract), plus
//     K_NoMatchingObjectFormat, K_FormatLacksImportSupport,
//     K_RelocationKindMismatch, K_WalkerInputContractViolation
//     (format-walker dispatch / extern / reloc invariants).
//   - LIR verifier / lowering structural invariants (17 L_* codes):
//     L_UnsupportedLoweringForOpcode (MIR→LIR coverage-gap fail-loud),
//     L_RequiredLirOpcodeMissing, L_VirtualRegInPostRegalloc,
//     L_MemOperandMalformed, L_PhysRegOrdinalOutOfRange,
//     L_InvalidSpillSlotSentinel, L_MoveCycleUnsupported,
//     L_IndirectCallUnsupported,
//     L_IndirectCalleeClobberedByArgSetup (FC4 c2 — the indirect-
//     callee/arg-setup collision backstop), L_StackPassedArgUnsupported,
//     L_CcRegLookupFailed, L_VlaDynamicAllocaUnsupported,
//     L_VlaNonLeafFrameUnsupported,
//     L_TerminatorSuccessorMismatch (D-LIR-TEXT-CONDBR-BLOCKREF-OPERANDS-DROPPED
//     — the terminator's recorded successors and its own BlockRef
//     operands disagree),
//     L_SideStructureIndexDangling, L_SideStructurePoolShrank,
//     L_SideStructureReferenceLost (D-LIR-PER-INST-REG-CONSTRAINTS — the
//     literal pool and the per-instruction register-constraint pool are
//     referenced BY INDEX from the instruction stream that four passes
//     rebuild; every way that carry can fail is silent).
//     ⚠ THIS ENUMERATION WAS STALE AND SAID "11" WHILE THE ARRAY HELD 13:
//     the two VLA codes were appended to `kUnsuppressableCodes` without
//     being named here, so the header under-reported the band by two for
//     two cycles. Counted from the array, not from this list.
//   - Regalloc Error-severity invariants (3 R_* codes; Info-
//     severity spillage codes intentionally OUT):
//     R_NoCallingConventions, R_CallingConventionLookupFailed,
//     R_VRegHasNoClass.
//   - Assembler / encoding bytes-on-disk invariants (5 A_* codes):
//     A_LirToMirSizeMismatch, A_NoMatchingEncodingVariant,
//     A_RoundTripMismatch, A_NoEncodingDeclared,
//     A_NoEncodingShapeWalker.
//   - AUTHORED ABORT (D-CPP-ERROR-WARNING) — the first and only P_*
//     member, and a DELIBERATE break in the D_/F_/H_/I_/K_/L_/R_/A_/S_
//     family pattern above, so it is spelled out rather than left to
//     look like a stray: P_PreprocessorErrorDirective. Every other
//     entry in this table is a MACHINE-detected invariant (the
//     compiler found something it must not ship). A reached `#error`
//     (C23 6.10.5) is the opposite direction — the SOURCE AUTHOR's own
//     abort, written precisely because the configuration being built
//     is one their code cannot correctly build. Suppressing it hides
//     no compiler opinion; it silently BUILDS the configuration the
//     header author declared invalid, and since the reject is the only
//     thing failing that build, it would do so GREEN. (It fires only
//     when conditional inclusion REACHES the directive — an `#error`
//     in a not-taken branch never emits, so membership costs nothing
//     on the skipped-guard shape that dominates real headers.) Its
//     twin P_PreprocessorWarningDirective (C23 6.10.6) is deliberately
//     NOT a member: translation continues, no wrong bytes ship, and
//     `--suppress` must stay able to silence exactly that advisory
//     class — the S_DeprecatedSymbolUsed / S_UnknownAttribute posture.
//
// Adding a code here is a commitment: this code's emission MUST be visible
// to the build pipeline regardless of any --suppress policy. New entries
// land paired with the fold that introduces the underlying invariant.
//
// Anchored sub-row D-FF2-UNSUPP-INFO-WAE-ASYMMETRY: the silencing-vs-
// elevation refinement gates Warning→Error via warningsAsErrors but
// the elevation arm does NOT promote Info → Warning/Error. If a future
// Info-severity unsuppressable producer lands, --warnings-as-errors
// strict mode would NOT fail-loud on its emission (Info diagnostics
// don't bump errorCount, and the elevation gate keys on severity ==
// Warning). Trigger: first Info-severity entry added to the closed-
// table. Resolution at that point: either extend the elevation gate
// to also promote Info, or harden `unsuppressable_codes.cpp` with a
// consteval check forbidding Info-severity members (requires
// augmenting `kUnsuppressableCodes` into a `{code, severity}` table
// so introspection is compile-time).
//
// ⛔ NO LONGER DORMANT — UPDATED 2026-08-12. The 2026-06-01 note here
// read "no Info-severity unsuppressable producer exists; the asymmetry
// is dormant". That is now FALSE in the way that matters:
// `D_DependencyGitFetchFallback` (0xD01F) is a REAL Info-severity
// producer whose ENTIRE PURPOSE is to NOT fail the build — a git
// dependency's fetch failed, a usable `.dss-deps/<name>` checkout is
// present and reused, and the build PROCEEDS on possibly-stale sources.
// That is the offline-build guarantee (a laptop on a train; CI with a
// flaky network); the Info line exists so "possibly stale" is never
// silent. See its allocation block in `parse_diagnostic.hpp`.
//
// ⚠ PRECISION, because it decides which resolution is still open: that
// code is NOT a member of `kUnsuppressableCodes`, so the trigger as
// literally worded ("first Info-severity entry added to the closed
// table") has not fired. But the RESOLUTION named above is what the
// consumer bites on, because the elevation arm is CODE-AGNOSTIC —
// `diagnostic_reporter.cpp` `applyPolicy` promotes every Warning to
// Error under `--warnings-as-errors` with no per-code exemption and no
// membership test at all.
//
// ⇒ ★ CLOSING THIS ANCHOR BY "EXTENDING THE ELEVATION GATE TO ALSO
//   PROMOTE INFO" WOULD SILENTLY REGRESS THE OFFLINE-BUILD GUARANTEE:
//   every project built with `--warnings-as-errors` would fail the
//   moment the network did, which is the exact outcome 0xD01F exists to
//   prevent.
//
// ★ NARROWED 2026-08-13 — TWO OF THE THREE CANDIDATE RESOLUTIONS ARE NOW
//   REFUTED, AND WHAT REMAINS IS SMALLER THAN THE ANCHOR IMPLIES.
//   · "Extend elevation to promote Info" — refuted above, unchanged.
//   · "Harden with a consteval check FORBIDDING Info-severity members" —
//     also refuted, and by the same consumer. 0xD01F was judged against
//     this table's suppression criterion on 2026-08-13 and QUALIFIES on
//     prong (1): suppressed, a build that silently reused stale sources
//     after a failed fetch reports SUCCESS, so the artifact ships green
//     from a revision the operator did not choose. A rule forbidding Info
//     members would therefore forbid a member that qualifies — it would
//     resolve the anchor by outlawing its own use case.
//   · What is left is the observation that dissolves most of it: for
//     0xD01F the non-elevation of Info is not a gap, it is THE REQUIRED
//     BEHAVIOUR. The anchor's premise — "strict mode would NOT fail-loud
//     on its emission" — silently assumes fail-loud is wanted; for the
//     offline-build guarantee it is precisely what must not happen. So
//     Info-severity membership is admissible, and non-elevation is its
//     correct semantics rather than an asymmetry to repair.
//   ⇒ RESIDUAL QUESTION, and it is the only one still open: whether a
//     FUTURE Info member could arrive that DOES want strict-mode
//     fail-loud, which would need per-code elevation opt-in rather than
//     the code-agnostic arm. No such producer exists, and inventing the
//     mechanism before one does would ship an untested arm.
//   ✅ UNBLOCKED AND LANDED 2026-08-14 (AP6 lane D1). The blocker recorded
//     here was: "0xD01F has NO EMIT SITE yet (MEASURED 2026-08-13), so it
//     cannot join this table at all". It has one now —
//     `program/dependency_cache.cpp`'s fetch-failed-with-usable-checkout
//     arm — and the row is in the closed table, so the trigger as worded
//     ("first Info-severity entry added to the closed table") HAS FIRED.
//     What fired with it is the resolution already argued above, not the
//     one the original anchor proposed: for this member, Info's
//     non-elevation under `--warnings-as-errors` is THE REQUIRED
//     BEHAVIOUR, so there is nothing to repair. Pinned three-sided in
//     `tests/program/test_dependency_git_cache.cpp` — under
//     `--warnings-as-errors` the code appears exactly once, at Info, with
//     `errorCount() == 0` — because "no promotion happened" is also what
//     zero diagnostics looks like.
//   ⇒ WHAT REMAINS OPEN IS ONLY THE RESIDUAL NAMED ABOVE: a FUTURE Info
//     member that DOES want strict-mode fail-loud would need per-code
//     elevation opt-in rather than the code-agnostic arm. No such producer
//     exists; inventing the mechanism before one does would ship an
//     untested arm. That is a trigger, not a gap.
//   ⓘ Note what is NO LONGER part of this question: 0xD01F also needs the
//     reporter's cap to leave it alone, and that used to be a second
//     reason to want membership. It is now a separate, independently
//     grantable property (`DiagnosticDelivery::Guaranteed`), so the
//     delivery need no longer applies any pressure to this decision.
[[nodiscard]] DSS_EXPORT bool
isUnsuppressable(DiagnosticCode code) noexcept;

// Public view of the closed-table for introspection (tests, --help,
// future diagnostic-policy validation at CLI parse time).
[[nodiscard]] DSS_EXPORT std::span<UnsuppressableEntry const>
unsuppressableCodes() noexcept;

// The recorded reason `code` is protected, or an EMPTY view when `code` is
// not a member. The empty return is the only correct answer for a non-member
// — there is no reason, because there is no protection — and callers must
// treat it as such rather than rendering a blank explanation:
// `D_SuppressRequestIgnored` is only ever emitted for a code that
// `isUnsuppressable` already answered true for, so the empty arm is
// unreachable there by construction.
[[nodiscard]] DSS_EXPORT std::string_view
unsuppressableRationale(DiagnosticCode code) noexcept;

// Which prong admitted `code`, or `nullopt` when `code` is not a member.
// ⚠ The optional is load-bearing — see the definition. A defaulted prong would
// make "admitted under prong (1)" and "never in the table" render identically,
// which is the plausible-wrong-answer defect this whole cluster is about.
[[nodiscard]] DSS_EXPORT std::optional<MembershipProng>
membershipProngOf(DiagnosticCode code) noexcept;

} // namespace dss
