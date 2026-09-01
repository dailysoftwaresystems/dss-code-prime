#include "core/types/unsuppressable_codes.hpp"

#include <algorithm>
#include <array>

namespace dss {

namespace {

// D-FF2-UNSUPP closed-table. Sorted by phase letter (D / F / H / I / K
// / L / R / A / S / P) + numeric value within each phase for at-a-glance
// audit. The linear scan via `std::ranges::find` is O(N) over the
// table — still faster than hash lookup at this size + needs no
// static-init dance.
//
// ★ MEMBERSHIP RULE — AND IT IS ABOUT SUPPRESSION, NOTHING ELSE.
// This table answers exactly one question: may `--suppress=<code>` silence
// this? It does NOT answer "may the reporter's cap drop this" — that is the
// DELIVERY question, it now has its own property (`DiagnosticDelivery` on
// `ParseDiagnostic`), and a code that needs only delivery must take the
// property and stay OUT of here. Joining this table to obtain cap-immunity
// uses membership as a side-channel for something it was never defined to
// carry, and pays for it by loosening the one criterion the table has.
// (Membership does still bypass the cap — that is a consequence of what a
// member IS, not a service it offers.)
//
// A code is a member when suppressing it would produce EITHER of two
// outcomes. Both prongs are stated because both are ALREADY APPLIED here,
// and a rule that describes only half of its own table is a rule the next
// reader will mis-apply:
//
//   (1) WRONG ARTIFACT SHIPS GREEN. Its surface is a load-bearing
//       structural invariant whose silent re-opening would let a
//       miscompile / wrong-bytes / undefined-extern artifact ship with a
//       successful-looking build. This is the majority of the table.
//
//   (2) THE BUILD FAILS WITH NOTHING SAID. The build still fails without
//       the diagnostic — so no wrong bytes ship — but the diagnostic is
//       the ONLY statement of why, and suppressing it leaves a non-zero
//       exit with an empty explanation. `--suppress` is documented as a
//       VISIBILITY control; under prong (2) it would become a mute-the-
//       failure control instead. Members admitted on this ground SAY SO in
//       their own blocks: S_TypeNameDeclaratorNotAbstract ("suppressing
//       this ships NO wrong bytes … closed here so the failure is never
//       SILENT"), S_StaticAssertFailed, S_GenericSelection{NoMatch,
//       Ambiguous}, the five S_Alignas*, H_ConflictingStringLiteral-
//       Prefixes.
//
// Prong (2) is narrow, and its narrowness is what keeps it honest: it
// requires that the failure STILL HAPPENS and that this diagnostic is the
// only thing explaining it. A code whose suppression merely hides advice
// while the build proceeds fails both prongs and must stay suppressable —
// see P_PreprocessorWarningDirective, S_UnknownAttribute,
// S_DeprecatedSymbolUsed, S_NodiscardResultDiscarded,
// S_AsmLabelOnAutomaticVariable below, each pinned as a NEGATIVE.
//
// Examples in shipped
// closed-table: D-LK6-8.2 split codes (silent ABI mismatch ⇒
// SIGILL at user runtime), I_* verifier invariants (SSA / CFG
// violations sailing through), K_ImageWrite* (silently truncated
// on-disk image), F_FfiIngest* architectural exclusions (silent
// wrong-shape FfiMetadata for the wrong abiModel). The table grows
// as new architectural surfaces close and SHRINKS when one is
// retired (see the 144 → 139 → 141 notes below — it does NOT grow
// monotonically); each addition includes a one-line rationale
// block alongside the entry, and each removal leaves that block
// rewritten in place rather than deleted.
// 141 → 144: the program-entry resolution family replaced one member
// (`K_EntryShapeNotDeclared`) with four (`S_EntryShapeNotDeclared`,
// `K_ProgramEntryUndefined`, `K_ProgramEntryAmbiguous`,
// `K_EntryVerbUnmaterializable`) — the one gate split into the four distinct ways
// a build can end up running the wrong entry, or none, while reporting success.
//
// ★ 144 → 139 (2026-08-10): the table does NOT grow monotonically after all — a
// RETIRED code must LEAVE it. Five members had outlived their emit sites
// (`F_FfiNoImportLibraryForFormat`, `F_FfiResolveLibrarySymbolAbsent`,
// `S_BitIntWidthAboveC1Limit`, `S_BitIntWideMulDivUnsupported`,
// `S_VlaMultiDimUnsupported`), two of them still carrying doc comments that
// described the retired surface as live — and one of those stale comments had
// already produced a wrong C23 conformance claim. Membership was the thing that
// made them read as load-bearing to a reader, because a closed table of
// silent-miscompile guards is exactly where a name looks load-bearing. An
// unemittable code cannot be suppressed, so those rows asserted NOTHING.
// `EveryMemberHasAnEmitSiteOrIsMarkedRetired` in
// `tests/core/test_unsuppressable_codes.cpp` now enforces the property that
// found them, so the next retirement cannot leave its row behind silently.
//
// 139 → 141 (inline-asm P1, D-LANG-GNU-EXTENDED-INLINE-ASM-UNSUPPORTED): TWO of
// that arc's three new codes join — `S_InlineAsmExtendedUnsupported` and
// `S_InlineAsmLabelSectionRequiresGoto`. The THIRD,
// `S_InlineAsmDuplicateQualifier`, deliberately does NOT (see the ⓘ note at its
// siblings): it is an Error by default but its suppression ships no wrong bytes,
// which is this table's whole membership criterion. Two out of three is the
// evidence the criterion was applied rather than the codes swept in as a batch.
//
// 155 → 157 (the shipped-surface backing gate): `C_UnbackedPredefinedMacro`
// (D-LANG-PREDEFINED-MACRO-REQUIRES-REALIZED-SURFACE) and
// `F_ShippedCorpusInvariantBroken` (D-FFI-DESCRIPTOR-INCLUDES-EDGE-GATE) join.
// Both meet the membership criterion the same way: each exists precisely to stop
// a CLAIM that is false about the platform from reaching the user as a
// far-away, unrelated-looking error — so a suppressible form does not merely
// hide a message, it restores the distance between the false claim and its
// consequence that the check was built to remove. ⚠ THE 155 IS MEASURED AT THE
// ARRAY (the running totals above are known to disagree with each other), and
// the new count is likewise the array's, not this comment's arithmetic.
//
// 148 → 155 (inline-asm P5 wave 2, D-CSUBSET-INLINE-ASM-OPERANDS): the seven
// operand-binding codes 0xE065..0xE06B join, with the P1 verdict on
// `S_InlineAsmDuplicateQualifier` left standing — see the block beside the
// rows themselves. ⚠ THE 148 IS MEASURED AT THE ARRAY, NOT READ OFF THE
// RUNNING TOTALS ABOVE, and it had to be: those totals contradict each other
// (a "141 → 144" and a "★ 139 → 141" both claim to be steps of the same
// sequence, and one step was never recorded at all, as the note below admits).
// A count in this file is only ever trustworthy if it was counted from
// `kUnsuppressableCodes` at the commit that carries it.
//
// 142 → 143 (D-LIR-TEXT-CONDBR-BLOCKREF-OPERANDS-DROPPED):
// `L_TerminatorSuccessorMismatch` joins — a terminator whose recorded successors
// and whose own BlockRef operands disagree. Suppressed, the encoder takes its
// branch displacement from an operand list the CFG no longer agrees with: a
// branch to the wrong block, or no branch target at all, with a green build.
// ⓘ The step before this one was 141 → 142 and went unrecorded here, which is
// why the running total above stops at 141 while the array held 142.
// ★ 139 → 141 (2026-08-12, AP5): the two build-lifecycle hook codes joined.
// The AP5 cycle wrote the argument for membership at `program.cpp`'s
// `D_PlanNotLanded` reject — "`--suppress` must not be able to convert this
// loud reject back into the silent no-op it exists to replace" — and then did
// not apply it to the hook codes, for which the same sentence is true
// verbatim: `parse_diagnostic.hpp` describes `D_ScriptExitedNonZero`'s purpose
// as keeping "precisely the silent-success class" out of the driver.
//
// ⚠ THAT ENTRY ORIGINALLY GAVE TWO REASONS AND ONE OF THEM WAS WRONG. It
// listed (a) the `--suppress` route and (b) "NO FLAGS AT ALL — the global cap
// latches on a warning-heavy compile and eats the hook's diagnostic", and
// called membership the fix for both. (b) is a CAP argument, and answering it
// with membership is precisely the side-channel the rule above now forbids:
// it made the cap's behaviour a reason to weaken a suppression table. (b) is
// REAL — it is measured, and it is why `DiagnosticDelivery` exists — but it is
// not this table's business, and a code whose ONLY problem was (b) must take
// the property instead of a row here.
//
// ✔RE-EXAMINED 2026-08-13 UNDER THE SUPPRESSION CRITERION ALONE — BOTH STILL
// QUALIFY, ON PRONG (2), AND THE MEASUREMENT IS THE SAME ONE: `runBuildScripts`
// (`program/build_scripts.cpp`) returns false whether or not its report
// survived, and the driver turns that into `return 1`. So suppression does NOT
// ship a wrong artifact — the build still fails, which is why prong (1) does
// not apply — but it makes the build fail MUTELY: `--suppress=D_ScriptExited-
// NonZero`, which `cli_args.cpp` accepts for any real code name, turns a
// documented VISIBILITY control into a silence-the-reason control it cannot
// even fully exercise. And the hook case is a PURER prong-(2) instance than
// the S_* members that established the prong: a pre-build hook fails before
// any compilation has begun, so there is no other diagnostic in the stream to
// partially explain the exit — stderr is empty, exactly.
// ── PER-ENTRY RATIONALE, AS DATA ─────────────────────────────────────────
// The user-facing half of every prose block below, promoted OUT of the
// comment and INTO the row — because a rationale that exists only in a
// comment cannot be shown to the operator whose `--suppress` this table
// refuses. `D_SuppressRequestIgnored` renders this text verbatim, so what
// the user reads IS the justification recorded beside the entry, not a
// second copy of it composed in a message string.
//
// ★ THE GRANULARITY IS THE ONE THE TABLE ALREADY HAS, AND THAT IS MEASURED,
// NOT ASSUMED: the 141 members carry 62 distinct rationale blocks (36
// singletons; the rest group-level, up to 14 codes under one argument).
// Where N codes share ONE argument they share ONE constant — splitting it
// into N near-copies would manufacture exactly the drift this promotion
// exists to remove. A code needing a different reason takes a new constant;
// a member added without one does not compile past
// `kUnsuppressableEntriesAllExplainThemselves` below.
//
// ⚠ AND THE PROSE COULD NOT HAVE BEEN LIFTED MECHANICALLY. ✔MEASURED while
// writing these: FIVE blocks OPEN with de-listing notes about RETIRED
// NON-members (`F_FfiNoImportLibraryForFormat` /
// `F_FfiResolveLibrarySymbolAbsent` above `F_BinaryReaderPartialCorruption`;
// `S_VolatilePointeeNotSupported` above `S_IncompleteTypeMember`; the two
// retired `S_BitInt*` above `S_BitIntWideFloatConvUnsupported`; the
// `S_UnknownAttribute` NEGATIVE pins above `P_PreprocessorErrorDirective`;
// the `dependsOn` git verdicts above the `D_*` driver band) — so "take the
// comment above the entry" would have shown a user suppressing a LIVE code
// the reasoning for a DEAD one, which is a fresh instance of the very class
// this promotion exists to close, delivered straight to the operator. Every
// string below was written by reading its own block.
//
// Style: reads as the tail of "cannot be suppressed: <text>". Present
// tense, no anchor ids, no dates, no cross-references — those stay in the
// prose, which remains the internal record this text is drawn from.
constexpr MembershipReason kWhyBuildHook{
    MembershipProng::BuildFailsWithNothingSaid,
    "a failed build hook already aborts the build; silenced, it aborts "
    "with nothing on stderr saying which hook failed or why"};
constexpr MembershipReason kWhyGitNotFound{
    MembershipProng::BuildFailsWithNothingSaid,
    "nothing can be acquired without git, so the build stops; silenced, it "
    "stops with nothing naming the missing tool"};
constexpr MembershipReason kWhyGitAcquireFailed{
    MembershipProng::BuildFailsWithNothingSaid,
    "a git dependency could not be fetched and no checkout exists to build "
    "from; silenced, the build stops without naming the dependency that is "
    "missing"};
constexpr MembershipReason kWhyGitFetchFallback{
    MembershipProng::WrongArtifactShipsGreen,
    "the build deliberately continued on a checkout it could not refresh; "
    "silenced, an artifact compiled from a revision the operator did not "
    "choose ships green, with the only line that said so removed"};
constexpr MembershipReason kWhyGitNameCollision{
    MembershipProng::BuildFailsWithNothingSaid,
    "two git dependencies want one cache directory and the build stops "
    "before either is fetched; silenced, it stops with nothing naming the "
    "two entries that collide"};
constexpr MembershipReason kWhyDependencyCycle{
    MembershipProng::WrongArtifactShipsGreen,
    "the dependency graph has a ring, and the reject is what stops the walk "
    "from breaking it; silenced, the build resolves whichever half the walk "
    "reached first and ships an artifact missing the rest"};
constexpr MembershipReason kWhyPlanNotLanded{
    MembershipProng::BuildFailsWithNothingSaid,
    "it announces a mode whose engine has not landed; silenced, the run "
    "fails with no statement that the feature does not exist yet"};
constexpr MembershipReason kWhyTargetAbi{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a mismatched target spec dispatches the wrong backend and "
    "the emitted image executes wrong machine code at user runtime"};
constexpr MembershipReason kWhyLanguageTargetIsa{
    MembershipProng::BuildFailsWithNothingSaid,
    "it is the SOLE statement of why the build stopped; silenced, the run "
    "exits non-zero having printed nothing at all"};
constexpr MembershipReason kWhySynthRecipe{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, the shim recipe falls out of every synthesis pass and its "
    "symbol goes undefined, breaking the binary LOAD rather than the "
    "build"};
constexpr MembershipReason kWhyFfiIngest{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, FFI ingest builds wrong-shape metadata for an abiModel it "
    "does not support, or shadows every row under an empty canonical name"};
constexpr MembershipReason kWhyBinaryReaderPartial{
    MembershipProng::WrongArtifactShipsGreen,
    "translation continues on the salvaged part of an input the reader "
    "could not fully decode; silenced, that build ships green with "
    "nothing saying so"};
constexpr MembershipReason kWhyShippedHeaderNotFound{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a program that calls an undeclared shipped-library symbol "
    "compiles clean"};
constexpr MembershipReason kWhyShippedLibDescriptor{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, the lowering synthesizes no externs and a program whose "
    "system-header symbols resolve to nothing compiles clean"};
constexpr MembershipReason kWhyShippedHeaderTarget{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, the header symbols, structs and typedefs are injected on a "
    "platform its own descriptor says does not have them"};
constexpr MembershipReason kWhyShippedSymbolTarget{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a symbol unknown to this object format binds to the "
    "format-default library, links clean, and dies at LOAD"};
constexpr MembershipReason kWhyHeaderNameCase{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, the resolver picks one of several case-folded matches, and "
    "which one it picks differs by build host"};
constexpr MembershipReason kWhyUnbackedPredefine{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, an identity macro keeps promising a platform surface that "
    "was never built, and the failure surfaces inside the user's own "
    "#ifdef branch instead of at the false claim"};
constexpr MembershipReason kWhyShippedCorpusInvariant{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, an include edge promises a header the corpus declares "
    "absent, or a header declares nothing at all, and the #include "
    "compiles while its contents silently are not there"};
constexpr MembershipReason kWhyShippedStructVariant{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, an under-specified per-target variant picks the first "
    "match, giving a wrong struct layout for the active target"};
constexpr MembershipReason kWhyShippedVariant{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, an under-specified per-target variant picks the first "
    "match, giving a wrong constant value, typedef width or macro "
    "replacement"};
constexpr MembershipReason kWhyTypeIdentityConflict{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, two descriptors declaring one tag as different types "
    "resolve first-wins by name, giving include-order-dependent member "
    "access"};
constexpr MembershipReason kWhyHirStructural{
    MembershipProng::WrongArtifactShipsGreen,
    "a HIR lowering / verifier structural invariant; silenced, the module "
    "reaches codegen violating a contract every downstream tier assumes"};
constexpr MembershipReason kWhyWideLiteral{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a code point that does not fit the requested element width "
    "ships as a wrong or truncated code unit"};
constexpr MembershipReason kWhyConflictingStringPrefixes{
    MembershipProng::Both,
    "silenced, a mixed-prefix literal concatenation fails the build with "
    "nothing shown, or is typed as a plain narrow array"};
constexpr MembershipReason kWhyShimSignature{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, the build goes green and emits a call to a synthesized "
    "shim under a different ABI than the caller made it"};
constexpr MembershipReason kWhyMirVerifier{
    MembershipProng::WrongArtifactShipsGreen,
    "a frozen-module MIR-verifier invariant; silenced, an SSA, CFG, "
    "dominance or type violation sails past the verifier into codegen"};
constexpr MembershipReason kWhyVlaAllocaOperand{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a runtime-sized alloca with a malformed operand shape "
    "ships a mis-sized stack slot"};
constexpr MembershipReason kWhyAtomicNotLowered{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a plain load or store of _Atomic memory performs a "
    "NON-atomic access, the exact miscompile _Atomic exists to prevent"};
constexpr MembershipReason kWhyCallSignature{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a call whose operands do not match its resolved callee "
    "signature ships green with the arguments in the wrong slots"};
constexpr MembershipReason kWhyStoreValueType{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a store whose value type is not its slot type ships the "
    "wrong BYTES to memory with no other build-time symptom"};
constexpr MembershipReason kWhyLinkerImage{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, errorCount() reads zero while the image on disk is "
    "refused, empty, missing or truncated"};
constexpr MembershipReason kWhyNoMatchingObjectFormat{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, the linker dispatches the wrong format walker and writes a "
    "corrupted artifact"};
constexpr MembershipReason kWhyFormatLacksImportSupport{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, an extern goes unresolved in a dynamic image whose format "
    "cannot carry imports at all"};
constexpr MembershipReason kWhyRelocationKindMismatch{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a relocation kind the format does not support is applied "
    "anyway, putting wrong bytes in the image"};
constexpr MembershipReason kWhyWalkerInputContract{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, malformed linker-driver input propagates through the "
    "format walker undetected"};
constexpr MembershipReason kWhyEntryResolvesToExtern{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, the loader jumps to unrelocated import-stub bytes at "
    "process entry, a fault with no diagnostic trail"};
constexpr MembershipReason kWhyAssembledData{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a producer ships two data items at one symbol "
    "(last-write-wins) or a bss item carrying bytes that silently drop"};
constexpr MembershipReason kWhyStackReserve{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, the build reports success while emitting an image whose "
    "stack is not the size the program asked for"};
constexpr MembershipReason kWhyEntryTrampoline{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, the image entry points at the wrong code and the program "
    "runs the wrong entry, or falls off it, from a green build"};
constexpr MembershipReason kWhyProgramEntry{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, entry resolution proceeds on a wrong, absent or ambiguous "
    "candidate and the binary faults at startup from a green build"};
constexpr MembershipReason kWhyExternImportConflict{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, two CUs declaring one external name under different "
    "binding models merge first-wins and half the calls bind wrong"};
constexpr MembershipReason kWhyLirStructural{
    MembershipProng::WrongArtifactShipsGreen,
    "a LIR verifier / lowering structural invariant; silenced, the "
    "violation reaches the assembler and miscompiles through the LIR "
    "layer"};
constexpr MembershipReason kWhySideStructureIntegrity{
    MembershipProng::WrongArtifactShipsGreen,
    "a module side structure (literal pool / per-instruction register-"
    "constraint pool) is referenced by index from the instruction stream; "
    "silenced, a rebuild that loses the reference miscompiles silently"};
constexpr MembershipReason kWhyVlaDynamicAlloca{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a runtime-sized alloca falls to the fixed-slot path and "
    "emits a one-slot scalar for the whole VLA"};
constexpr MembershipReason kWhyTerminatorSuccessors{
    MembershipProng::WrongArtifactShipsGreen,
    "the terminator's recorded successors and its own BlockRef operands "
    "disagree; silenced, the encoder takes a branch displacement from an "
    "operand list the CFG no longer matches -- a branch to the wrong block, "
    "or none at all"};
constexpr MembershipReason kWhyAsmTextUnsupported{
    MembershipProng::WrongArtifactShipsGreen,
    "an unrecognized `.s` construct; silenced, the assembler emits a binary "
    "that omits an instruction the programmer wrote"};
constexpr MembershipReason kWhyInlineAsmExtended{
    MembershipProng::WrongArtifactShipsGreen,
    "a GNU extended inline-asm statement lowers to a 0-child barrier that "
    "discards its operand list; silenced, the asm executes and clobbers "
    "registers the allocator still believes it owns, while the declared "
    "outputs keep their prior values"};
constexpr MembershipReason kWhyInlineAsmLabelSection{
    MembershipProng::WrongArtifactShipsGreen,
    "a label section without the `goto` qualifier; silenced, the statement "
    "reaches the extended-asm gate mis-sectioned, so which colon delimited "
    "which operand role becomes a guess"};
constexpr MembershipReason kWhyVlaNonLeafFrame{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a non-leaf VLA frame places outgoing call arguments inside "
    "the VLA region, an ABI break"};
constexpr MembershipReason kWhyRegallocInvariant{
    MembershipProng::WrongArtifactShipsGreen,
    "a register-allocation calling-convention / class invariant; "
    "silenced, allocation proceeds with no convention or class to honour"};
constexpr MembershipReason kWhyEncodingBytes{
    MembershipProng::WrongArtifactShipsGreen,
    "an assembler bytes-on-disk invariant; silenced, the encoder emits "
    "wrong machine code"};
constexpr MembershipReason kWhyImmediateRange{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a too-wide immediate is truncated into a wrong "
    "machine-code constant, a wrong syscall number for instance"};
constexpr MembershipReason kWhyImmediateNarrowed{
    MembershipProng::WrongArtifactShipsGreen,
    "the one statement that a shipped instruction carries a DIFFERENT "
    "constant than the one written; silenced, the narrowing becomes "
    "indistinguishable from the reference assembler's own silence, which is "
    "the arm the operator's ruling exists to reject"};
constexpr MembershipReason kWhyIncompleteType{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, an object or member of an incomplete composite folds its "
    "size to zero, a wrong-bytes layout"};
constexpr MembershipReason kWhySilentConstraint{
    MembershipProng::BuildFailsWithNothingSaid,
    "the build already fails on this constraint violation; silenced, it "
    "fails with ZERO diagnostics shown and no statement of why"};
constexpr MembershipReason kWhyPackedBitfield{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a packed struct carrying a bit-field is laid out padded "
    "instead, the wrong ABI"};
constexpr MembershipReason kWhyNullptrOperand{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, nullptr lowers through the integer-0 null constant and "
    "`nullptr + 1` compiles as `0 + 1`"};
constexpr MembershipReason kWhyEnumUnderlying{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, the enum is laid out at the default width instead, or an "
    "out-of-range enumerator wraps into a wrong constant"};
constexpr MembershipReason kWhyTypeofBitfield{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, the typeof resolves to the bit-field declared (widened) "
    "type, a wrong type in the declaration it specifies"};
constexpr MembershipReason kWhyConstexpr{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, constexpr degrades to plain const and an object that "
    "cannot deliver a translation-time value compiles quietly"};
constexpr MembershipReason kWhyAutoInference{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, the declaration adopts its initializer type and compiles "
    "the very form the constraint forbids"};
constexpr MembershipReason kWhyThreadLocal{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, thread storage lowers wrong: a per-call automatic, a split "
    "binding, or a link-time tpoff bit-cast into a data slot"};
constexpr MembershipReason kWhyBitIntWidth{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, the _BitInt(N) type has no computable width and masking "
    "and layout pick a garbage N"};
constexpr MembershipReason kWhyBitIntFloatConv{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, wide _BitInt float conversion takes the naive scalar path, "
    "emitting the wrong sign and dropping the upper limbs"};
constexpr MembershipReason kWhyVlaStorage{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a runtime-sized array is carried into the static-local "
    "lowering, whose layout has no static size"};
constexpr MembershipReason kWhyVlaSize{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a non-integer VLA bound truncates to a garbage element "
    "count, or a null bound gives a silent 0-byte array"};
constexpr MembershipReason kWhyArrayParamQualifier{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, the illegal array-declarator decoration is dropped and a "
    "mis-typed or mis-sized object ships"};
constexpr MembershipReason kWhyInlineAsmTemplate{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a non-empty asm template lowers to a no-op barrier and its "
    "instructions simply vanish"};
// ── inline-asm P5 operand binding ─────────────────────────────────────────
// SEVEN reasons rather than one shared string, because the second candidate
// lowering differs per code and the reason is what the operator is SHOWN when
// their --suppress is refused. A shared "inline asm cannot be suppressed"
// would tell them which table refused them and nothing about what would have
// happened.
constexpr MembershipReason kWhyAsmConstraintLetter{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, an operand whose constraint letter this target does not "
    "declare binds to nothing, and the asm runs on whatever the allocator "
    "left in place"};
constexpr MembershipReason kWhyAsmConstraintForm{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a multi-alternative or multi-letter constraint makes the "
    "binder pick one alternative on its own, unannounced"};
// ⚠ THE WIDTH-VIEW MODIFIER ITSELF IS NO LONGER WHAT THIS CODE REFUSES (P30 —
// the dialects declare `assembly.templateModifiers` and `%w0` lowers), so the
// reason names the PROPERTY rather than the retired spelling: what is left is
// every other `%`-form this build cannot expand, and silencing any of them
// emits the operand at the full register width.
constexpr MembershipReason kWhyAsmOperandModifier{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a % form this build cannot expand is emitted with its operand "
    "at the full register width, so an operation the template asked to be "
    "narrow executes wide"};
constexpr MembershipReason kWhyAsmClobberUnknown{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, the clobber is dropped, a live value stays parked in a "
    "register the asm overwrites, and the wrong value is read after it"};
constexpr MembershipReason kWhyAsmTemplateUnparsable{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a template the dialect could not parse lowers to nothing and "
    "the instructions the programmer wrote vanish"};
constexpr MembershipReason kWhyAsmPlaceholderRange{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a placeholder past the last declared operand is dropped or "
    "emitted raw, and the instruction operates on the wrong thing"};
constexpr MembershipReason kWhyAsmPlaceholderInBasic{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a % form in a template with no operand sections is either "
    "bound against an operand list that does not exist or passed through "
    "raw, and the two ship different machine code"};
constexpr MembershipReason kWhyAsmDuplicateSymbolicName{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, one of the two bindings the repeated name introduces is "
    "discarded by a first-match lookup and the template reads the other"};
constexpr MembershipReason kWhyBitfieldMutation{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, the mutation falls to a full-unit store that clobbers "
    "packed neighbours and skips truncation"};
constexpr MembershipReason kWhyErrorDirective{
    MembershipProng::WrongArtifactShipsGreen,
    "an #error the author wrote to stop exactly this configuration; "
    "silenced, the build they declared invalid is built, and reports "
    "success"};
constexpr MembershipReason kWhyPragmaUnhonored{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a pragma that changes memory layout is ignored without a "
    "word and composites are laid out at the wrong size"};
constexpr MembershipReason kWhyPragmaPackAmbiguous{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a composite with two candidate layouts of different size "
    "and offsets simply gets one of them, unannounced"};
constexpr MembershipReason kWhyAsmLabel{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, the intended symbol name is not restored: a C-mangled or "
    "synthetic name ships and the build stays green all the way to link"};
constexpr MembershipReason kWhyOperatorNameNotDefinable{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, __has_include(<h>) answers 0 while #include <h> still "
    "splices the header, so the guard and the include disagree"};

// ★★★ THE TWO CODES D-DIAG-UNSUPPRESSABLE-FAMILY-UNDECIDED WAS FILED ABOUT.
//
// TF-C87 minted `P_PreprocessorIncludeReentryRefused` and deliberately did NOT
// add it here, on the ground that its sibling was not a member either and that
// the whole family deserved one considered decision rather than a drive-by.
// That was the right instinct about PROCESS and a wrong belief about the FACTS:
// ✔MEASURED 2026-08-26, the two-prong rule at the top of this file answers both
// codes cleanly, and had been able to the whole time. The family was never
// undecided — the rule was simply never run against it.
//
// ⭐ AND THE VERDICT IS MEASURED, NOT ARGUED. Both were put through the shipped
// CLI with the diagnostic suppressed, elf64-x86_64:
//   * a TU whose quote-`#include` names a header that does not exist:
//     UNSUPPRESSED -> `error[P0016]`, no artifact. SUPPRESSED -> **zero error
//     lines and a 9,584-byte ELF64 executable on disk**.
//   * a self-including header with no include guard: UNSUPPRESSED ->
//     `error[P0022]`, no artifact. SUPPRESSED -> **zero error lines and a
//     9,584-byte ELF64 executable on disk**.
// A real binary, built green, from a translation unit that silently lost an
// entire header. That is prong (1) with the artifact in hand, and it is exactly
// the outcome the row predicted when it wrote that a build "can silently stop
// reporting missing headers, and the only survivor of that is a smaller number
// that looks like progress".
constexpr MembershipReason kWhyIncludeError{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a translation unit whose #include could not be resolved, read "
    "or nested compiles WITHOUT that header's contents and links into a real "
    "binary, green"};
constexpr MembershipReason kWhyIncludeReentryRefused{
    MembershipProng::WrongArtifactShipsGreen,
    "silenced, a header this build refused to re-enter is simply absent from "
    "the translation unit, which then compiles and links green -- and the "
    "refusal fires only on a genuine cycle or a gap in the guard detector, so "
    "it is the one line that could have told you which"};

// ⓘ EXTENT 150 → 151 (2026-08-15): `D_LanguageTargetIsaMismatch` (0xD02A)
// joined on prong (2) — see its row below. The extent is deliberately an
// explicit count rather than a deduced `[]`, so adding a row without thinking
// about it is a COMPILE ERROR rather than a silent append; ✔it caught this
// very addition. Raise it by hand, exactly once per row.
// ⓘ EXTENT 151 → 163 (2026-08-18, rebase of Cycles P5–P7 onto AP6): the two
// branches raised this independently — AP6 by 1 and P5 by 5 — so the rebase
// had to RECONCILE rather than pick a side. Taking either number would have
// dropped the other branch's rows silently past this guard.
// ⓘ EXTENT 163 → 164 (2026-08-19, cycle P20): `S_InlineAsmDuplicateSymbolicName`
// (0xE06C) joined on prong (1) — see its row below. ✔The explicit extent did its
// job again: the row was written first and the build failed until this line moved.
// ⓘ EXTENT 164 → 165 (2026-08-25, cycle P34): `A_ImmediateNarrowedToOperandField`
// (0x1009) joined on prong (1) — see its row below, and note it is the FIRST
// WARNING-severity member, which strengthens rather than stretches the prong.
// ✔The explicit extent did its job a third time: `too many initializers for
// 'UnsuppressableEntry [164]'` was the build's answer until this line moved.
// ⓘ EXTENT 165 → 166 (2026-08-26, cycle P36): `I_StoreValueTypeMismatch`
// (0xA01A) joined on prong (1) — see its row below. ✔The explicit extent did its
// job a FOURTH time: `too many initializers for 'UnsuppressableEntry [165]'` was
// the build's answer until this line moved, and it was the first thing the new
// row's syntax check reported.
// ⓘ EXTENT 166 → 168 (2026-08-26, cycle P36, D-DIAG-UNSUPPRESSABLE-FAMILY-UNDECIDED):
// `P_PreprocessorIncludeError` (0x0016) and `P_PreprocessorIncludeReentryRefused`
// (0x0022) join, both on prong (1), both with a measured ELF64 artifact as the
// evidence — see the block beside `kWhyIncludeError`. These are the two codes
// the row was filed about, and they close the split it called "arbitrary on its
// face": within the preprocessor's own codes, `#pragma` failures were
// unsuppressable while `#include` failures were not. ✔The explicit extent did
// its job a FIFTH time.
constexpr std::array<UnsuppressableEntry, 168> kUnsuppressableCodes{{
    // D_* build-lifecycle band — a `.dss-project.json` pre/post-build hook
    // that could not be spawned, or that ran and failed. PRONG (2), and only
    // prong (2): both already abort the build with or without the diagnostic
    // (MEASURED at `runBuildScripts`), so suppression ships no wrong bytes —
    // it leaves a non-zero exit with nothing on stderr saying which hook
    // failed or why. Their cap-immunity is NOT the reason they are here; that
    // is `DiagnosticDelivery`'s job, and these two would still need delivery
    // if they were not members.
    {DiagnosticCode::D_ScriptSpawnFailed, kWhyBuildHook},
    {DiagnosticCode::D_ScriptExitedNonZero, kWhyBuildHook},
    // ── `dependsOn` git acquisition (0xD01D..0xD020) — ✅ ALL FOUR LANDED,
    // 145 → 149, IN THE SAME CHANGE AS THEIR EMIT SITES ───────────────────
    // Judged individually 2026-08-13, because a four-code family is exactly
    // the shape that gets swept in as a batch (the 2-of-3 inline-asm split
    // below is this table's evidence that the criterion gets APPLIED). Each
    // verdict is per-code:
    //   * D_DependencyGitNotFound (0xD01D) — PRONG (2). `git` is absent, so
    //     no acquisition is possible and the build cannot proceed; suppressed,
    //     it fails with no statement that the missing tool was the cause, and
    //     the remediation ("install git") is the single most concretely
    //     actionable line the resolver can print.
    //   * D_DependencyGitAcquireFailed (0xD01E) — PRONG (2). A hard failure
    //     with no usable checkout: the dependency's sources do not exist on
    //     this machine, so continuing "would compile against a hole" and the
    //     build stops. Suppressed, it stops mutely.
    //   * D_DependencyGitFetchFallback (0xD01F) — ★ PRONG (1), the strongest
    //     of the four and the only one that reaches prong (1) at all. This is
    //     the notice that the build DELIBERATELY PROCEEDED on possibly-stale
    //     sources after a failed fetch. Suppressed, the build compiles sources
    //     the operator did not intend and reports SUCCESS — an artifact built
    //     from the wrong revision, shipping green, with the one line that
    //     would have said so removed. Note the asymmetry that makes it the
    //     strongest case: for the other three, suppression removes an
    //     explanation of a failure; for this one it removes the only evidence
    //     distinguishing a correct build from a stale one.
    //   * D_DependencyGitNameCollision (0xD020) — PRONG (2) on the emit shape
    //     its allocation note specifies ("Fail loud", detected on the derived
    //     names BEFORE acquisition, so the build stops). If the emit site
    //     instead proceeds with one of the two repos it becomes prong (1) —
    //     "the build would then compile against a dependency it did not ask
    //     for" — so this verdict must be re-read against the landed site, not
    //     assumed.
    //
    // ✔ RE-READ AGAINST THE LANDED SITES, 2026-08-14 (AP6 lane D1), because the
    // deferral note below used to say the verdicts must be. All four now emit
    // from `program/dependency_cache.cpp`:
    //   * 0xD01D — `DependencyCache::requireGit`, LATCHED so it fires once per
    //     build however many git dependencies asked, and returning false so the
    //     caller abandons. Prong (2) HOLDS.
    //   * 0xD01E — the acquire path's no-usable-checkout arm, which returns
    //     `CacheOutcome::AcquireFailed` with an EMPTY checkout path so a caller
    //     cannot proceed on it. Prong (2) HOLDS.
    //   * 0xD020 — `DependencyCache::registerGitDependency`, which returns
    //     nullopt BEFORE any clone or fetch runs, so the collision stops the
    //     build rather than picking a repo. The verdict therefore stays PRONG
    //     (2) and does NOT flip to prong (1): nothing is acquired, so nothing
    //     wrong can be compiled. (Had the site instead chosen one of the two,
    //     the entry would have had to be rewritten as prong (1).)
    //   * 0xD01F — the fetch-failed-with-usable-checkout arm, which emits at
    //     `DiagnosticSeverity::Info` with `DiagnosticDelivery::Guaranteed` set
    //     AT THE SITE and then RETURNS SUCCESS, so the build proceeds on
    //     unrefreshed sources. Prong (1) HOLDS, and it is the only one of the
    //     four that reaches prong (1) at all.
    // ⓘ The `Guaranteed` at 0xD01F's site is deliberately NOT redundant even
    // though membership also bypasses the cap. That bypass is a CONSEQUENCE of
    // what a member is, not a service membership offers: a code must obtain
    // delivery on delivery's merits, or the suppression criterion becomes a
    // side-channel again. `test_dependency_git_cache.cpp` asserts the field on
    // the emitted diagnostic, not merely that it survived a saturated cap —
    // membership alone would make the survival assertion vacuous.
    //
    // ⚠ 0xD019 / 0xD01A / 0xD01B / 0xD01C are a DIFFERENT question with a
    // different recorded answer and are NOT here. Do not extend this block to
    // them by pattern.
    {DiagnosticCode::D_DependencyGitNotFound, kWhyGitNotFound},
    {DiagnosticCode::D_DependencyGitAcquireFailed, kWhyGitAcquireFailed},
    {DiagnosticCode::D_DependencyGitFetchFallback, kWhyGitFetchFallback},
    {DiagnosticCode::D_DependencyGitNameCollision, kWhyGitNameCollision},
    // ── `dependsOn` GRAPH STRUCTURE: 0xD01A JOINS, 149 → 150, AND ITS THREE
    // SIBLINGS STILL DO NOT ────────────────────────────────────────────────
    // The paragraph above says 0xD019 / 0xD01A / 0xD01B / 0xD01C are "a
    // DIFFERENT question with a different recorded answer". They are — and the
    // answer, written when the resolver landed, splits them 1-of-4. That split
    // is the same evidence the 2-of-3 inline-asm split below is: the criterion
    // was APPLIED rather than the family swept in.
    //
    // ★ D_DependencyCycle (0xD01A) — PRONG (1), and the argument is
    // CODE-SPECIFIC rather than the code-independent one that was rejected
    // ("resolve() returns false, so a suppressed reject means rc=1 with empty
    // stderr" is equally true of eleven current NON-members). What makes this
    // one different is what the reject PREVENTS. The alternative to failing on
    // a back edge is breaking it and continuing, and its allocation note spells
    // out the consequence: the resolved dependency set then depends on where
    // the walk started, so two targets of ONE build can legitimately see
    // DIFFERENT source sets. So the reject AND ITS STATEMENT are the mechanism
    // — silencing it does not merely hide an explanation, it re-opens a
    // wrong-artifact shape whose symptom is an inexplicable link error much
    // later. That is prong (1) on the written criterion.
    //
    // ⚠ 0xD019 / 0xD01B / 0xD01C are STILL OUT, deliberately, and take
    // `DiagnosticDelivery::Guaranteed` at their emit sites instead: nothing
    // distinguishes them from `D_FileNotFound` or
    // `D_ArtifactProfileFormatMismatch`, both non-members, and 0xD01B is
    // allocated as the THIRD SIBLING of 0xD010 / 0xD011, which are non-members
    // too. Suppressing any of the three leaves a build that stopped with less
    // explanation — a DELIVERY concern, which now has its own property. The
    // three AP6 codes allocated after them (0xD022 / 0xD023 / 0xD024) get no
    // membership verdict at all this cycle: 0xD020's own note requires a
    // verdict be re-read against the landed site, not assumed, and 0xD025 /
    // 0xD026 arrive with the same reservation.
    //
    // ★ 0xD029 `D_DependencyBuildFailed` IS OUT, AND UNLIKE THE ROWS ABOVE THAT
    // IS A VERDICT RATHER THAN A RESERVATION — read off the landed emit site as
    // 0xD020's note requires. `dependency_resolver.cpp`'s `buildNode_` emits it
    // when a dependency's own sub-build returns non-zero and then returns
    // `nullopt`, so the resolve fails either way: no wrong bytes can ship, and
    // prong (1) is out.
    //
    // What makes it different from every other `dependsOn` code is prong (2),
    // and the difference is structural rather than a judgment call. Prong (2)
    // is narrow BY CONSTRUCTION — it requires that the diagnostic be the ONLY
    // statement of why the build stopped — and here it demonstrably is not:
    // the emit site's immediately preceding loop merges EVERY diagnostic from
    // the dependency's own build into this reporter, each carrying a
    // `contextPrefix` of `[dependency=<name> target=<spec>] `. Those are the
    // explanation; 0xD029 is the attribution line above them, and its own text
    // says so ("the reason is in the diagnostic(s) above"). Suppressing it
    // costs an operator a summary while leaving the cause on screen — advisory,
    // which is exactly what `--suppress` is documented to control.
    //
    // ⓘ It also does NOT take `DiagnosticDelivery::Guaranteed`, the other half
    // of 0xD019 / 0xD01B / 0xD01C's shape, and for the same reason: a pointer
    // whose referents are ordinary capped diagnostics must not outlive them.
    // Guaranteeing the pointer alone would produce a surviving line directing
    // the reader upward at nothing. If this needs cap-immunity, the merged
    // inner diagnostics take it FIRST and this line comes with them.
    {DiagnosticCode::D_DependencyCycle, kWhyDependencyCycle},
    // D_* driver / target band — pending-plan announcement,
    // permanent architectural exclusion of operand-stack / result-id
    // abiModels from the register-machine LIR pipeline, and the
    // D-LK6-8.2 split codes that close the SIGILL surface
    // (suppressing either would let `--target=arm64:elf64-x86_64...`
    // or schema-typo'd `machine` dispatch wrong PLT-stub emitter).
    {DiagnosticCode::D_PlanNotLanded, kWhyPlanNotLanded},
    {DiagnosticCode::D_TargetAbiModelUnsupportedByDriver, kWhyTargetAbi},
    {DiagnosticCode::D_TargetMachineCodeMismatch, kWhyTargetAbi},
    {DiagnosticCode::D_TargetAbiModelMismatch, kWhyTargetAbi},
    // ★★ D_LanguageTargetIsaMismatch (0xD02A) — A MEMBER ON PRONG (2), AND
    // THIS ROW CORRECTS THE CODE'S OWN ALLOCATION NOTE, WHICH WAS WRONG.
    // The note reasoned that suppressing it costs only "a build that stops
    // with LESS explanation" — a prong-(2) miss by a hair. ✔MEASURED through
    // the real CLI instead of reasoned about: `--suppress=
    // D_LanguageTargetIsaMismatch` on x86 assembly aimed at arm64 yields
    // **rc=1, stdout 0 bytes, stderr 0 bytes** — a completely silent non-zero
    // exit, which is prong (2) verbatim. There is no "less" explanation
    // because there is no OTHER diagnostic: unlike D_DependencyBuildFailed
    // (0xD029), which is an attribution line ABOVE a set of merged inner
    // diagnostics, this reject fires alone and both call sites then return
    // `nullopt`/false without reporting anything further. ⇒ it IS the whole
    // explanation, so removing it removes all of it.
    // ⓘ Its two nearest neighbours were already members for the same class of
    // reason, which is what makes the omission an inconsistency rather than a
    // judgement call: D_TargetMachineCodeMismatch and D_TargetAbiModelMismatch
    // sit immediately above. Prong-(2) precedent in its own band: 0xD01D and
    // 0xD020. ⚠ `DiagnosticDelivery::Guaranteed` does NOT substitute for
    // membership — delivery and suppression are separate questions, and the
    // measurement above was taken WITH Guaranteed already set.
    // Control that proves the fix works: suppressing a real member
    // (D_TargetMachineCodeMismatch) yields D_SuppressRequestIgnored and still
    // reports.
    {DiagnosticCode::D_LanguageTargetIsaMismatch, kWhyLanguageTargetIsa},
    // D_SynthRecipeFamilyUnknown (D-CSUBSET-C11-THREADS-HEADER /
    // D-FFI-PE-CRT-UCRT-MIGRATION, 2026-07-25): the driver's shim-synthesis
    // seam found a `synthesize` recipe id belonging to no known shim family
    // — a lockstep break between the descriptor loader's closed `kRecipes`
    // guard and the family split that feeds each synth pass. Suppressed, the
    // recipe would fall out of BOTH passes and the shim symbol would go
    // undefined with no diagnostic — a silently-undefined function that
    // breaks the binary's LOAD at user runtime (the eager-import law's
    // failure mode), not the build. It also replaces both seams' former
    // borrow of the linker-band `K_NoMatchingObjectFormat`, itself a member
    // below — so this entry PRESERVES the non-suppressible property rather
    // than granting a new one.
    {DiagnosticCode::D_SynthRecipeFamilyUnknown, kWhySynthRecipe},

    // F_* FFI band — architectural exclusions on the FF5 ingest path
    // (WASM/SPIR-V abiModels don't take FF4 mangling; empty canonical
    // names would silently shadow `bySymbol[""]` rows).
    {DiagnosticCode::F_FfiIngestAbiModelUnsupported, kWhyFfiIngest},
    {DiagnosticCode::F_FfiIngestEmptyCanonical, kWhyFfiIngest},
    // UCRT-P4 Decision 1 RETIRED F_FfiNoImportLibraryForFormat, and TF-C66
    // RETIRED F_FfiResolveLibrarySymbolAbsent; both were DE-LISTED here
    // 2026-08-10 by the EveryMemberHasAnEmitSite property below, which is what
    // found them still listed. The first gated a per-language
    // `DeclarationRule.externLibraryByFormat` map that no longer exists (a
    // per-symbol platform fact cannot be one string per language); the second
    // was an UNSOUND per-CU verdict (it could not see a sibling TU's definition
    // — sqlite false-positived 2770 times). Both surfaces now resolve at LINK
    // per C23 5.1.1.2 phase 8, where a genuine typo still rejects loud with
    // K_SymbolUndefined — a member below. Neither code has an emit site, and an
    // unemittable code cannot be suppressed, so membership asserted nothing
    // while making dead codes read as load-bearing (0xE025 precedent).
    // F_BinaryReaderPartialCorruption (silent-failure-hunter
    // 2nd-order audit on 9dbdc8e): a binary input was read only
    // PARTIALLY because some of it did not decode. PRONG (1): this is a
    // Warning, so `hasErrors()` is untouched and translation CONTINUES
    // on the salvaged part — suppressing it therefore ships a GREEN
    // build over an input the compiler itself could not fully parse,
    // with the one line saying so removed. Warning-severity members are
    // admissible: the criterion is about what suppression lets ship,
    // not about the producer's severity.
    //
    // ⚠ REWRITTEN 2026-08-13, and the rewrite is the point. This block
    // used to argue from the CAP — "without membership the four
    // cap/dedup gates could silently drop it under multi-target cap
    // saturation". That is the side-channel the membership rule at the
    // top of this file now forbids: it makes the reporter's cap
    // behaviour a reason to widen a SUPPRESSION table. The cap concern
    // was real, and it is now answered where it belongs, by
    // `DiagnosticDelivery`. What is left here is the argument this
    // table actually adjudicates, and the row stands on it alone.
    {DiagnosticCode::F_BinaryReaderPartialCorruption, kWhyBinaryReaderPartial},
    // F_ShippedHeaderNotFound (FF11, 2026-06-05): a `#include <h>`
    // SYSTEM header not found on any `shippedLibDirs` search dir. A
    // missing system header is a HARD error in C (unlike a local
    // include's soft `D_UnresolvedImport`) — suppressing or cap-dropping
    // it would let a program that calls an undeclared shipped-library
    // symbol compile SILENTLY, exactly the silent-miscompile this
    // fail-loud closes. The closed-table membership pins it.
    {DiagnosticCode::F_ShippedHeaderNotFound, kWhyShippedHeaderNotFound},
    // F_ShippedLibDescriptorMalformed / F_ShippedLibUnsupportedType
    // (neutral shipped-lib descriptor, 2026-06-06): the LANGUAGE-NEUTRAL
    // shipped-library JSON descriptor read by
    // `dss::ffi::readShippedLibDescriptor`. The first fires on a
    // malformed descriptor (bad JSON / missing-required-key / wrong-type
    // / unknown-key / bad enum); the second on a symbol whose
    // `signature` hir-text type fails to decode. Both are load-bearing:
    // suppressing either would let the lowering synthesize NO externs
    // (or skip a symbol that fails to decode) and silently compile a
    // program whose `#include <stdio.h>` symbols resolve to nothing —
    // exactly the silent dropped-import surface these fail-louds close.
    // The CRITICAL invariant is that a signature that does not decode
    // MUST NOT reach `makeExternFunction` with InvalidType.
    {DiagnosticCode::F_ShippedLibDescriptorMalformed, kWhyShippedLibDescriptor},
    {DiagnosticCode::F_ShippedLibUnsupportedType, kWhyShippedLibDescriptor},
    // F_ShippedHeaderUnavailableForTarget (p18 Cluster G c8, 2026-06-25):
    // a `#include <h>` whose shipped descriptor declares the header is NOT
    // available on the active target's object-format (POSIX <sys/time.h> on
    // windows-pe). Suppressing it would let the semantic phase resume past the
    // gate and INJECT the header's symbols/structs/typedefs on the wrong
    // platform — the exact wrong-platform silent miscompile this fail-loud
    // closes. A direct sibling of the three shipped-header surfaces above.
    {DiagnosticCode::F_ShippedHeaderUnavailableForTarget, kWhyShippedHeaderTarget},
    // F_ShippedSymbolUnavailableForTarget (D-FFI-SHIPPED-SYMBOL-ORACLE-IGNORES-OBJECT-FORMATS,
    // 2026-07-30): the per-SYMBOL sibling of the header gate directly above. The
    // `--resolve-library` oracle judged a name KNOWN on a format its descriptors
    // do NOT declare it for, and bound it to the format-default library; the
    // image then LINKED CLEAN and died at LOAD with no diagnostic (MEASURED:
    // elf-only `fdatasync` on a macho build → exit 255). Suppressing this would
    // restore exactly that silent loader death — the class this fail-loud exists
    // to convert into a compile-time error.
    {DiagnosticCode::F_ShippedSymbolUnavailableForTarget, kWhyShippedSymbolTarget},
    // F_HeaderNameCaseAmbiguous (D-PP-HEADER-CASE-INSENSITIVE-PE, 2026-08-04): an
    // `#include` name fold-matched TWO OR MORE distinct files under a
    // case-INSENSITIVE format's header-name convention. Suppressing it would
    // force the resolver to pick one — and since a case-only pair cannot exist on
    // NTFS or default APFS at all, the pick would differ by BUILD HOST. That is
    // the precise host-dependence the `headerNameMatching` axis removes, so a
    // suppressible form of this code would reinstate the defect one layer down.
    {DiagnosticCode::F_HeaderNameCaseAmbiguous, kWhyHeaderNameCase},
    // C_UnbackedPredefinedMacro (D-LANG-PREDEFINED-MACRO-REQUIRES-REALIZED-SURFACE,
    // 2026-08-18): a predefined macro's `requires` claim about the shipped header
    // surface is not backed by the corpus. The whole mechanism exists to convert
    // a claim that fails FAR from its declaration (inside the user's `#ifdef`
    // branch, as an unrelated-looking error) into one that fails AT the
    // declaration — so a suppressible form would restore the exact distance the
    // check removes, and the shipped `_MSC_VER` defect would be expressible
    // again with one flag.
    {DiagnosticCode::C_UnbackedPredefinedMacro, kWhyUnbackedPredefine},
    // F_ShippedCorpusInvariantBroken (D-FFI-DESCRIPTOR-INCLUDES-EDGE-GATE,
    // 2026-08-18): the corpus-wide sweep that ships WITH the conditional
    // `includes` edge gate. The gate lets a config author convert a loud failure
    // into an empty surface by writing a `when` that never fires; these
    // invariants are what keep that expensive. A suppressible form would hand
    // back the free silence the gate was designed not to sell.
    {DiagnosticCode::F_ShippedCorpusInvariantBroken, kWhyShippedCorpusInvariant},
    // F_ShippedStructVariantAmbiguous (p18 Cluster G, plan 25, 2026-06-26): a
    // shipped `structs` entry's per-target `variants` had MORE THAN ONE match the
    // active (arch, format). The selection contract is exactly-one-matches;
    // suppressing this would re-open the "pick the first" silent wrong-layout
    // surface (e.g. an under-specified `when:{arch:"x86_64"}` matching both
    // x86_64-elf and x86_64-pe → the linux struct layout used on windows). A
    // direct sibling of the four shipped-lib surfaces above — its invariant is the
    // SAME class (a wrong-bytes import must never ship green).
    {DiagnosticCode::F_ShippedStructVariantAmbiguous, kWhyShippedStructVariant},
    // F_ShippedConstantVariantAmbiguous / F_ShippedTypedefVariantAmbiguous /
    // F_ShippedMacroVariantAmbiguous (p18 Cluster G, plan 25 extension,
    // 2026-06-26): the per-target `variants` mechanism extended from `structs` to
    // the CONSTANTS, TYPEDEFS, and MACROS surfaces — a macOS build can get a
    // different constant VALUE / typedef WIDTH / macro REPLACEMENT than the linux
    // build from one descriptor. Each fires when MORE THAN ONE variant matches the
    // active target. Same selection contract + same silent-miscompile class as the
    // struct-variant sibling above: an under-specified `when` would silently pick
    // the first → a wrong constant value / typedef width / macro replacement on
    // this target. Suppressing any would re-open that "pick the first" wrong-value
    // surface — so all three are members like F_ShippedStructVariantAmbiguous.
    {DiagnosticCode::F_ShippedConstantVariantAmbiguous, kWhyShippedVariant},
    {DiagnosticCode::F_ShippedTypedefVariantAmbiguous, kWhyShippedVariant},
    {DiagnosticCode::F_ShippedMacroVariantAmbiguous, kWhyShippedVariant},
    // F_ShippedTypeIdentityConflict (D-LANG-TYPE-IDENTITY-VOCABULARY,
    // 2026-07-20): two descriptors resolved for the SAME target declare one
    // struct/union TAG (or typedef NAME) as DIFFERENT types, or a descriptor's
    // vocabulary tag contradicts the active language's width for that name.
    // The per-file reader structurally cannot see either — it reads ONE
    // descriptor at a time — yet injection is FIRST-WINS BY NAME, so the loser
    // interns a second type with NO field scope and the user gets an
    // include-order-dependent member-access failure (or, for the width case, a
    // phantom type matching no `_Generic` arm). Suppressing it would restore
    // exactly that silent first-wins. Same class as the five shipped surfaces
    // above: a wrong-bytes / unreachable-member import must never ship green.
    {DiagnosticCode::F_ShippedTypeIdentityConflict, kWhyTypeIdentityConflict},

    // H_* HIR-lowering / verifier band — structural invariants (cannot
    // reach MIR codegen without violating downstream contracts). Post-
    // fold #11 type-design CRITICAL: both `H_ExternDeclMalformed` and
    // `H_ExternHasInitializer` MUST be here — they are the two arms
    // of the H2 split, both terminate lowering with `return
    // errorNode(node)` + gate ok via errorCount.
    {DiagnosticCode::H_TypeUnresolved, kWhyHirStructural},
    {DiagnosticCode::H_VerifierFailure, kWhyHirStructural},
    {DiagnosticCode::H_UnsupportedLoweringForKind, kWhyHirStructural},
    {DiagnosticCode::H_ExternHasInitializer, kWhyHirStructural},
    {DiagnosticCode::H_ExternDeclMalformed, kWhyHirStructural},
    // H_WideCharSurrogateUnsupported (C11/C23 6.4.5, wide/UTF string literals):
    // a code point that cannot be represented in the requested element width
    // without truncation (astral under a 16-bit element, ill-formed UTF-8, or
    // cp > U+10FFFF). Same silent-miscompile class as H_UnsupportedLoweringForKind
    // — suppressing it would let a wrong/truncated code unit ship green. Emits an
    // Error HIR node + fails the gate via errorCount.
    {DiagnosticCode::H_WideCharSurrogateUnsupported, kWhyWideLiteral},
    // H_Utf8CharLiteralOutOfRange + H_WideCharValueUnrepresentable (C11/C23 6.4.4.4,
    // wide/UTF CHARACTER constants): a `u8'…'` code point > U+007F, or a wide/UTF
    // char that does not denote exactly one representable code unit (astral under a
    // 16-bit element, empty/multi-character, ill-formed UTF-8, cp > U+10FFFF). Same
    // silent-miscompile class as H_WideCharSurrogateUnsupported — suppressing either
    // would let a wrong/truncated code unit ship green. Emits an Error HIR node +
    // fails the gate via errorCount.
    {DiagnosticCode::H_Utf8CharLiteralOutOfRange, kWhyWideLiteral},
    {DiagnosticCode::H_WideCharValueUnrepresentable, kWhyWideLiteral},
    // H_InvalidUniversalCharacterName (C11/C23 6.4.3, Cycle C) + H_WideByteEscapeUnsupported
    // (6.4.5, D-CSUBSET-WIDE-HEX-OCTAL-ESCAPE-VALUE): a malformed/invalid `\u`/`\U`
    // universal character name, and a `\x`/octal byte escape in a wide/UTF literal.
    // Same silent-miscompile class as the wide/UTF codes above — suppressing either
    // would let a wrong/CESU-8/collapsed code unit ship green. Both emit an Error HIR
    // node + fail the gate via errorCount.
    {DiagnosticCode::H_InvalidUniversalCharacterName, kWhyWideLiteral},
    {DiagnosticCode::H_WideByteEscapeUnsupported, kWhyWideLiteral},
    // H_ConflictingStringLiteralPrefixes (C11/C23 6.4.5p5, Cycle D): a run of adjacent
    // string literals mixing TWO DIFFERENT non-narrow encoding prefixes (`u"a" U"b"`).
    // It is a silent-failure REASON code (like S_GenericSelectionNoMatch below): on the
    // conflict path the semantic typer leaves the stringLiteralExpr node UNTYPED and HIR
    // lowering returns an Error node, so the build already fails via errorCount /
    // hasErrors regardless of the emit gate — no wrong bytes ship. But a SUPPRESSED
    // conflict would fail the build with ZERO diagnostics shown (a confusing silent
    // failure) OR, worse, if a future reader keyed lowering on the semantic untyping
    // alone, re-open the silent MISCOMPILE the explicit fail-loud closes (a plain
    // `u"a" U"b";` statement typed Array<Char,3> "ab"). Closed here so a mixed-prefix
    // concat is never silent.
    {DiagnosticCode::H_ConflictingStringLiteralPrefixes, kWhyConflictingStringPrefixes},
    // H_ShippedShimSignatureMismatch (TF-C112, D-FFI-PE-CRT-UCRT-MIGRATION): a
    // user prototype re-declares a shipped row realized as a compiler-synthesized
    // SHIM with a signature that is not the row's. Unlike its H_* neighbours this
    // one has NO second line of defence: the refusal is the only thing standing
    // between the mismatch and a shim body that answers the call under a different
    // ABI than the caller made it — nothing downstream re-derives the callee's
    // signature from the prototype, so nothing else can notice. Suppressed, the
    // build goes GREEN and emits a wrong-ABI call. It also guards the surface the
    // whole cycle exists to close: the alternative realization for that symbol is a
    // raw `ucrtbase.dll` import, which does not load at all (0xC0000139).
    {DiagnosticCode::H_ShippedShimSignatureMismatch, kWhyShimSignature},

    // I_* MIR-verifier band — frozen-module invariants. A suppressed
    // violation here would let a miscompile sail past the verifier.
    // Post-fold #11 F2 expansion: all 12 I_* codes are structural
    // invariants in the same band as I_VerifierFailure/I_NoEntryBlock;
    // pre-fold only 5 were listed — the gap let `--suppress=I_NotDominated`
    // (or any other I_* code) re-open the SSA / CFG miscompile surface.
    {DiagnosticCode::I_VerifierFailure, kWhyMirVerifier},
    {DiagnosticCode::I_NoEntryBlock, kWhyMirVerifier},
    {DiagnosticCode::I_MultipleEntryBlocks, kWhyMirVerifier},
    {DiagnosticCode::I_EntryBlockNotFirst, kWhyMirVerifier},
    {DiagnosticCode::I_BlockNotTerminated, kWhyMirVerifier},
    {DiagnosticCode::I_PhiPredNotInCfg, kWhyMirVerifier},
    {DiagnosticCode::I_NotDominated, kWhyMirVerifier},
    {DiagnosticCode::I_TerminatorTypeMismatch, kWhyMirVerifier},
    {DiagnosticCode::I_ArgIndexOutOfRange, kWhyMirVerifier},
    {DiagnosticCode::I_ArgPositionDuplicate, kWhyMirVerifier},
    {DiagnosticCode::I_ExtensionTypeInMir, kWhyMirVerifier},
    {DiagnosticCode::I_NullptrTypeInMir, kWhyMirVerifier},
    {DiagnosticCode::I_StructCfMismatch, kWhyMirVerifier},
    {DiagnosticCode::I_UnreachableBlock, kWhyMirVerifier},
    // I_VlaAllocaOperandInvalid (VLA C1a, D-CSUBSET-VLA): the runtime-sized-Alloca
    // operand↔payload invariant (a VLA alloca carries exactly one size operand +
    // zero payload; a fixed alloca carries none). A member like every I_* verifier
    // invariant — a suppressed violation would let a mis-sized stack slot sail past.
    {DiagnosticCode::I_VlaAllocaOperandInvalid, kWhyVlaAllocaOperand},
    // I_AtomicAccessNotLowered (FC17.9(d) 1b, D-CSUBSET-ATOMIC): the atomic-lowering
    // belt — a plain Load/Store still carrying an `_Atomic`-qualified accessed type
    // is a missed funnel site that would SILENTLY perform a non-atomic access. A
    // member like every I_* verifier invariant; a suppressed violation would let a
    // non-atomic access to atomic memory sail past (the exact miscompile `_Atomic`
    // exists to prevent).
    {DiagnosticCode::I_AtomicAccessNotLowered, kWhyAtomicNotLowered},
    // I_CallSignatureMismatch (TF-C112, D-MIR-VERIFIER-NO-CALLSITE-SIGNATURE-CHECK):
    // the call-site signature belt — a MIR `Call` whose operands do not match its
    // statically-resolved callee's FnSig (arity, or the type at a POSITION). A
    // member like every I_* verifier invariant; suppressed, a mis-wired synthesis
    // shim would call the C runtime with arguments in the wrong slots and ship
    // green — a wrong-BYTES miscompile with no other build-time symptom.
    {DiagnosticCode::I_CallSignatureMismatch, kWhyCallSignature},
    // I_StoreValueTypeMismatch (P36, D-MIR-VERIFIER-STORE-CALLARG-TYPE-BLIND):
    // the memory-write typing belt — a Store/AtomicStore whose value type is not
    // its address pointee's. A member like every I_* verifier invariant;
    // suppressed, the exact narrowing-cast class that hid inside
    // D-CSUBSET-INT128-NARROWING-CAST-SITE-INCOMPLETE would go back to shipping
    // green — wrong bytes in memory, no diagnostic, no other symptom.
    //
    // ★★ THE TIER ARGUMENT, ON THE RECORD, BECAUSE THIS CODE WAS BORN REDDENING
    // FIFTEEN CORPUS EXAMPLES AND THAT IS EXACTLY WHEN A FALSE POSITIVE IS LEAST
    // AFFORDABLE — unsuppressable means no user can ever get past it. Three
    // reasons it is nonetheless a member, and one honest admission:
    //
    //  (1) WHAT IT GUARDS HAS NO OTHER SYMPTOM. Every other build-time signal is
    //      silent for a wrong-typed store: the bytes are written, the link
    //      succeeds, the binary runs and is wrong. A suppressible miscompile
    //      guard is a guard the first person under deadline turns off, and the
    //      thing they turn off is the only thing that was going to tell them.
    //
    //  (2) IT CANNOT BE TRIPPED BY A USER'S PROGRAM, ONLY BY OURS. The rule reads
    //      MIR, which no source language can author directly. A firing is always
    //      a defect in a DSS lowering or synthesis pass — never in the C the user
    //      wrote — so there is no legitimate program for an escape hatch to
    //      rescue. Suppressing it would let OUR defect ship under THEIR flag.
    //
    //  (3) THE WHOLE I_* BAND IS UNSUPPRESSABLE AND AN EXCEPTION WOULD BE UNSTATED.
    //      Making this the one silenceable verifier invariant would leave the
    //      band's meaning depending on which code fired.
    //
    //  ⚠ (4) THE ADMISSION: it DID have a false-positive class on day one — a
    //      store into an `enum` / bit-field `_BitInt` slot whose value carries the
    //      tier's own declared CONTAINER type. Width, signedness and bits all
    //      agree there, so nothing could be miscompiled. That was fixed AT THE
    //      RULE (mir_verifier.cpp, the declared-representation narrowing), which
    //      is the only correct answer: the escape for a false positive is to fix
    //      the rule, never to hand the user a switch that also silences the true
    //      positives. If a future firing turns out to be a false positive, that is
    //      a bug in this rule with the same severity as a missed store — fix it
    //      there, and do NOT promote this code out of the band to buy time.
    {DiagnosticCode::I_StoreValueTypeMismatch, kWhyStoreValueType},

    // K_* linker band — image refused / undefined extern + the LK10
    // image-write contract codes. Suppressing any K_ImageWrite* code
    // would let `errorCount() == 0` while the image is missing/truncated
    // on disk — exactly the silent-failure LK10 cycle 1 closed.
    {DiagnosticCode::K_SymbolUndefined, kWhyLinkerImage},
    {DiagnosticCode::K_ImageNotOk, kWhyLinkerImage},
    {DiagnosticCode::K_ImageWriteParentMissing, kWhyLinkerImage},
    {DiagnosticCode::K_ImageWriteOpenFailed, kWhyLinkerImage},
    {DiagnosticCode::K_ImageWriteShort, kWhyLinkerImage},
    {DiagnosticCode::K_ImageWriteCloseFailed, kWhyLinkerImage},
    {DiagnosticCode::K_ImageEmpty, kWhyLinkerImage},
    // Post-fold #12 D-FF2-UNSUPP-FULL-SWEEP additions:
    // K_NoMatchingObjectFormat — format-walker dispatch invariant
    //   (suppressing → wrong walker / corrupted artifact)
    // K_FormatLacksImportSupport — extern resolution against format
    //   without import-table support (suppressing → unresolved extern
    //   in dynamic image)
    // K_RelocationKindMismatch — applying a reloc kind the format
    //   doesn't support (suppressing → silent miscompile bytes)
    // K_WalkerInputContractViolation — walker received malformed input
    //   from the linker driver (suppressing → upstream corruption
    //   propagates downstream silently)
    {DiagnosticCode::K_NoMatchingObjectFormat, kWhyNoMatchingObjectFormat},
    {DiagnosticCode::K_FormatLacksImportSupport, kWhyFormatLacksImportSupport},
    {DiagnosticCode::K_RelocationKindMismatch, kWhyRelocationKindMismatch},
    {DiagnosticCode::K_WalkerInputContractViolation, kWhyWalkerInputContract},
    // K_EntryPointResolvesToExtern — extern-named-as-entry is a
    // schema misconfiguration that produces a runnable binary
    // pointing at a stub IAT slot. Suppressing → the loader jumps
    // to unrelocated import-stub bytes at process entry → SEGV with
    // no diagnostic trail.
    {DiagnosticCode::K_EntryPointResolvesToExtern, kWhyEntryResolvesToExtern},
    // K_DuplicateDataSymbol / K_BssDataHasBytes — producer-side
    // AssembledData invariant violations caught by
    // `validateAssembledData()`. Suppressing either would let a
    // producer ship an `AssembledModule` with two items at the
    // same SymbolId (last-write-wins silent resolution) or a Bss
    // item carrying bytes that would either bloat the on-disk
    // image or silently drop. Both are substrate-shape violations
    // that must not be silently accepted. 3rd-order audit fold
    // (D-LK4-RODATA-BSS-INVARIANT).
    {DiagnosticCode::K_DuplicateDataSymbol, kWhyAssembledData},
    {DiagnosticCode::K_BssDataHasBytes, kWhyAssembledData},
    // K_FormatLacksStackReserveControl / K_InvalidStackReserveRequest
    // (D-SQLITE-PE64-FULL-TIER-STACK-DEPTH) — the per-program stack-reserve
    // request gate. Suppressing either restores EXACTLY the silent drop the
    // capability exists to prevent: the build would report success while
    // emitting an image whose stack is NOT the size the program asked for,
    // and the failure surfaces later as a stack overflow at a recursion depth
    // with no diagnostic trail back to the dropped request.
    {DiagnosticCode::K_FormatLacksStackReserveControl, kWhyStackReserve},
    {DiagnosticCode::K_InvalidStackReserveRequest, kWhyStackReserve},
    // K_FormatLacksProcessExit / K_ExecEntryNotTrampolined (D-LK10-ENTRY §2.13)
    // — the entry-trampoline contract, the same format-capability shape as the
    // two codes above. They are TWO codes because their predicates DISAGREE on
    // one field, and both REPLACE A SILENT WRONG-ENTRY EMISSION, so suppressing
    // either restores exactly the defect it closed:
    //   K_FormatLacksProcessExit — an exec-flavored format that declares NO
    //     `processExit` skipped trampoline synthesis with NO diagnostic
    //     (linker.cpp's gate tested the same predicate the emitter would have
    //     failed on, making the emitter's own check dead code by construction).
    //   K_ExecEntryNotTrampolined — a walker reached DIRECTLY (bypassing
    //     `linker::link`) with no `imageEntryOverride`, under a format that DOES
    //     declare `processExit`, defaulted the image entry to `functions[0]` —
    //     MEASURED: a Mach-O exec whose LC_MAIN entryoff pointed at `main`'s
    //     `sub rsp,0x10` prologue, rc=0, zero diagnostics.
    // Neither has a runtime symptom that points back here: the program simply
    // runs the wrong entry (or falls off it), so the diagnostic IS the only
    // trace and must be undroppable. ★ And suppression is not even required to
    // lose it — a code absent from this table is droppable by the reporter's
    // dedup window and per-code cap, so membership is part of CREATING the
    // code, not a follow-up.
    {DiagnosticCode::K_FormatLacksProcessExit, kWhyEntryTrampoline},
    {DiagnosticCode::K_ExecEntryNotTrampolined, kWhyEntryTrampoline},
    // The PROGRAM-ENTRY RESOLUTION family (D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE,
    // 2026-08-10) — four codes, one rule, and the rule is that EVERY outcome of
    // entry resolution other than "exactly one candidate" must be undroppable.
    // Suppression here restores a MEASURED fault rather than a hypothetical one:
    // before these codes existed, `int main(int, char**, char**)` compiled rc=0
    // with ZERO diagnostics on pe64 AND elf64 and the emitted binary faulted on
    // its first envp dereference (`0xC0000005` / SIGSEGV rc=139), while gcc built
    // the identical source correctly. C23 5.1.2.2.1 permits supporting that shape
    // OR refusing it — accepting-then-faulting is the one outcome it does not
    // allow, and suppression would hand exactly that binary back. A silently
    // miscompiled entry has NO other trace: the build reports success.
    //
    // ★ WHY ALL FOUR AND NOT JUST THE SHAPE CHECK. Each is a distinct way to end
    // up running the wrong code, or none, with a successful-looking build:
    //   * S_EntryShapeNotDeclared — the definition has the wrong signature for its
    //     name (the measured 3-param-main fault above).
    //   * K_ProgramEntryUndefined — an exec build with NO candidate. Suppressed,
    //     the build proceeds to a link-tier "symbol not found" at best, and the
    //     explanation of WHY (the verb this format cannot realize) is lost.
    //   * K_ProgramEntryAmbiguous — two rival candidates. Suppressed, resolution
    //     falls back to first-match-wins on a program ENTRY, which is the silent
    //     wrong-entry class in its purest form.
    //   * K_EntryVerbUnmaterializable — the decided verb and the MIR signature
    //     disagree. Suppressed, the materializer reads past the end of the
    //     parameter list.
    {DiagnosticCode::S_EntryShapeNotDeclared, kWhyProgramEntry},
    {DiagnosticCode::K_ProgramEntryUndefined, kWhyProgramEntry},
    {DiagnosticCode::K_ProgramEntryAmbiguous, kWhyProgramEntry},
    {DiagnosticCode::K_EntryVerbUnmaterializable, kWhyProgramEntry},

    // The extern-import dedup fold (D-LK11-EXTERN-IMPORT-DEDUP) at BOTH merge
    // tiers — linker.cpp `mergeModules` and mir_merge.cpp — collapses N CUs'
    // imports of one dynamic symbol into one row. Four fields cannot be folded
    // and must be a hard stop: `isData` and `isThreadLocal` SELECT THE BINDING
    // MODEL, and two differing non-zero `dataSizeBytes`/`dataAlignBytes` are
    // two CUs declaring DIFFERENT objects under one external name.
    // Suppressing this restores first-wins EXACTLY — the merge proceeds with
    // one CU's answer and the other CU's calls bind through the wrong model
    // (a PLT stub standing in for a data object: D-LK-EXTERN-DATA-IMPORT).
    // ★ AND SUPPRESSION IS NOT EVEN REQUIRED TO LOSE IT: a code absent from
    // this table is also droppable by the reporter's dedup window and its
    // per-code cap, so on a link the size of the 103-TU SQLite CLI the
    // diagnostic can vanish with NO flag and no trace. That is why membership
    // here is part of creating the code, not a follow-up.
    {DiagnosticCode::K_ExternImportAttributeConflict, kWhyExternImportConflict},

    // L_* LIR verifier / lowering band — structural invariants
    // (cannot reach assembler-tier codegen without violating
    // downstream contracts). ✔MEASURED 2026-08-14: 17 L_* rows in the
    // array below (the band grew by the three side-structure codes).
    // Count it, never re-quote it — the header's own enumeration of this
    // band has already gone stale twice. Every one of them fires from an
    // arm that gates
    // the producer's ok() / return value. Suppressing any → silent
    // miscompile through the LIR layer.
    //
    // Post-fold #13 silent-failure CRITICAL: L_UnsupportedLoweringForOpcode
    // (0xB001) is the MIR→LIR analog of H_UnsupportedLoweringForKind —
    // fires from 22+ sites across mir_to_lir.cpp / lir_callconv.cpp /
    // lir_2addr_legalize.cpp / lir_pass_util.cpp / lir_verifier.cpp on
    // every coverage-gap deferral. Was omitted in post-fold #12 →
    // `--suppress=L_UnsupportedLoweringForOpcode` silently re-opened
    // the MIR→LIR miscompile surface for unrecognized opcodes.
    //
    // L_IndirectCalleeClobberedByArgSetup (FC4 c2): the backstop for
    // the indirect-callee regalloc rules — suppressing it would turn
    // a callee-clobbered-by-arg-setup regression back into a SILENT
    // garbage jump through an argument value.
    {DiagnosticCode::L_UnsupportedLoweringForOpcode, kWhyLirStructural},
    {DiagnosticCode::L_RequiredLirOpcodeMissing, kWhyLirStructural},
    {DiagnosticCode::L_VirtualRegInPostRegalloc, kWhyLirStructural},
    {DiagnosticCode::L_MemOperandMalformed, kWhyLirStructural},
    {DiagnosticCode::L_PhysRegOrdinalOutOfRange, kWhyLirStructural},
    {DiagnosticCode::L_InvalidSpillSlotSentinel, kWhyLirStructural},
    {DiagnosticCode::L_MoveCycleUnsupported, kWhyLirStructural},
    {DiagnosticCode::L_IndirectCallUnsupported, kWhyLirStructural},
    {DiagnosticCode::L_IndirectCalleeClobberedByArgSetup, kWhyLirStructural},
    {DiagnosticCode::L_StackPassedArgUnsupported, kWhyLirStructural},
    {DiagnosticCode::L_CcRegLookupFailed, kWhyLirStructural},
    // L_VlaDynamicAllocaUnsupported (VLA C1a→C1b boundary, D-CSUBSET-VLA): a runtime-
    // sized `Alloca` reached `lowerAlloca` (the dynamic `sub rsp,<size>` is the named
    // C1b cycle). A member: suppressed, the alloca would fall through to the fixed-
    // slot path and silently emit a `lea` of a 1-slot scalar for the whole VLA — a
    // stack miscompile (MINOR-3). Same load-bearing-boundary class as the L_ band.
    {DiagnosticCode::L_VlaDynamicAllocaUnsupported, kWhyVlaDynamicAlloca},
    // L_VlaNonLeafFrameUnsupported (VLA C1b LEAF gate, D-CSUBSET-VLA-NONLEAF-CALL-FRAME):
    // a VLA function that ALSO calls / uses va_start. A member: suppressed, a non-leaf
    // VLA would place outgoing call args INSIDE the VLA region under the moved SP (an
    // ABI break — a silent stack miscompile). Same load-bearing-boundary class.
    {DiagnosticCode::L_VlaNonLeafFrameUnsupported, kWhyVlaNonLeafFrame},
    {DiagnosticCode::L_TerminatorSuccessorMismatch, kWhyTerminatorSuccessors},

    // Module SIDE-STRUCTURE integrity (D-LIR-PER-INST-REG-CONSTRAINTS).
    // The literal pool and the per-instruction register-
    // constraint pool are referenced BY INDEX from the instruction stream,
    // and four passes rebuild that stream into a fresh builder. Every way
    // the carry can fail is silent — the module stays well-formed and the
    // loss surfaces as wrong bytes. Suppressed, `IndexDangling` becomes an
    // abort deep in a pool accessor with no instruction named,
    // `PoolShrank` becomes a dangling reference one pass later, and
    // `ReferenceLost` becomes a vanished clobber set that lets the
    // allocator reuse a register the instruction destroys.
    {DiagnosticCode::L_SideStructureIndexDangling, kWhySideStructureIntegrity},
    {DiagnosticCode::L_SideStructurePoolShrank, kWhySideStructureIntegrity},
    {DiagnosticCode::L_SideStructureReferenceLost, kWhySideStructureIntegrity},

    // R_* regalloc band — calling-convention / class invariants.
    // R_SpilledDueToPressure + R_SpilledDueToCrossCallExhaustion
    // are Info-severity (intentional informational signal) and stay
    // OUT of the table; only the Error-severity gating codes are
    // members.
    {DiagnosticCode::R_NoCallingConventions, kWhyRegallocInvariant},
    {DiagnosticCode::R_CallingConventionLookupFailed, kWhyRegallocInvariant},
    {DiagnosticCode::R_VRegHasNoClass, kWhyRegallocInvariant},

    // A_* assembler / encoding band — bytes-on-disk invariants
    // (suppressing → wrong machine code emitted). The
    // A_NoMatchingEncodingVariant arm fires from format walkers
    // when no encoding row matches; A_RoundTripMismatch fires from
    // the round-trip self-test; A_NoEncodingDeclared /
    // A_NoEncodingShapeWalker / A_LirToMirSizeMismatch are
    // pipeline-shape invariants.
    {DiagnosticCode::A_LirToMirSizeMismatch, kWhyEncodingBytes},
    {DiagnosticCode::A_NoMatchingEncodingVariant, kWhyEncodingBytes},
    {DiagnosticCode::A_RoundTripMismatch, kWhyEncodingBytes},
    {DiagnosticCode::A_NoEncodingDeclared, kWhyEncodingBytes},
    {DiagnosticCode::A_NoEncodingShapeWalker, kWhyEncodingBytes},
    // D-LK10-ENTRY-ARM64 (v0.0.2 V2-1): a too-wide immediate that
    // can't fit a fixed32 immediate slot must never be silently
    // truncated to a wrong machine-code constant (e.g. wrong syscall
    // number). Same bytes-on-disk-invariant band as the others above.
    {DiagnosticCode::A_ImmediateOperandOutOfRange, kWhyImmediateRange},
    // D-ASM-X86-IMMEDIATE-WINDOW-REFUSES-WHAT-GAS-TRUNCATES (cycle P34).
    //
    // ★★★ THE STRONGEST PRONG-(1) MEMBER IN THIS TABLE, AND ITS SEVERITY IS
    // WHY. Every other member above is an ERROR: suppress it and the build
    // still fails, so no wrong bytes ship by that route alone. This one is a
    // WARNING — the build SUCCEEDS and the narrowed instruction goes to disk
    // by design. Prong (1) reads "wrong artifact ships green"; here the green
    // is not a hypothetical consequence of silencing the code, it is the
    // code's own normal operating condition. The diagnostic is the ONLY thing
    // standing between a narrowed immediate and a silent one.
    //
    // ★★ AND THE ARCHITECTURAL REASON, WHICH IS THE DECIDING ONE. The
    // operator ruled a THIRD arm over two opposed ones: (A) match the
    // reference and truncate silently, (B) keep refusing what the reference
    // assembles. The third arm is (A)'s acceptance plus (B)'s loudness. If
    // `--suppress=0x1009` could silence the warning, the third arm would
    // COLLAPSE BACK INTO ARM (A) at the flick of one flag — the ruling would
    // ship as a default rather than as a behaviour, and the arm the operator
    // rejected would be one command line away. A ruling that a flag can undo
    // was not a ruling.
    //
    // ⓘ THE COUNTER-ARGUMENT, CONSIDERED AND REJECTED: "an unsilenceable
    // warning breaks a -Werror build of legitimate code." It does not. This
    // is not an error and does not gate `ok`; and every value that trips it
    // has an exact, local, zero-cost remedy — write the constant that fits
    // (`$0x7fff` for `$-32769`). A diagnostic whose remedy is one edit in the
    // line that caused it is precisely the kind that may be made
    // unsuppressable; one that merely hides advice about code the author
    // cannot change is not, which is why P_PreprocessorWarningDirective and
    // S_DeprecatedSymbolUsed stay suppressable below.
    //
    // ⓘ It needs no `DiagnosticDelivery::Guaranteed`: membership already
    // bypasses the volume cap (`mustDeliver`), and a narrowing warning lost
    // to a cap is as silent as a suppressed one.
    {DiagnosticCode::A_ImmediateNarrowedToOperandField, kWhyImmediateNarrowed},
    {DiagnosticCode::A_AsmTextUnsupported, kWhyAsmTextUnsupported},

    // S_* semantic band — silent-MISCOMPILE guards.
    // c27 (D-CSUBSET-VOLATILE-POINTEE, 2026-06-27) RETIRED
    // S_VolatilePointeeNotSupported: a pointer-to-volatile-POINTEE is no longer a
    // reject — `volatile` is now a TYPE qualifier (TypeKind::VolatileQual), so
    // `volatile <base> *` builds Ptr<VolatileQual(base)> and the deref carries
    // MirInstFlags::Volatile from the pointee type (the c21 model-B limitation the
    // reject fronted is gone). The diagnostic enum + name are kept for ordinal
    // stability / historical golden references but the code is NEVER emitted, so it
    // is no longer a member of this closed unsuppressable table (an unemittable code
    // cannot be suppressed). The silent-miscompile it once guarded is now prevented
    // by the access chokepoint, pinned red-on-disable by the `volatile_pointee_cse`
    // corpus + the multi-site MIR access tests.
    // S_IncompleteTypeMember (c24, D-CSUBSET-SELF-REFERENTIAL-STRUCT, 2026-06-27):
    // a DIRECT (non-pointer) member of an INCOMPLETE composite — e.g.
    // `struct N { struct N n; }` (a struct containing itself by value, an
    // infinite-size cycle). Suppressing it would let the member fold its size to
    // 0 (the incomplete composite has no layout) — a silent wrong-bytes layout.
    // Same silent-miscompile-guard class as the entries above; a pointer-to-
    // incomplete (`struct N *`) is legal and is NOT rejected.
    {DiagnosticCode::S_IncompleteTypeMember, kWhyIncompleteType},
    // S_IncompleteTypeObject (c35, D-CSUBSET-FORWARD-STRUCT-DECLARATION,
    // 2026-06-28): a by-VALUE OBJECT (local/global) of an INCOMPLETE composite —
    // `struct S v;` where `struct S` is forward-declared but never defined. c35's
    // opaque-tag forward-mint makes the reference RESOLVE (so an opaque `struct S
    // *` pointer compiles); suppressing this by-value reject would let the object
    // fold its frame/.bss size to 0 (the incomplete composite has no layout) — a
    // silent wrong-bytes object. Same silent-miscompile-guard class as
    // S_IncompleteTypeMember (the by-value MEMBER case); a pointer-to-incomplete is
    // legal and is NOT rejected.
    {DiagnosticCode::S_IncompleteTypeObject, kWhyIncompleteType},
    // S_TypeNameDeclaratorNotAbstract (c26, D-CSUBSET-ABSTRACT-DECLARATOR-TYPE-NAME,
    // 2026-06-27): a TYPE-NAME (cast / sizeof / compound-literal) whose abstract
    // declarator illegally carries a NAME (`(int x)expr`). NOTE — unlike the
    // silent-miscompile guards above, suppressing this ships NO wrong bytes: the
    // type resolves to InvalidType regardless of the emit gate, so the build still
    // fails. It is closed here so the failure is never SILENT — a suppressed
    // constraint violation would otherwise fail the build with zero diagnostics
    // shown (a confusing silent failure REASON), which the closed table forbids.
    {DiagnosticCode::S_TypeNameDeclaratorNotAbstract, kWhySilentConstraint},
    // S_StaticAssertFailed (FC16, D-CSUBSET-STATIC-ASSERT, 2026-07-07): a
    // `_Static_assert(cond[, "msg"]);` whose condition is non-constant or folds
    // to zero. Same posture as S_TypeNameDeclaratorNotAbstract above —
    // suppressing it ships NO wrong bytes (the analyzer's error still fails the
    // build via `hasErrors()`), but a suppressed constraint violation would
    // fail the build with ZERO diagnostics shown — a confusing silent failure
    // REASON the closed table forbids. Closed here so a false static_assert is
    // never silent.
    {DiagnosticCode::S_StaticAssertFailed, kWhySilentConstraint},
    // S_GenericSelectionNoMatch / S_GenericSelectionAmbiguous (FC16,
    // D-CSUBSET-GENERIC-SELECTION, 2026-07-07): a `_Generic` whose controlling
    // type matched no typed association (and had no `default`), or matched more
    // than one. Same posture as S_StaticAssertFailed / S_TypeNameDeclaratorNot-
    // Abstract above: on the no-match/ambiguous path the genericExpr node is left
    // UNTYPED (InvalidType), so the build fails via `hasErrors()` regardless of
    // the emit gate — no wrong bytes ship — but a suppressed constraint violation
    // would fail the build with ZERO diagnostics shown, a confusing silent
    // failure REASON the closed table forbids. Closed here so an unselectable
    // `_Generic` is never silent.
    {DiagnosticCode::S_GenericSelectionNoMatch, kWhySilentConstraint},
    {DiagnosticCode::S_GenericSelectionAmbiguous, kWhySilentConstraint},
    // S_Alignas* (C11/C23 6.7.5, D-CSUBSET-ALIGNAS, 2026-07-07): the five
    // `_Alignas`/`alignas` constraint violations — not-power-of-two, exceeds-max,
    // weaker-than-natural, invalid-context (typedef/function/parameter/bit-field),
    // and non-constant. Same posture as S_StaticAssertFailed above: each is a
    // 6.7.5 CONSTRAINT violation; the analyzer's error already fails the build via
    // `hasErrors()` (no wrong bytes ship — the stored alignment is simply not
    // applied), but a SUPPRESSED constraint violation would fail the build with
    // ZERO diagnostics shown, the confusing silent-failure REASON the closed table
    // forbids. Closed here so an invalid alignas is never silent.
    {DiagnosticCode::S_AlignasNotPowerOfTwo, kWhySilentConstraint},
    {DiagnosticCode::S_AlignasExceedsMax, kWhySilentConstraint},
    {DiagnosticCode::S_AlignasWeakerThanNatural, kWhySilentConstraint},
    {DiagnosticCode::S_AlignasInvalidContext, kWhySilentConstraint},
    {DiagnosticCode::S_AlignasNonConstant, kWhySilentConstraint},
    // S_PackedBitfieldUnsupported (FC16, D-CSUBSET-PACKED, 2026-07-08): a `packed`
    // struct/union that ALSO carries a bit-field member — an UNSUPPORTED combination
    // (bit-granular packed packing is a distinct, deferred algorithm). Unlike the
    // S_Alignas* constraint violations above, suppressing THIS would ship WRONG BYTES:
    // the layout engine's nullopt belt fails the type out on the packed+bitfield path,
    // so a suppressed diagnostic would leave the composite to be laid out padded (the
    // wrong ABI). Closed here so a packed bit-field struct is never silently mislaid.
    // (S_UnknownTypeAttribute is deliberately NOT a member — it mirrors the suppressible
    // H_UnknownLinkageSpecifier typo diagnostic, and the build still fails via
    // hasErrors when it fires unsuppressed.)
    {DiagnosticCode::S_PackedBitfieldUnsupported, kWhyPackedBitfield},
    // S_NullptrInvalidOperand (FC17, D-CSUBSET-NULLPTR): `nullptr` used as an
    // invalid operator operand (`nullptr + 1`, `nullptr < p`, `-nullptr`). Unlike a
    // plain type mismatch, suppressing THIS would ship a SILENT MISCOMPILE: the HIR
    // lowering turns `nullptr` into the integer-0 null constant, so a suppressed
    // diagnostic would leave `nullptr + 1` compiled as `0 + 1 == 1` — ill-formed C
    // silently accepted. Closed here so nullptr misuse is never silently lowered.
    {DiagnosticCode::S_NullptrInvalidOperand, kWhyNullptrOperand},
    // S_InvalidEnumUnderlyingType / S_EnumeratorValueOutOfRange (FC17,
    // D-CSUBSET-ENUM-UNDERLYING-TYPE, C23 6.7.2.2): the explicit enum
    // underlying-type constraint violations — a non-integer underlying type
    // (`enum E : float`) and an enumerator value out of the underlying's range
    // (`enum E : unsigned char { A = 256 }`). Both ship WRONG BYTES if suppressed:
    // a suppressed invalid-underlying would silently lay the enum out at the default
    // int width/signedness instead of failing, and a suppressed out-of-range value
    // would be truncated/wrapped into the underlying type — a wrong constant. Same
    // silent-miscompile-guard class as S_PackedBitfieldUnsupported above. (The
    // default-int enum path never emits either, so unsuppressing changes nothing
    // for existing enums.)
    {DiagnosticCode::S_InvalidEnumUnderlyingType, kWhyEnumUnderlying},
    {DiagnosticCode::S_EnumeratorValueOutOfRange, kWhyEnumUnderlying},
    // S_TypeofBitfieldOperand (FC17, D-CSUBSET-TYPEOF, C23 6.7.2.5): the operand
    // of a `typeof`/`typeof_unqual` is a bit-field member access. Same
    // silent-miscompile-guard class as the enum/nullptr entries above: on the
    // reject path the typeof node resolves to InvalidType, so the build fails via
    // hasErrors() regardless of the emit gate — but a SUPPRESSED constraint
    // violation would silently resolve the typeof to the bit-field's declared
    // (widened) type, a wrong type in the declaration it specifies. Closed here so
    // a bit-field typeof is never silently mistyped.
    {DiagnosticCode::S_TypeofBitfieldOperand, kWhyTypeofBitfield},
    // S_Constexpr* (FC17, D-CSUBSET-CONSTEXPR, C23 6.7.1): the five constexpr
    // OBJECT constraint violations — non-constant initializer, missing
    // initializer, unsupported (aggregate) object type, constexpr-on-a-function,
    // and a volatile-qualified object type. Each is a 6.7.1 constraint whose
    // SUPPRESSION would silently degrade `constexpr` to plain `const` — the exact
    // silent-accept the feature's fail-loud contract forbids (a constexpr object
    // IS its translation-time value; a declaration that cannot deliver that value
    // must never compile quietly). The function form additionally guards a wrong
    // INTERNAL linkage (the file-scope constexpr linkage row would apply to a
    // function the object-only feature never validated). Closed here so an
    // invalid constexpr is never silent.
    {DiagnosticCode::S_ConstexprNonConstantInitializer, kWhyConstexpr},
    {DiagnosticCode::S_ConstexprMissingInitializer, kWhyConstexpr},
    {DiagnosticCode::S_ConstexprUnsupportedType, kWhyConstexpr},
    {DiagnosticCode::S_ConstexprFunctionNotSupported, kWhyConstexpr},
    {DiagnosticCode::S_ConstexprInvalidQualifier, kWhyConstexpr},
    // S_Auto* (FC17.5, D-CSUBSET-AUTO-TYPE-INFERENCE, C23 6.7.9): the four
    // initializer-inference constraint violations — multi-declarator, a
    // derived (non-plain-identifier) declarator, a missing initializer, and
    // an invalid inference (missing required `auto` specifier / void /
    // nullptr_t / unresolvable-self-referential initializer). Suppressing ANY
    // of them re-opens a SILENT-MISCOMPILE seam: the inference arm is the
    // only tier that types these symbols at Pass 1.5, and Pass 2's decl arm
    // BACKFILLS `rec.type = initializer-type` for any still-unresolved
    // declarator-mode symbol — so a suppressed violation would silently
    // adopt the initializer's type and compile the very form the constraint
    // forbids (`static x = 5;` as implicit-int, `auto a = 1, b = 2;`
    // per-declarator, a NullptrT-typed object headed for the 0xA014 MIR
    // tripwire). Closed here so a rejected inference never compiles quietly.
    {DiagnosticCode::S_AutoRequiresSingleDeclarator, kWhyAutoInference},
    {DiagnosticCode::S_AutoRequiresPlainIdentifier, kWhyAutoInference},
    {DiagnosticCode::S_AutoRequiresInitializer, kWhyAutoInference},
    {DiagnosticCode::S_AutoInferenceInvalid, kWhyAutoInference},
    // S_ThreadLocal* (TLS C1, D-CSUBSET-THREAD-LOCAL, C11/C23 6.7.1 + 6.6p9):
    // the five thread-storage constraint violations — thread_local on a
    // function, a block-scope object without static/extern, a same-TU
    // redeclaration mismatch, a thread-local address in a static initializer,
    // and a forbidden storage-class combination (constexpr / register).
    // Suppressing ANY of them ships wrong STORAGE bytes, not just a missed
    // lint: the block-scope form would lower as a per-call automatic, the
    // redeclaration mismatch would bind half the accesses to the wrong
    // storage, and the address-constant form would emit an abs64 relocation
    // whose resolved value is a link-time tpoff bit-cast into a data slot (a
    // silent garbage pointer — the arc's CRIT-1). Closed here so an invalid
    // thread_local never compiles quietly.
    {DiagnosticCode::S_ThreadLocalOnFunction, kWhyThreadLocal},
    {DiagnosticCode::S_ThreadLocalRequiresStaticOrExtern, kWhyThreadLocal},
    {DiagnosticCode::S_ThreadLocalRedeclarationMismatch, kWhyThreadLocal},
    {DiagnosticCode::S_ThreadLocalAddressNotConstant, kWhyThreadLocal},
    {DiagnosticCode::S_ThreadLocalInvalidCombination, kWhyThreadLocal},
    // S_BitInt* (D-CSUBSET-BITINT, C23 6.2.5/6.7.2): the `_BitInt(N)` width gates.
    // UNSUPPRESSABLE — a suppressed width violation would leave the type with no
    // computable / representable width and the masking + layout would silently pick
    // a garbage N (or reach codegen with no multi-limb lowering for the N>64 gate).
    {DiagnosticCode::S_BitIntWidthNotConstant, kWhyBitIntWidth},
    {DiagnosticCode::S_BitIntWidthNotPositive, kWhyBitIntWidth},
    {DiagnosticCode::S_BitIntSignedWidthTooSmall, kWhyBitIntWidth},
    {DiagnosticCode::S_BitIntWidthExceedsMax, kWhyBitIntWidth},
    // S_BitIntWidthAboveC1Limit (the C1 N>64 gate, RETIRED in C2) and
    // S_BitIntWideMulDivUnsupported (the C2 `* / %` boundary, RETIRED in C3
    // 2026-07-12) were DE-LISTED here 2026-08-10. Both surfaces LOWER now —
    // N>64 is a runnable multi-limb type and wide `* / %` lower to schoolbook
    // mul + long-division — so neither code has an emit site, and an unemittable
    // code cannot be suppressed. What still guards the wide-op family is the
    // member below (float<->wide conversion, genuinely unimplemented) plus the
    // WideBitIntMulDivModLowersAtC3 pin asserting nDiag(0xE04F)==0. 0xE025
    // precedent; found by the EveryMemberHasAnEmitSite property below.
    // D-CSUBSET-BITINT-FLOAT-CHAR-ENUM-CONV: float<->wide `_BitInt(N>64)` conversion —
    // deferred (a correct multi-limb FP<->limbs path is a later cycle). UNSUPPRESSABLE:
    // suppressed, the naive scalar path emits the wrong sign + drops the upper limbs
    // (a wide `(_BitInt(128))1.5` / `(double)wide`) and silently miscompiles.
    {DiagnosticCode::S_BitIntWideFloatConvUnsupported, kWhyBitIntFloatConv},
    // S_VlaWithStaticStorage (VLA C1a, D-CSUBSET-VLA, C99/C11 §6.7.6.2): a
    // block-scope static/extern VLA (a VLA needs automatic storage). Suppressed it
    // ships a WRONG type — a runtime-sized `vlaArray` carried into the
    // static-local→hidden-global lowering, whose layout has no static size. Same
    // silent-miscompile-guard class as the S_BitInt* / S_Alignas* constraint
    // entries above. Closed here so an invalid VLA never compiles quietly.
    // ⓘ Its former neighbour S_VlaMultiDimUnsupported (the C1a multi-DIMENSIONAL
    // boundary) was DE-LISTED 2026-08-10: C3 lifted all four of its reject sites,
    // so `int a[n][m]` / `int a[5][n]` / `int a[n][5]` LOWER and RUN (MEASURED
    // rc=0, debug + release + pe64) and the code has no emit site. The multi-LEVEL
    // shape that IS still refused (`typedef int R[5]; R a[n];`) is owned at the MIR
    // tier under H0009, positioned and named. 0xE025 precedent; found by the
    // EveryMemberHasAnEmitSite property below.
    {DiagnosticCode::S_VlaWithStaticStorage, kWhyVlaStorage},
    // S_VlaSizeNotInteger (C11 §6.7.6.2p1): a non-integer VLA size (float / nullptr /
    // pointer). Suppressed, it ships a bogus VLA — a float bound FPToSI-truncates to
    // a garbage element count, a nullptr bound is a silent 0-byte array. Same
    // silent-miscompile-guard class as the S_Vla* siblings above.
    {DiagnosticCode::S_VlaSizeNotInteger, kWhyVlaSize},
    // S_ArrayParamQualifierNonParameter (VLA C4c, D-CSUBSET-VLA, C99 §6.7.6.2/
    // 6.7.6.3): a `static` / cv-qualifier / `*` inside an array declarator's `[ ]`
    // outside a function parameter. Suppressed, it ships the decorated array with
    // the illegal decoration silently dropped (a mis-typed / mis-sized object) —
    // the same silent-miscompile-guard class as the S_Vla* siblings above.
    {DiagnosticCode::S_ArrayParamQualifierNonParameter, kWhyArrayParamQualifier},
    // S_InlineAsmNonEmptyTemplate (FC17.9(i), D-CSUBSET-INLINE-ASM): an `__asm__`
    // statement whose template is not strictly empty (non-empty / whitespace-only /
    // malformed-escape). Cycle-1 emits only the empty-template optimizer barrier;
    // a non-empty template carries real per-target instructions (deferred,
    // D-CSUBSET-INLINE-ASM-TEXT). Suppressed, a dropped `asm("hlt")` would lower to
    // a silent no-op barrier — the instructions vanish, a miscompile. Same silent-
    // miscompile-guard class as the S_Vla* / S_AtomicNonLockFree siblings above.
    {DiagnosticCode::S_InlineAsmNonEmptyTemplate, kWhyInlineAsmTemplate},
    {DiagnosticCode::S_InlineAsmExtendedUnsupported, kWhyInlineAsmExtended},
    {DiagnosticCode::S_InlineAsmLabelSectionRequiresGoto, kWhyInlineAsmLabelSection},
    // Inline-asm P5 operand binding, 0xE065..0xE06B (D-CSUBSET-INLINE-ASM-OPERANDS
    // + D-CSUBSET-INLINE-ASM-TEXT). SEVEN codes, all admitted on
    // PRONG (1), and the prong is met the same way in each: the construct has
    // a SECOND CANDIDATE LOWERING that a silenced compiler would take without
    // saying so — an unbound operand, an alternative the binder chose itself,
    // a full-width register where a narrow view was written, a dropped
    // clobber, a vanished template, a placeholder dropped or emitted raw. In
    // every case the build stays GREEN and the bytes are wrong, which is this
    // table's majority class and exactly what 0xE062 (their P1 ancestor,
    // already a member) was closed against.
    //
    // ⓘ THE ARC'S OTHER CODE IS DELIBERATELY STILL OUT.
    // `S_InlineAsmDuplicateQualifier` (0xE064) is an Error by default and is
    // NOT a member: `volatile volatile` suppressed compiles to what `volatile`
    // means, so there is no second candidate to pick. Eight codes in one arc,
    // seven members, and the odd one out is the same one P1 left out — the
    // criterion is being applied per code rather than per arc.
    //
    // ⚠ MEMBERSHIP HERE IS ABOUT SUPPRESSION ONLY. None of the seven is here
    // for cap-immunity; a refusal aborts the compilation before codegen, so
    // none of them needs `DiagnosticDelivery::Guaranteed` to be seen.
    {DiagnosticCode::S_InlineAsmConstraintLetterUndeclared, kWhyAsmConstraintLetter},
    {DiagnosticCode::S_InlineAsmConstraintUnsupportedForm, kWhyAsmConstraintForm},
    {DiagnosticCode::S_InlineAsmOperandModifierUnsupported, kWhyAsmOperandModifier},
    {DiagnosticCode::S_InlineAsmClobberUnknown, kWhyAsmClobberUnknown},
    {DiagnosticCode::S_InlineAsmTemplateUnparsable, kWhyAsmTemplateUnparsable},
    {DiagnosticCode::S_InlineAsmPlaceholderOutOfRange, kWhyAsmPlaceholderRange},
    {DiagnosticCode::S_InlineAsmPlaceholderInBasicTemplate, kWhyAsmPlaceholderInBasic},
    // S_InlineAsmDuplicateSymbolicName (0xE06C, cycle P20,
    // D-ASM-DUPLICATE-SYMBOLIC-NAME-BINDS-THE-WRONG-OPERAND) — the EIGHTH
    // member of this arc, admitted on the same prong (1) and with the strongest
    // evidence of any of them, because the suppressed behaviour was MEASURED
    // rather than reasoned: before the refusal existed, `[out] "=r"(r),
    // [v] "=r"(d) : [v] "r"(a)` compiled rc=0 at debug AND release through the
    // shipped CLI and returned the OUTPUT's value where the input's was written.
    // ⓘ It is a NAME collision, not a qualifier repeat, so the reasoning that
    // keeps `S_InlineAsmDuplicateQualifier` (0xE064) suppressible does not reach
    // it: `volatile volatile` has one candidate reading, a name used twice has
    // exactly two and the compiler picks one in silence.
    {DiagnosticCode::S_InlineAsmDuplicateSymbolicName, kWhyAsmDuplicateSymbolicName},
    // S_BitfieldMutationUnsupportedBase (D-CSUBSET-BITFIELD-ANON-ARROW-MUTATION-RESIDUAL):
    // a bit-field compound/inc-dec/value mutation whose containing aggregate the
    // read-modify-write reconstruction could not address. Suppressed, the mutation
    // falls to the generic via-ptr path whose full-unit store CLOBBERS packed
    // neighbours + skips truncation — a silent miscompile. Same silent-miscompile-
    // guard class as the S_Vla* / S_InlineAsmNonEmptyTemplate siblings above.
    // ⓘ The anonymous-member and array-arrow bases it once named are SUPPORTED as of
    // 2026-08-31; the code stays as the should-never-fire backstop for every other
    // decline, and stays UNSUPPRESSABLE because what it guards has not changed.
    {DiagnosticCode::S_BitfieldMutationUnsupportedBase, kWhyBitfieldMutation},
    // S_UnknownAttribute / S_DeprecatedSymbolUsed / S_NodiscardResultDiscarded
    // (FC17, D-CSUBSET-ATTRIBUTE-SEMANTICS, C23 6.7.13) are deliberately NOT
    // members — the same suppressible posture as S_UnknownTypeAttribute above.
    // All three are WARNINGS on conforming programs: C23 forbids treating an
    // unknown standard attribute as fatal (an unknown `[[frobnicate]]` is
    // ignorable by definition), and deprecated/nodiscard are lint-tier advice
    // whose suppression ships no wrong bytes and hides no build failure
    // (hasErrors() is untouched by a warning). Forcing any of them
    // unsuppressable would make `--suppress` unable to silence exactly the
    // class of diagnostic the standard defines as ignorable.

    // P_* preprocessor band — the AUTHORED ABORT (D-CPP-ERROR-WARNING, C23
    // 6.10.5). This is the FIRST P_* member of the table, and the break in the
    // D_/F_/H_/I_/K_/L_/R_/A_/S_ family pattern is deliberate, not a stray: every
    // other entry above is a MACHINE-detected invariant (the compiler found
    // something it must not ship), whereas a reached `#error` is the SOURCE
    // AUTHOR's own abort — a constraint they wrote precisely because the
    // configuration being built is one their code cannot correctly build.
    // Suppressing it hides no compiler opinion; it silently BUILDS the
    // configuration the header author declared invalid, and since this reject is
    // the only thing failing that build, it would do so GREEN — the exact
    // ship-a-broken-artifact-green surface this closed table exists to forbid.
    // Membership is cheap on real code because the emit site is REACHABILITY-
    // gated (below the preprocessor's dead-branch gate): an `#error` inside a
    // not-taken `#if` branch — the shape that dominates SDK headers — never
    // emits at all, so it is never suppressed either.
    // P_PreprocessorWarningDirective (C23 6.10.6) is deliberately NOT a member:
    // translation continues, no wrong bytes ship, no build failure is hidden, and
    // `--suppress` must stay able to silence exactly that advisory class — the
    // same posture as S_DeprecatedSymbolUsed / S_UnknownAttribute above.
    // P_PreprocessorDefinedFromExpansion (D-PP-DEFINED-VIA-MACRO-EXPANSION, C
    // 6.10.1) is deliberately NOT a member either, and it is the cleanest case in
    // the file for the "merely hides advice" prong: the construct it reports is
    // one DSS deliberately SUPPORTS — it evaluates the operator and continues, so
    // suppressing the warning changes no answer, ships no bytes and hides no
    // failure. It is also advice about code the author usually CANNOT change:
    // ✔MEASURED, Apple's `secure/_string.h` uses the construct on purpose, so a
    // user compiling that SDK would be handed an unsilenceable warning per
    // translation unit for a header they do not own. That is exactly the
    // A_ImmediateNarrowedToOperandField distinction drawn above — a diagnostic
    // whose remedy is one edit in the line that caused it may be made
    // unsuppressable; one about a line the author cannot edit may not.
    // `--warnings-as-errors` remains the lever for a project that wants the
    // construct out of its own sources.
    // P_PreprocessorIfLiteralImplicitlyUnsigned
    // (D-PP-IF-LARGE-DECIMAL-LITERAL-HAS-NO-WARNING, C 6.10.1p4) is deliberately
    // NOT a member, by the same two prongs: DSS evaluates the `#if` and takes
    // the SAME branch both references take whether or not the warning is shown,
    // so suppressing it ships no wrong bytes and hides no build failure. It also
    // fails the "remedy is one edit in the offending line" test the
    // A_ImmediateNarrowedToOperandField note draws — the literal is routinely in
    // a system header the author does not own (a `SIZE_MAX`-shaped guard), which
    // is exactly why both references make it an ordinary, silenceable warning
    // (clang's is even named: -Wimplicitly-unsigned-literal) rather than an
    // error. `--warnings-as-errors` is the lever for a project that wants it out.
    {DiagnosticCode::P_PreprocessorErrorDirective, kWhyErrorDirective},
    // TF-C82 (D-PP-PRAGMA-REGISTRY): a REACHED pragma DSS does not implement, or
    // one whose operand it cannot honour. Same argument as its `#error` neighbour
    // above, arrived at from the other direction: the author did not write this
    // one as an abort, but the thing it asks for — `#pragma pack(4)` — CHANGES
    // MEMORY LAYOUT, and MEASURED, ignoring it turns `sys/fcntl.h`'s `struct
    // log2phys` from 20 bytes into 24 on a live `fcntl(F_LOG2PHYS)` syscall path.
    // A `--suppress` of this code does not hide a compiler opinion; it re-opens
    // exactly the silent wrong-layout channel this cycle closed, and it would do
    // so GREEN. The RECOGNIZED-and-inert pragmas never reach here at all: they
    // match a `pragmaEffects` row that says, in config the user reads, why
    // ignoring them is true — so membership costs nothing on conforming input.
    // Reachability-gated identically (a pragma in a dead `#if` branch is silent).
    {DiagnosticCode::P_PreprocessorPragma, kWhyPragmaUnhonored},
    // TF-C82: the semantic-tier half of the same guarantee. A composite whose
    // layout key is ambiguous has TWO candidate layouts with different sizes and
    // different offsets; suppressing the refusal does not remove the ambiguity,
    // it just picks one of them without saying so.
    {DiagnosticCode::S_PragmaPackAmbiguous, kWhyPragmaPackAmbiguous},
    // TF-C88 (D-CSUBSET-ASM-LABEL-SYMBOL-RENAME): a label the compiler
    // cannot turn into an
    // assembler name, and a declarator carrying two of them. Both are RENAMES —
    // suppressing either does not restore the intended symbol name, it emits the
    // C-mangled name (or one of two labels) instead, silently. The failure then
    // surfaces as a foreign linker's undefined reference with no line number, or
    // worse: `nameOf` treats an empty name as "module-private" and DROPS the
    // symbol-table row, so the object writer falls back to a synthetic
    // `sym_<id>` and the build stays green all the way to link.
    // S_AsmLabelOnAutomaticVariable is deliberately NOT a member: it is a
    // WARNING about a construct clang also ignores, translation continues, and no
    // wrong symbol ships — the S_DeprecatedSymbolUsed posture.
    {DiagnosticCode::S_AsmLabelInvalid, kWhyAsmLabel},
    {DiagnosticCode::S_AsmLabelDuplicate, kWhyAsmLabel},
    // TF-C86 (D-CSUBSET-STDARG-F001A): a `#define`/`#undef` of a
    // conditional-inclusion OPERATOR name (`__has_include` and siblings).
    // Suppressing it does not make the shadowing harmless — it lets the
    // program's `__has_include(<h>)` answer 0 while `#include <h>` still
    // splices the header, so the guard and the include it guards disagree
    // about the same file. MEASURED, that disagreement is what turned FIVE
    // present-and-readable SDK headers into `F001A: not found` before this
    // cycle (`mach/boolean.h`, `mach/kern_return.h`, `mach/port.h`,
    // `mach/vm_types.h`, `malloc/_malloc_type.h`; the sixth,
    // `mach/mach_types.h`, is blocked by a SEPARATE guarded-include-cycle
    // defect). The universal `#ifndef __has_include` shim never reaches this
    // code (its guard is dead once the operator is `defined`), so membership
    // costs conforming input nothing.
    {DiagnosticCode::P_PreprocessorOperatorNameNotDefinable, kWhyOperatorNameNotDefinable},
    // P36 (D-DIAG-UNSUPPRESSABLE-FAMILY-UNDECIDED): the two codes that row was
    // filed about, admitted on prong (1) by the rule this file already had —
    // with the ELF64 binaries the suppressed builds produced as the evidence.
    // See the block beside `kWhyIncludeError` for the measurement.
    // ★ THE SPLIT THE ROW CALLED "ARBITRARY ON ITS FACE" IS CLOSED: `#pragma`
    // failures were unsuppressable while `#include` failures were not, in one
    // subsystem, for no stated reason. Both are members now, for the same
    // stated reason.
    {DiagnosticCode::P_PreprocessorIncludeError, kWhyIncludeError},
    {DiagnosticCode::P_PreprocessorIncludeReentryRefused, kWhyIncludeReentryRefused},
}};

// Post-fold #11 code-review F1: consteval uniqueness pin matches the
// codebase pattern at `kAbiCatalogTuplesUnique` + `kHeaderReadErrorTable`'s
// row-alignment static_assert. The runtime `ListSelfConsistent` test
// already catches duplicates, but the codebase prefers compile-time
// closed-table invariants where possible — a paste-error duplicate
// becomes a build failure, not a test failure.
consteval bool kUnsuppressableCodesAreUnique() {
    for (std::size_t i = 0; i < kUnsuppressableCodes.size(); ++i) {
        for (std::size_t j = i + 1; j < kUnsuppressableCodes.size(); ++j) {
            if (kUnsuppressableCodes[i].code == kUnsuppressableCodes[j].code) return false;
        }
    }
    return true;
}
static_assert(kUnsuppressableCodesAreUnique(),
              "kUnsuppressableCodes must not contain duplicate entries — "
              "every code appears at most once.");

// FF11 audit (2026-06-05): guard against the "bump the array size but
// forget to add the entry" class — a missing initializer value-inits the
// trailing slot to `DiagnosticCode::None` (0), which would silently make a
// real code suppressible AND make `isUnsuppressable(None)` wrongly true.
// `std::array<DiagnosticCode, N>` accepts a short initializer; this catches
// the resulting `None` slot at COMPILE time (the uniqueness check above does
// not — a single `None` is "unique"). It also rejects an intentional `None`.
consteval bool kUnsuppressableCodesHaveNoNone() {
    for (auto const& e : kUnsuppressableCodes) {
        if (e.code == DiagnosticCode::None) return false;
    }
    return true;
}
static_assert(kUnsuppressableCodesHaveNoNone(),
              "kUnsuppressableCodes must not contain DiagnosticCode::None — a "
              "None slot means the array size was bumped without adding the "
              "intended code.");

// The rationale-as-data promotion's own guard, and it is deliberately the
// SAME SHAPE as the two checks above: a member that explains nothing is
// caught at COMPILE time, not by a test and not by an operator reading a
// diagnostic with an empty tail. It is what makes "every entry carries its
// reason" a property of the table rather than a convention someone has to
// remember — the short-initializer hazard `kUnsuppressableCodesHaveNoNone`
// exists for applies identically here (a row written `{DiagnosticCode::X}`
// value-initializes `why` to an empty view and compiles fine otherwise).
consteval bool kUnsuppressableEntriesAllExplainThemselves() {
    for (auto const& e : kUnsuppressableCodes) {
        if (e.why().empty()) return false;
    }
    return true;
}
static_assert(kUnsuppressableEntriesAllExplainThemselves(),
              "every kUnsuppressableCodes entry must carry a non-empty `why` — "
              "the text is what `D_SuppressRequestIgnored` shows the operator "
              "whose --suppress request this table refuses, so a member "
              "without one refuses silently.");

// ★★★ D-DIAG-UNSUPPRESSABLE-FAMILY-UNDECIDED: THE PRONG'S OWN GUARD, and it is
// deliberately the same shape as the three checks above.
//
// `MembershipProng` has NO zero enumerator, on purpose. That is what makes this
// check bite: a `MembershipReason` written with a short initializer, or a
// future `kWhy*` that forgets the verdict, value-initializes `prong` to 0 —
// which is not a valid enumerator and is caught HERE, at compile time, rather
// than shipping a member whose admitting argument nobody ever stated.
//
// ⚠ THIS IS THE LIMB THE ROW ACTUALLY NEEDED. The row asked for the criterion
// to be WRITTEN; ✔MEASURED, it already was, and had been for cycles. What was
// missing is that nothing ever forced it to be APPLIED: 147 of 166 members were
// admitted by text that cites no prong at all. A criterion in a comment drifts
// exactly the way this registry keeps finding written principles drift — so the
// verdict is a required field, and "I did not think about it" now fails to
// compile instead of passing review.
consteval bool kUnsuppressableEntriesAllStateTheirProng() {
    for (auto const& e : kUnsuppressableCodes) {
        switch (e.prong()) {
            case MembershipProng::WrongArtifactShipsGreen:
            case MembershipProng::BuildFailsWithNothingSaid:
            case MembershipProng::Both:
                continue;
        }
        return false;
    }
    return true;
}
static_assert(kUnsuppressableEntriesAllStateTheirProng(),
              "every kUnsuppressableCodes entry must be admitted under a NAMED "
              "prong of the two-prong membership rule at the top of this file. "
              "A zero/absent prong means a member joined the table without an "
              "argument — the drift this table's own history is a record of.");

} // namespace

bool isUnsuppressable(DiagnosticCode code) noexcept {
    return std::ranges::find(kUnsuppressableCodes, code,
                             &UnsuppressableEntry::code)
         != kUnsuppressableCodes.end();
}

std::span<UnsuppressableEntry const> unsuppressableCodes() noexcept {
    return kUnsuppressableCodes;
}

std::string_view unsuppressableRationale(DiagnosticCode code) noexcept {
    auto const it = std::ranges::find(kUnsuppressableCodes, code,
                                      &UnsuppressableEntry::code);
    return it == kUnsuppressableCodes.end() ? std::string_view{} : it->why();
}

std::optional<MembershipProng>
membershipProngOf(DiagnosticCode code) noexcept {
    auto const it = std::ranges::find(kUnsuppressableCodes, code,
                                      &UnsuppressableEntry::code);
    // ⚠ `std::optional`, NOT a defaulted prong — and the reason is this
    // cluster's whole subject. Returning some prong for a NON-member would be
    // a plausible wrong answer: the caller could not tell "admitted under prong
    // (1)" from "not in the table at all", and every reading would look
    // sensible. That is `diagnosticCodePrefix`'s `letter = 'P'` defect wearing
    // a different hat, in the file whose row is about exactly this. `nullopt`
    // is the only honest answer for a code that has no membership, and it
    // mirrors `unsuppressableRationale`'s empty-view contract.
    //
    // A `None` enumerator would ALSO have been wrong here, and not merely
    // stylistically: `MembershipProng`'s absent zero value is precisely what
    // lets `kUnsuppressableEntriesAllStateTheirProng` catch a member whose
    // verdict was never written. Adding None to serve this function would
    // spend the guard to save an optional.
    return it == kUnsuppressableCodes.end()
               ? std::nullopt
               : std::optional{it->prong()};
}

} // namespace dss
