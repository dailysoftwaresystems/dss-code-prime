#pragma once

#include "analysis/semantic/semantic_model.hpp"  // SemanticModel (CuMirModule member, move-only)
#include "asm/asm.hpp"  // AssembledModule (assembleUnit return + linkAndWrite span)
#include "core/export.hpp"
#include "core/types/data_model.hpp"  // DataModel (CuMirModule member + lowerMergedToAssembly arg)
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"  // ExternImport (CuMirModule member)
#include "core/types/grammar_schema.hpp"
#include "core/types/strong_ids.hpp"  // CompilationUnitId (CuMirModule member)
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"  // TypeInterner (optimizeModule arg)
#include "link/image_request.hpp"  // ImageRequest (linkAndWrite per-program knobs)
#include "link/object_format_schema.hpp"
#include "mir/merge/mir_merge.hpp"  // MergedMirModule (lowerMergedToAssembly arg)
#include "mir/merge/synth_seh_funclets.hpp"  // MirSehScope (c116 D-WIN64-SEH-FUNCLETS)
#include "mir/mir.hpp"  // Mir (CuMirModule member, move-only)
#include "opt/optimizer.hpp"
#include "program/cli_args.hpp"  // CompileConfig

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Driver-tier "one CU → one (target, format) → one artifact"
// pipeline kernel (plan 14 LK10 cycle 2).
//
// Wraps the full HIR → MIR → LIR → ASM → link → writeImage chain
// behind a single substrate entry point. `Program::compileFiles`
// and `Program::compileDirectory` (LK10 cycle 2) plus the future
// CLI router (LK10 cycle 3) reuse this kernel verbatim.
//
// The kernel is source-language-blind (consumes any GrammarSchema),
// target-blind (consumes any TargetSchema), and linker-format-blind
// (consumes any ObjectFormatSchema). All three are passed in by
// reference; the kernel never branches on language name, target
// arch, or format kind.
//
// Output path is fully caller-owned — the kernel does NOT pick
// extensions or output directories. `program.cpp` derives those
// via `TargetSpec::outputExtension(...)` for the v1 convention;
// plan 06 (artifact profiles) will eventually own the policy.

namespace dss {

class CompilationUnit; // fwd-decl — `compile_pipeline.cpp` includes the full header

// ═══════════════════════════════════════════════════════════════════════════
// WHICH ARGUMENTS THE **DRIVER** MUST SUPPLY — and where each one is witnessed
// (D-TEST-STATIC-LINK-UNIT-SUITE-CANNOT-WITNESS-A-DRIVER-THREADING-GAP)
// ═══════════════════════════════════════════════════════════════════════════
//
// ★★★ WHY THIS BLOCK EXISTS. Every entry point below has thorough unit
// coverage, and for a whole class of defect that coverage cannot help: a unit
// case CONSTRUCTS the argument the driver is supposed to derive, so it tests
// the callee while ASSUMING the caller. Cycle P22 measured the consequence —
// dropping a newly-threaded parameter at ONE driver call site reproduced the
// original CLI failure exactly while the entire in-process suite stayed green
// at 32/32. The gap was found by a mutant, not by review, and nothing in the
// suite could have found it.
//
// ── THE CRITERION. A parameter is DRIVER-SUPPLIED when all three hold: ──────
//   1. its correct value is DERIVED by `src/program/program.cpp` — from a CLI
//      argument, from the loaded `.lang`/`.target`/`.format` document, or from
//      earlier pipeline state — rather than handed to the driver;
//   2. a WRONG value COMPILES: a default argument, an empty span/container,
//      `nullopt`, `false`, a zero ordinal, or a same-typed sibling is sitting
//      right there at the call site;
//   3. the wrong value still produces a BUILD. The loss is a dropped
//      capability, a wrong ABI, or a silently different optimizer schedule —
//      never a diagnostic.
// A parameter a UNIT TEST LEGITIMATELY CONSTRUCTS fails at least one clause:
// it is the case's own input (the source text, the schema under test, the
// reporter the test reads back), or a wrong value is a compile error, or a
// wrong value fails loud on its own. Clause 3 decides most rows, because a
// mis-supply that failed loud would already be caught by any test that
// compiles anything.
//
// ★★ AND THE CLASSIFICATION IS PER **ROUTE**, NOT PER ENTRY POINT. The driver
// reaches `linkAndWriteWithStaticArchives` by THREE routes (the `encode` tier,
// the N==1 sole CU, the N>1 merge) and `optimizeModule` by THREE (archive
// member, N==1, merged). Each is a separate argument list a later edit can
// change independently — which is exactly how a parameter ends up threaded to
// two call sites out of three. A pin on one route says NOTHING about the
// others, so every citation below names its route.
//
// ⚠ THIS IS A JUDGEMENT, AND DELIBERATELY NOT A GATE. What a unit test may
// legitimately construct cannot be decided mechanically; a detector would need
// an allowlist that re-states this convention in the place least likely to be
// read — the same ruling that withdrew the `.sh`/`.ps1` pairing anchors. It
// lives here, beside the declarations, and is updated when a signature changes.
//
// ── MEASURED, with the instrument ──────────────────────────────────────────
// ⚠ AND THE INSTRUMENT ITSELF HAS TO SURVIVE BEING QUOTED HERE. The first
// spelling of this paragraph cited `grep -c` for the bare export macro and said
// 21. ✔RE-MEASURED: that command returns 22, because THIS COMMENT contains the
// very token it counts — the citation went stale the moment it landed, inside
// its own cycle, which is the same failure mode as citing a line number. Anchor
// the pattern to the START of a declaration line and no prose can pollute it,
// because every line in this block opens with a slash pair:
//     grep -cE '^(struct |\[\[nodiscard\]\] )?DSS_EXPORT' <this header>  = 21
// — EIGHTEEN exported functions plus three exported structs (`CuMirModule`,
// `EntryCandidate`, `ResolvedEntry`). Driver call-site counts below come from a
// COMMENT-STRIPPED scan of `src/program/program.cpp` — 21 call sites over 13 of
// the 18 entry points. A raw grep over-counts there too: `linkAndWrite`
// "appears" twice and both occurrences are prose.
//
// ── A. NO DRIVER SEAM — `program.cpp` never calls these ────────────────────
// `effectiveLongDoubleFormat`, `compileSingleUnit`, `assembleUnit`,
// `linkAndWrite`, `pullStaticArchiveMembers`. Their callers live in
// `compile_pipeline.cpp`, so the composition is inside one TU and any test that
// builds a CU exercises the supplying. ★ `pullStaticArchiveMembers` is the P22
// case and belongs here for a reason worth keeping: it has no driver seam of
// its OWN — its `dynamicLibraries` value is supplied one level up, by
// `linkAndWriteWithStaticArchives`, which is where the pin had to go.
//
// ── B. DRIVER-SUPPLIED, WITNESSED ──────────────────────────────────────────
// `copyDiagnostics` (3 driver calls: the N>1 pooled per-CU scratch drain, plus
//   the front-half drains of a CU's own diagnostics and of each Tree's) — `dst`
//   is the RUN-WIDE reporter carrying the CLI's diagnostic policy; `src` is a
//   SCRATCH, i.e. earlier pipeline state. Several reporters are in scope and all
//   are the same type, so a drain into the wrong one compiles and the build
//   still succeeds — the diagnostics simply never reach the operator. PINNED by
//   `program/test_compile_pipeline`
//   `Program_CuParallelism.MultiTuDiagnosticsAreCuOrderedPoolVsSynchronous`
//   (the per-CU scratch drain) and `Program_CompileFiles.MalformedSourceRendersPositionedCaret`
//   (the per-Tree drain); `program/test_max_diagnostics_reaches_every_tier`
//   drives the same route end-to-end for the budget axis.
//
// `assembleAsmUnit` (1) — `format` (the WHOLE schema, never a hand-picked
//   slice) and `opts` (the DYNAMIC half of `--resolve-library`, `ar` archives
//   already partitioned out). Its own docblock records that a DEFAULTED `opts`
//   was REJECTED, which is clause 2 written down as a design rule. PINNED by
//   `program/test_asm_extern_binding`
//   (`AsmExternBinding.CallToLibcBindsAndLinksAnExecOnEveryFormat` plus the
//   `--resolve-library` cases).
//
// `buildCuMir` (2: the synchronous N==1 call and the pooled N>1 call) —
//   `callingConventionIndex`, resolved by `dss::ffi::resolveAbi` and turned into
//   an ordinal by pointer distance. ✔MEASURED: `x86_64.target.json` declares
//   `sysv_amd64` at ordinal 0 and `ms_x64` at 1, so the literal `0` compiles and
//   silently emits SysV register assignments on pe64 — `D-FF3-3`, exactly.
//   PINNED by `program/test_entry_argv_run`
//   `EntryArgvRun.RealCommandLineReachesMainByteExact`: a real `CreateProcess`
//   command line into `main(int, char**)`, which reads rcx/rdx under MS_x64 and
//   rdi/rsi under SysV. ⚠ Windows-only, and it takes `compileFiles` — the N==1
//   route. See section D for the merged route.
//
// `optimizeModule` (3) — `stage` is STRUCTURAL knowledge of WHICH call site this
//   is, and nothing downstream can check it; `externImports` is DEFAULTED `= {}`.
//   PINNED per route: archive member by `program/test_static_link`
//   `StaticArchive.ReleaseMemberIsProgramStageOptimized`; N==1 by
//   `program/test_driver_argument_supply`
//   `DriverArgumentSupply.SingleCuRouteRunsTheProgramStageOptimize`; merged by
//   `DriverArgumentSupply.MergedMultiCuRouteRunsTheProgramStageOptimize`. All
//   three count `PhaseTimers::read(Optimize).runs`, which is EXACT rather than a
//   proxy — the phase scope opens INSIDE `optimizeModule`, so the count IS the
//   invocation count. `externImports` is witnessed by the corpus:
//   `examples/c-subset/inline_c99_undefined_error` (N==1) and
//   `inline_c99_inline_definition_crosscu` (merged).
//   ⚠⚠ `Program_WholeProgramMerge.ShippedStageRoutingInlinesCrossCuAtProgramStage`
//   and `Program_WholeProgramMerge.UnitStageRunsTheUnitDocumentNotTheConfigDoc`
//   are NOT driver pins despite the `Program_` prefix: they call `buildCuMir` /
//   `optimizeModule` directly and re-derive `ccIndex` themselves. The prefix
//   names the SUBJECT, not the tier. ✔MEASURED 2026-08-20: with the driver's
//   merged program-stage call short-circuited away, BOTH of them stayed green.
//   Reading them as driver coverage is the mistake this block exists to stop.
//
// `lowerCuMirToAssembly` (2: archive member, N==1) — FIVE format-declared facts
//   read off the schema at the call site. `processArgs` / `sehPersonality` are
//   optionals (a `nullopt` compiles), `entryVerbs` is a span (an empty one
//   compiles AND means "this format starts no program"), `formatName` is a
//   `string_view` (any string compiles). PINNED: `processArgs` by
//   `program/test_entry_argv_run` `EntryArgvRun.RealCommandLineReachesMainByteExact`
//   (a nullopt makes the entry read register garbage); `entryVerbs` on this
//   route by the corpus pair `examples/c-subset/entry_wmain_only_refused_elf` /
//   `entry_wmain_only_refused_macho`.
//
// `resolveProgramEntry` (1, merged route) — `formatVerbs`. ★★★ THE WORST
//   FAILURE SHAPE IN THIS HEADER, AND IT IS SILENT: an EMPTY span is a DECLARED
//   answer, so it takes the "this format starts no program" arm and resolves by
//   NAME with no verb requirement and NO ambiguity check, returning candidate 0.
//   A driver that passed `{}` on an EXEC format would restore the exact
//   first-match-wins defect this function was created to remove. PINNED by
//   `program/test_driver_argument_supply`
//   `DriverArgumentSupply.MergedMultiCuRouteSuppliesTheFormatsEntryVerbs`.
//
// `isArArchiveFile` (1) — the supply risk is the driver not making the CALL, so
//   a `.a` on `--resolve-library` goes to the dynamic export reader. PINNED by
//   `program/test_static_link` `StaticLink.DriverStaticLinkBuildsSelfContainedExec`
//   (`StaticLink.ArMagicDispatchByBytesNotExtension` is the UNIT half — it calls
//   this predicate directly and cannot see the driver skipping it).
//
// `extractStaticArchiveMembers` (1) — `archivePaths`, the STATIC half of the
//   `--resolve-library` partition. An empty span compiles and silently ships an
//   incomplete "fat" library. PINNED by `program/test_static_link`
//   `StaticLink.StaticLibDriverMergesInputStaticArchive` and
//   `StaticLink.StaticLibFatArchiveExecRunsFortyTwo`.
//
// `linkAndWriteWithStaticArchives` (3: `encode` tier, N==1, merged) —
//   `staticArchives`, `dynamicLibraries` and `imageRequest`. `imageRequest` is
//   DEFAULTED, which is clause 2 in its purest form: a route that omits it
//   links, writes the artifact and reports success while the operator's
//   `--stack-reserve` has vanished — at runtime indistinguishable from never
//   having asked. PINNED per route: N==1 by `program/test_artifact_report`
//   `ArtifactReport.ALinkFailureAfterThePathIsKnownReportsNoArtifact`; merged by
//   `program/test_driver_argument_supply`
//   `DriverArgumentSupply.MergedMultiCuRouteSuppliesTheImageRequest`; `encode`
//   tier by `DriverArgumentSupply.AsmEncodeRouteSuppliesTheImageRequest`.
//   `dynamicLibraries` (the P22 parameter — deliberately NOT defaulted, though an
//   empty span still compiles) is pinned on the N==1 route by
//   `program/test_static_link`
//   `StaticLink.PeArchiveMemberBindsAnOperatorNamedLibraryThroughTheDriver`.
//
// `linkAndWriteStaticArchive` (1) — `memberNames` (parallel to `modules`) and
//   `imageRequest`. This route's CONTRACT is the refusal: no archive format
//   declares `stackReserveControl`, so a request routed here must be REFUSED,
//   and "accepting the parameter and ignoring it is what would let the request
//   vanish on a `staticlib` target" (its own docblock). PINNED by
//   `program/test_driver_argument_supply`
//   `DriverArgumentSupply.StaticArchiveRouteSuppliesTheImageRequest`;
//   `memberNames` by `program/test_static_link`
//   `StaticLink.ElfStaticLibDriverEmitsArchiveWithArmap`.
//
// `lowerMergedToAssembly` (1, merged) — NINE driver-derived arguments with an
//   observable consequence, every one of them now pinned on this route by
//   `program/test_driver_argument_supply`. It is the largest concentration in
//   the kernel and the least-travelled route to it, so each pin names the
//   argument it protects:
//     `bitFieldStrategy`         → `DriverArgumentSupply.MergedMultiCuRouteSuppliesTheFormatsBitFieldStrategy`
//     `dataModel`                → `…SuppliesTheFormatsDataModel`
//     `callingConventionIndex`   → `…SuppliesTheCallingConventionIndex`
//     `externCallDispatch`       → `…SuppliesTheFormatsExternCallDispatch`
//     `dataImportBinding`        → `…SuppliesTheFormatsDataImportBinding`
//     `externAddrBinding`        → `…SuppliesTheFormatsExternAddrBinding`
//     `sehScopes`                → `…SuppliesTheSehScopes`
//     `wideFloatSoftcallLibrary` → `…SuppliesTheWideFloatSoftcallLibrary`
//     `tlsAccess`                → `program/test_compile_pipeline`
//                                  `Program_CompileFiles.CrossCuExternThreadLocalMergesAndAccessesTpRelativeE2E`
//   ★★ EVERY ONE OF THOSE PINS ASSERTS THE OBSERVABLE CONSEQUENCE IN THE
//   PRODUCED IMAGE, NEVER THE ARGUMENT'S SHAPE — bytes of a const-init global, a
//   relocation type, an unwind-info flag, a declared library name. Six of the
//   arguments are optionals or containers whose EMPTY value is the CORRECT
//   answer for some shipped format, so "it is non-empty" would be a FALSE
//   assertion on those legs and a tautology on the rest.
//   ✔MEASURED 2026-08-20 by mutating the driver one argument at a time: SIX of
//   the eight mutants produce a SILENT WRONG ARTIFACT (exit 0, zero
//   diagnostics, a binary that builds and — where it can be run — answers
//   wrong: `bitFieldStrategy`, `dataModel`, `callingConventionIndex`,
//   `dataImportBinding`, `externAddrBinding`, `sehScopes`), and TWO fail loud
//   at MIR→LIR (`externCallDispatch`, `wideFloatSoftcallLibrary` — each refuses
//   rather than guessing). In every case the two in-process UNIT suites for the same callee
//   — `lir/test_mir_to_lir` and `mir/test_mir_merge`, which CONSTRUCT these
//   arguments — stayed green. That contrast is the whole point of this block.
//
// ── C. DRIVER-SUPPLIED BUT CLAUSE-2-SAFE ───────────────────────────────────
// `collectEntryCandidates` (1, merged) — two out-parameters and a model that is
//   earlier pipeline state. No confusable same-typed value sits at the call
//   site, so the driver cannot quietly supply the wrong one; the only failure
//   available is walking the wrong CU set, which the merge then reports.
//
// `lowerMergedToAssembly`'s `cuId` — the TENTH driver-derived argument, and the
//   one with no pin, because on this route it has no observable consequence to
//   pin. `lowerMergedToAssembly`'s own docblock says so: the merged image
//   carries CU0's id and it is "cosmetic, since the linker gets one module" —
//   the merge already collapsed every CU into ONE symbol space, so the
//   `(cuId, SymbolId)` key the linker builds has a single cuId whatever value
//   arrives. It fails clause 3. ⚠ That is a statement about THIS route only:
//   on the archive-member route a member must never share a cuId with another
//   (`linkAndWriteStaticArchive`'s symbol index is keyed by the pair), and
//   there the same argument is load-bearing.
//
// ── D. WITNESSED, BUT ONLY WHERE THE IMAGE RUNS — the honest remainder ─────
// No driver-derived argument of any entry point above is UNWITNESSED any more.
// What is left is narrower and worth naming precisely, because a reader who
// assumes "pinned" means "pinned on every leg" would be wrong about two of
// them: their DISCRIMINATING arm needs a pe64 image to EXECUTE, so it runs on
// the Windows leg and is skipped elsewhere. On the other legs those two pins
// still assert the build and their own premise — they simply cannot tell a
// correct supply from a wrong one there.
//   * `callingConventionIndex` on the merged route. ✔MEASURED: every static
//     signature of the wrong ordinal — which register `main` reads `argc` from,
//     which callee-saved registers spill, the `sub rsp` in a calling frame, the
//     UNWIND_INFO save offsets — keys on a REGISTER-ALLOCATION OUTCOME, so a
//     byte pin on any of them would red on honest allocator work. The N==1
//     route's pin (`program/test_entry_argv_run`
//     `EntryArgvRun.RealCommandLineReachesMainByteExact`) is Windows-gated for
//     exactly the same reason; this is its merged twin, not a new compromise.
//   * `dataImportBinding` on the merged route. The consequence is a wrong VALUE
//     at run time (the address materialization stops one indirection short and
//     yields the import slot's own address). ✔MEASURED that the ELF twin does
//     NOT discriminate: a merged program comparing `stdout` against `stderr`
//     still sees two distinct non-null words when both are slot addresses, so it
//     exits 42 either way — which is why the pe64 `_mbcasemap`/`__p__mbcasemap`
//     AGREEMENT ladder is the instrument.
// ⚠ Do not read this as "acceptable" either. It is the remainder, recorded so
// the next reader inherits the measurement instead of re-deriving it — and if a
// host-independent signature for either is ever found that is not a register-
// allocation outcome, it belongs here in place of the run.
//
// ── AND WHAT THE MERGED ROUTE COSTS TO REACH AT ALL ────────────────────────
// ✔MEASURED — by walking `examples/` for directories carrying an
// `expected.json` and counting their `.c`/`.s` files — the merged route is
// reachable ONLY through `Program::compileUnits` with ≥2 sources, and 22 of 613
// shipped corpus example manifests have ≥2 such sources, so the corpus
// exercises it about 3.6% as often as the single-CU route. (An earlier
// spelling of this line said "22 of 614" with no instrument named; the
// numerator reproduces, the denominator does not — hence the instrument.)
// That ratio is why this route accumulated the gaps, and why
// each pin above builds its own 2-CU program rather than reusing a fixture: a
// pin that quietly lost its second source would keep passing while testing the
// sole-CU path instead.
// ═══════════════════════════════════════════════════════════════════════════

// Drain every diagnostic from `src` into `dst`. Shared driver-tier
// helper — used by `program.cpp::compileFiles` (drains CU +
// per-Tree reporters) AND by `compile_pipeline.cpp::compileSingleUnit`
// (drains the SemanticModel's reporter after `analyze`). Hoisted
// out of the program-anon namespace at LK10 cycle 2 post-fold
// review #1 (silent-failure-hunter F9 — eliminates the inline drain
// duplicate that risked future divergence).
//
// **Policy-aware semantics (D-LK10-7 closed at LK10 cycle 3):**
// `dst.report(d)` re-traverses `dst`'s `DiagnosticPolicy` (suppress
// / overrides / warningsAsErrors). With the CLI now wiring
// `--warnings-as-errors` and `--suppress=<code>` through a
// `DiagnosticReporter::Config` to `compileFiles`/`compileDirectory`,
// the policy ALSO applies to per-tier drains coming through
// `copyDiagnostics`. That's the intended shape — the user's
// `--suppress` applies uniformly across the front-half and back-half
// of the pipeline. (D-LK10-7's `copyDiagnosticsRaw` alternative
// becomes unnecessary; this routing IS the design.)
DSS_EXPORT void copyDiagnostics(DiagnosticReporter const& src,
                                 DiagnosticReporter&       dst);

// D-CSUBSET-BITFIELD-ABI-EXACT: resolve the effective per-ABI bit-field packing
// strategy for a (target, format) pair. C bit-field allocation is FORMAT/OS-
// determined (one CPU target — x86_64 — serves BOTH ELF-SysV `gnu_packed` and
// PE-MS `msvc_straddle`), so the FORMAT's declared strategy WINS; when the format
// declares none (`None`), it falls back to the TARGET's
// `aggregateLayout().bitFieldStrategy` (back-compat for targets that predate the
// format-side field). This is the ONE resolution chokepoint the driver overlays
// onto the target's `AggregateLayoutParams` before threading them into the layout
// engine at all three consumer sites (analyze / HIR→MIR / asm globals). It selects
// purely on config (the declared enum), NEVER on a target/format name.
[[nodiscard]] DSS_EXPORT BitFieldStrategy
effectiveBitFieldStrategy(TargetSchema const&       target,
                          ObjectFormatSchema const& format) noexcept;

// FC17.9(e) (D-CSUBSET-LONG-DOUBLE): resolve the effective `long double` axis
// for a (target, format) pair — the effectiveBitFieldStrategy twin. The
// representation is FORMAT/OS-determined (one x86_64 target serves BOTH pe64's
// 64-bit-IEEE and ELF-SysV's x87 80-bit `long double`), and UNLIKE
// bitFieldStrategy no target-side fallback field exists — the axis is
// format-only, so `None` means genuinely undeclared (wasm/spirv skeletons):
// the semantic bind then leaves `long double` rows unrealized
// (S_LongDoubleFormatUndeclared on use), never a silent width guess. The
// (target, format) signature keeps the resolver-family shape so a future
// target-side contribution slots in without touching the call sites.
[[nodiscard]] DSS_EXPORT LongDoubleFormat
effectiveLongDoubleFormat(TargetSchema const&       target,
                          ObjectFormatSchema const& format) noexcept;

// ★ NO `effectiveCharIsUnsigned` HERE — and its absence is the design, not an
// omission (D-TARGET-CHAR-SIGNEDNESS-PER-PLATFORM, TF-C75). The two resolvers
// above exist because their axes have contributions from BOTH schemas that must
// be reconciled. Bare-`char` signedness has exactly ONE contributor: the TARGET
// declares the whole (processor × platform) fact in its single `charIsUnsigned`
// key (`{"default": …, "byObjectFormat": {…}}`), and resolution is
// `TargetSchema::charIsUnsigned(ObjectFormatKind)` — a method on the owner, with
// the format KIND as a REQUIRED argument so no caller can silently take the
// processor half alone. A wrapper here would be a second place the fact is
// "about", which is precisely the duplication this reshape removed. The result
// still lands in `MirLoweringConfig.charIsUnsigned`, whose sole reader is
// `isSignedIntKind(TypeKind::Char)` — the single SExt-vs-ZExt decision for the
// char→int promotion.

// Compile a single CompilationUnit through the full HIR→write
// pipeline for one (target, format) pair. Returns true iff every
// tier succeeded AND `writeImage` committed bytes to disk.
//
// Failure modes (all fail loud via `reporter`):
//   * Any tier emits an error (H_/I_/L_/R_/A_/K_) — the kernel
//     halts at the failing tier and returns false.
//   * `dss::linker::link` produces a non-`ok()` image.
//   * `dss::linker::writeImage` returns false (parent missing,
//     open failed, short write, ...).
//
// The caller is responsible for:
//   * Constructing the CompilationUnit (parse, addFile/addInMemory).
//   * Computing the output path (extension + directory).
//   * Creating any required parent directories before calling — the
//     kernel does NOT `create_directories` (mirrors the writer
//     substrate's contract; auto-mkdir would mask config bugs).
// `callingConventionIndex` is the per-(target, format) cc ordinal
// resolved by `dss::ffi::resolveAbi` in the driver before reaching
// this kernel. Threaded through to the LIR allocator so prologue/
// epilogue emission picks the correct cc table row. Pre-D-FF3-3
// every compile silently used index 0 — a real miscompile on
// non-default-cc targets (PE64 + x86_64 silently emitted SysV
// register assignments instead of MS_x64).
// D-OPT-COMPILE-OPTIONS-STRUCT: consolidates the trailing-nullable
// parameters that were individually positional. Adding future knobs
// (emitDebugInfo, ltoMode, inlineThreshold, ...) is a zero-signature-
// churn struct-field addition.
struct CompileOptions {
    // ★ REQUIRED, hence the explicit constructor and the deleted default one:
    // this is how the operator's `--max-diagnostics` value reaches the SEMANTIC
    // tier. `compileOneTarget`'s `reporter` parameter cannot serve -- it is the
    // per-target SCRATCH, deliberately relaxed to SIZE_MAX for the merge, so a
    // budget read from it would give the semantic model no bound at all (that is
    // design (A), which the operator declined). The budget must therefore travel
    // beside the reporter, from `rep`, not be derived from it
    // (D-DIAG-VOLUME-CAP-ENFORCED-AT-SIX-STAGES-NOT-ONCE).
    //
    // Making it a required CONSTRUCTOR argument rather than a defaulted field is
    // the point: `CompileOptions opts;` no longer compiles, so the three pipeline
    // entry points that used to say `CompileOptions const& opts = {}` had to be
    // updated deliberately instead of silently reinstating the library default.
    explicit CompileOptions(DiagnosticBudget b) noexcept : diagBudget(b) {}
    CompileOptions() = delete;

    DiagnosticBudget diagBudget;

    // Selects the default optimizer pipeline when `pipelineOverride`
    // is null. Resolved via `resolvePipelineName` (a constexpr table
    // indexed by ordinal — NO `if (config == Release)` branches per
    // D-OPT1-PIPELINE-CONFIG-FROM-COMPILECONFIG agnosticism contract).
    CompileConfig config = CompileConfig::Debug;

    // Non-null: bypasses the JSON registry; used by the examples_runner's
    // differential-verify arm + MIR unit tests (D-OPT1-DIFFERENTIAL-
    // VERIFY-RUNNER).
    ::dss::opt::OptPipeline const* pipelineOverride = nullptr;

    // c162 (D-FF1-READER-CONSUMER): the `--resolve-library <path>` driver
    // surface. Each entry is a real binary (a `.so` / `.dll` / `.dylib`,
    // typically a DSS-BUILT library) whose export surface is READ (via the
    // FF1 binary reader) and used to resolve this build's source-declared
    // externs. A DSS-built library has NO shipped JSON descriptor, so
    // reading its real export table is the ONLY way to link against it --
    // a genuine, NON-DUPLICATIVE capability (unlike the shipped-lib JSON
    // path). When non-empty, compile_pipeline step 2.5 routes every
    // source-declared extern that carries no explicit per-symbol library
    // override (and is not a bare no-library-binding reference) through the
    // live `ingest()` consumer against these binaries: a match binds the
    // extern to the library (validated PRESENT -- fail loud otherwise);
    // externs with an explicit override / shipped-descriptor binding stay
    // on the trusting `synthesizeFfiFromSourceDecls` path. Empty (the
    // default) ⇒ every build before c162 is byte-identical (synthesize over
    // all externs). See the step-2.5 precedence docblock.
    //
    // D-FFI-DECLARED-IMPORT-NAME: an entry may additionally STATE the runtime
    // identity to RECORD for the symbols read out of it, which the pipeline
    // hands to `ffi::BinaryLibrarySource::declaredImportName` — the top of the
    // three-level recorded-identity precedence (`src/ffi/ingest.hpp`). Empty
    // (the default) leaves that precedence exactly as it was.
    std::vector<ResolveLibrarySpec> resolveLibraries;
};

// Resolve `CompileConfig` to a shipped pipeline name. Uses a
// constexpr table of {ordinal, name} pairs — adding a new
// CompileConfig enumerator without extending the table fails the
// static_assert below at compile time. Mirrors the kPassNameTable
// precedent in optimizer.hpp INCLUDING the "in-order" check that
// catches a row-swap (e.g. accidentally putting "release" first):
// without the swap guard, a future edit could silently flip
// Debug→release and Release→debug at zero compile cost.
inline constexpr std::size_t kCompileConfigCount = 2;
inline constexpr std::pair<CompileConfig, std::string_view>
kPipelineNameTable[kCompileConfigCount] = {
    {CompileConfig::Debug,   "debug"},
    {CompileConfig::Release, "release"},
};
static_assert(kCompileConfigCount ==
              static_cast<std::size_t>(CompileConfig::Release) + 1,
              "CompileConfig / kPipelineNameTable drift — add a row to "
              "kPipelineNameTable when a new CompileConfig enumerator "
              "lands (D-OPT1-PIPELINE-CONFIG-FROM-COMPILECONFIG).");

[[nodiscard]] constexpr bool kPipelineNameTableInOrder() noexcept {
    for (std::size_t i = 0; i < kCompileConfigCount; ++i) {
        if (static_cast<std::size_t>(kPipelineNameTable[i].first) != i) {
            return false;
        }
    }
    return true;
}
static_assert(kPipelineNameTableInOrder(),
              "kPipelineNameTable entries must appear in CompileConfig "
              "ordinal order — a row-swap would silently flip the "
              "Debug↔Release mapping.");

// Returns nullopt on an out-of-range `CompileConfig` ordinal (e.g.
// `static_cast<CompileConfig>(99)` produced by a buggy CLI parser
// or a future enumerator added without a table row). Callers
// fail loud — silent fallback to "debug" would let a release build
// silently degrade to debug-pipeline.
[[nodiscard]] constexpr std::optional<std::string_view>
resolvePipelineName(CompileConfig config) noexcept {
    auto const idx = static_cast<std::size_t>(config);
    if (idx >= kCompileConfigCount) return std::nullopt;
    return kPipelineNameTable[idx].second;
}

// The REVERSE lookup over the SAME table (P10: the examples runner's
// `shippedPipeline: <name>` arm resolves a pipeline NAME back to the
// CompileConfig that selects it, so that arm exercises the REAL production
// channel — config → resolvePipelineName → loadShippedPipeline → stage
// routing — instead of injecting a `pipelineOverride`). One table, both
// directions: a second mapping would drift. Nullopt on an unknown name —
// callers fail loud (an unknown name in a manifest is a config error, never
// a silent debug fallback).
[[nodiscard]] constexpr std::optional<CompileConfig>
resolveCompileConfigFromPipelineName(std::string_view name) noexcept {
    for (auto const& [cfg, pipeline] : kPipelineNameTable) {
        if (pipeline == name) return cfg;
    }
    return std::nullopt;
}

// Assemble ONE standalone-assembly CompilationUnit at the `encode` pipeline
// tier (plan 29 P4) — the `assembleUnit` sibling for a language whose root rule
// declares `{"tier": "encode"}`.
//
// ★★★ WHAT MAKES IT A SEPARATE ENTRY POINT RATHER THAN A FLAG ON `assembleUnit`:
// it shares NO tier with it. A `.s` is already the machine tier, so this runs
// text→LIR and then `assemble()`, and everything between — analyze, CST→HIR,
// HIR→MIR, the optimizer, MIR→LIR, liveness, regalloc, two-address legalize,
// callconv — does not run at all. Three of those would rewrite the programmer's
// own decisions SILENTLY (registers, instruction forms, and the prologue), so
// the two paths are different pipelines and not one pipeline with a switch.
//
// A defined label whose name is one of the language's entry spellings becomes
// the module's `userEntrySymbol`. Returns nullopt on any failure (diagnostics
// emitted via `reporter`).
//
// ★★ IT TAKES THE WHOLE `ObjectFormatSchema` AND THE WHOLE `CompileOptions`,
// NOT A HAND-PICKED SLICE (D-ASM-EXTERN-CALL-CANNOT-BIND-A-LIBRARY +
// D-ASM-RESOLVE-LIBRARY-SILENTLY-IGNORED-ON-ENCODE-TIER). It used to take only
// `formatVerbs` (the format's `entryVerbs()`), which was every fact the entry
// election needed and NOTHING the extern binder needs: the platform-realization
// oracle keys on the active object format KIND + DATA MODEL, and honoring
// `--resolve-library` needs `opts`. A `.s` calling `putchar` therefore could not
// bind ANY library and was refused at the EXEC link with `undefined symbol`,
// while the exact flag that should have bound it was accepted and dropped
// without a word. Both facts now arrive by the same route `assembleUnit` uses,
// so the two tiers cannot drift on what "the active format" means.
// ⚠ A DEFAULTED PARAMETER WAS REJECTED for `opts`: it would compile everywhere
// while the shipped driver silently never passed one — precisely the shape of
// the defect being closed.
[[nodiscard]] DSS_EXPORT std::optional<AssembledModule>
assembleAsmUnit(CompilationUnit const&                cu,
                GrammarSchema const&                  grammar,
                TargetSchema const&                   target,
                ObjectFormatSchema const&             format,
                DiagnosticReporter&                   reporter,
                CompileOptions const&                 opts);

[[nodiscard]] DSS_EXPORT bool
compileSingleUnit(CompilationUnit const&         cu,
                  GrammarSchema const&           grammar,
                  TargetSchema const&            target,
                  ObjectFormatSchema const&      format,
                  std::uint16_t                  callingConventionIndex,
                  std::filesystem::path const&   outPath,
                  DiagnosticReporter&            reporter,
                  CompileOptions const&          opts);

// Assemble ONE CompilationUnit to its `AssembledModule` (the per-CU half of
// `compileSingleUnit` — no link, no write). Returns nullopt on any tier failure
// (diagnostics emitted via `reporter`). The multi-CU driver (CU6) calls this per
// CU, collects the N modules, then `linkAndWrite`s them into one merged image.
//
// Implemented as `buildCuMir(...)` composed with `lowerCuMirToAssembly(...)` — the
// two halves below. The single-CU output is byte-identical to the former monolithic
// `buildAssembledModule`; the split exists so the multi-CU driver can build EVERY
// CU's MIR (loop 1) before lowering any (loop 2) — the prerequisite shape for a
// future whole-program MIR merge (cycle 25). Most callers use this composed entry.
[[nodiscard]] DSS_EXPORT std::optional<AssembledModule>
assembleUnit(CompilationUnit const&         cu,
             GrammarSchema const&           grammar,
             TargetSchema const&            target,
             ObjectFormatSchema const&      format,
             std::uint16_t                  callingConventionIndex,
             DiagnosticReporter&            reporter,
             CompileOptions const&          opts);

// ── assembleUnit's two halves (Cycle 24) ──────────────────────────────────────
//
// The verified MIR/LIR seam: everything through the optimizer is the BUILD half
// (`buildCuMir`); MIR→LIR onward is the LOWER half (`lowerCuMirToAssembly`). The
// `CuMirModule` carries every piece of state the lower half reads across the seam —
// crucially the move-only `SemanticModel`, whose `TypeLattice` owns the type
// interner that MIR→LIR + the optimizer + the symbol-table populate all consume.
// Holding the model BY VALUE keeps the interner alive after MIR-build returns, so
// loop 1 can build all CUs' MIR and loop 2 can lower them later.
//
// Move-only (the `Mir` + `SemanticModel` members are both move-only). The
// `grammar` / `target` references are non-owning POINTERS into the caller's
// schemas, which outlive both loops (owned by `compileOneTarget`).
struct DSS_EXPORT CuMirModule {
    Mir                       mir;             // the optimized module
    SemanticModel             model;           // MOVED in — owns the interner past MIR-build
    std::vector<ExternImport> externImports;   // MOVED into lowerToLir by the lower half
    CompilationUnitId         cuId{};          // == cu.id(); stamped onto the AssembledModule

    // Non-owning back-references the lower half reads. The caller's GrammarSchema /
    // TargetSchema outlive the CuMirModule (they live across both driver loops).
    GrammarSchema const*      grammar = nullptr;  // entry-name list + (unused-by-lower) policy
    TargetSchema const*       target  = nullptr;  // MIR→LIR + assemble target
    std::uint16_t             callingConventionIndex = 0;
    // D-FFI-EXTERN-CALL-DISPATCH: the active object format's extern-call
    // shape (indirect-slot / direct-plt), captured at build time from the
    // `ObjectFormatSchema` so the LOWER half (which sees only this struct)
    // selects the right call-site opcode. nullopt iff the format declared
    // none — MIR→LIR then fails loud on any extern call.
    std::optional<ExternCallDispatch> externCallDispatch;
    // D-LK-EXTERN-DATA-IMPORT (c117): the active object format's extern-DATA
    // binding model (got-indirect / copy-relocation), captured here for the
    // SAME reason as `externCallDispatch` — the LOWER half (which sees only
    // this struct) selects how a GlobalAddr of an extern-DATA object (libc
    // stdout) materializes its address (got-indirect → lea-of-slot + deref).
    // nullopt iff the format declared none (data imports fail loud at link).
    std::optional<DataImportBinding> dataImportBinding;
    // D-LK-ARM64-EXTERN-DATA-ADDR-PIE-GOT (TF-C52): the active object
    // format's extern-ADDRESS materialization binding (`got`), captured
    // here for the SAME reason as `dataImportBinding` — the LOWER half's
    // MIR→LIR GlobalAddr value-form arm routes an `&extern` VALUE through
    // the arm64 GOT-address macro from it. nullopt iff the format declared
    // none (the ordinary lea — foreign-PIE-safe only for a DSS exec /
    // x86_64).
    std::optional<ExternAddrBinding> externAddrBinding;
    // TLS C1 (D-CSUBSET-THREAD-LOCAL): the active object format's
    // thread-local access block, captured here for the SAME reason as
    // `dataImportBinding` — the LOWER half's MIR→LIR GlobalAddr lowering
    // selects the TLS access sequence (local-exec → tlsbase + tpoff lea)
    // from it. nullopt iff the format declared none (a thread-local access
    // then fails loud K_FormatLacksThreadLocalSupport — never a silent
    // process-shared alias).
    std::optional<TlsAccessInfo> tlsAccess;
    // D-LK4-RODATA-PRODUCER-AGGREGATE-GLOBAL: the active format's data model
    // (pointer width), captured here for the SAME reason as `externCallDispatch`
    // — the LOWER half sees only this struct, and the aggregate-global rodata
    // encoder needs it (with the target's `aggregateLayout`) to compute byte
    // layout. The target's alignment params are read from `*target` directly.
    DataModel dataModel{};
    // D-CSUBSET-BITFIELD-ABI-EXACT: the FORMAT-resolved bit-field packing strategy
    // (`effectiveBitFieldStrategy(target, format)`), captured here for the SAME
    // reason as `dataModel` — the LOWER half sees only this struct, and the
    // aggregate-global rodata encoder must lay out a bit-field global byte-ABI-
    // exact for the active format. The target's alignment params come from
    // `*target`; this overlays the per-format bit-field rule onto them.
    BitFieldStrategy bitFieldStrategy = BitFieldStrategy::None;
    // FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER) + D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3):
    // shim SymbolId.v → recipe id for EVERY `synthesize`-tagged shipped-library symbol this
    // CU touched, carried from the BUILD half's `CstToHirResult.synthRecipeBySymbol` so the
    // LOWER half can supply each shim function's definition. ONE map, MULTIPLE families —
    // pe64 <threads.h> (mtx_lock…) and pe64 <stdio.h> (printf…) both land here; the LOWER
    // half partitions it by `dss::ffi::shimFamilyOf` and hands each part to its own synth
    // pass. Named for the MECHANISM (a library shim) rather than for one family, because
    // the name `threadsRecipes` became a lie the moment the stdio family joined — a field
    // whose name contradicts its contents is how the wrong pass ends up reading it.
    // Empty for every elf/macho + every pe TU that touches neither family (the
    // overwhelming majority — a bare default-constructed map).
    std::unordered_map<std::uint32_t, std::string> libraryShimRecipes;
    // D-CSUBSET-C11-THREADS-MACHO: the active format's shipped-library synth vehicle
    // (win32 / pthread + import library), captured here for the SAME reason as
    // `tlsAccess` — the LOWER half sees only this struct, and `synthesizeThreadsShim`
    // reads it to pick the primitive family it emits over. nullopt on elf (direct FFI →
    // `libraryShimRecipes` is empty there anyway); a non-empty THREADS partition with a
    // nullopt vehicle is a fail-loud in the synth pass (never a silently-assumed vehicle).
    std::optional<LibrarySynthesis> librarySynthesis;
    // D-FFI-CMANGLING-RULE-NOT-CONFIG-DRIVEN (step C4): the active format's DECLARED
    // C-symbol decoration scheme, captured here so the LOWER half can C-mangle names for
    // the format — `synthesizeThreadsShim`'s native helper imports and `nameOf`'s
    // definition merge-keys — via the SAME `applyCMangling`/`linkNameFor` the FFI ingest
    // uses.
    //
    // ★ THIS FIELD USED TO BE `ObjectFormatKind objectFormat`, i.e. the format's
    // IDENTITY, and the change is the point rather than a rename. The mangling rule now
    // arrives as a declared VERB read out of `.format.json`, so the LOWER half is handed
    // the ANSWER and never the question — it cannot ask "which format is this?" because
    // the struct no longer carries it. The `Unspecified` zero stays an INVALID sentinel
    // exactly as the old `Unknown` did (same discipline as `librarySynthesis` keeping
    // "absent" representable): every loaded schema carries a real scheme, because
    // `ObjectFormatData::validate()` requires one on every format, unconditionally.
    CSymbolDecorationScheme cSymbolDecoration = CSymbolDecorationScheme::Unspecified;
    // D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3): the RESOLVED calling convention's WHOLE
    // `vaListLayout` block, captured here for the SAME reason as `librarySynthesis`/
    // `objectFormat` — the LOWER half sees only this struct, and `synthesizeStdioShim`'s
    // variadic-forwarding arm (a printf-family shim forwards its caller's va_list into the
    // UCRT `__stdio_common_v*` core) needs the target's va_list model to pick the right MIR
    // leaf, or to fail loud on a model it has no arm for.
    //
    // ★ THE WHOLE BLOCK, NOT JUST `.strategy` — and that is a CORRECTNESS requirement, not
    // tidiness. This field used to be a bare `std::optional<VaListStrategy>`, which was
    // enough only for as long as `HomogeneousPointer` meant Win64. It does not:
    // `arm64.target.json`'s `apple_arm64` CC declares `homogeneous_pointer` WITH
    // `variadicUsesOverflowBase: true`, and that second field is what decides between
    // `VaHomeArgAreaAddr` and `VaOverflowArgAreaAddr` in `hir_to_mir`'s real `va_start`
    // lowering. Passing the strategy alone dropped it on the floor, so the shim could only
    // ever emit the home leaf — silently pointing `ap` at named-arg storage on any
    // overflow-base target. Threading the layout WHOLE means the shim and the real lowering
    // read the same field off the same struct: one source of truth, no rule to drift.
    //
    // ★ OPTIONAL, DEFAULTED EMPTY — deliberately, and for the same reason its two siblings
    // above keep "absent" representable (`librarySynthesis` is an optional; `objectFormat`
    // defaults to an `Unknown` SENTINEL, not to a real format). A default-constructed
    // `VaListLayout` is a REAL one (its `strategy` defaults to SysVRegisterSave for
    // pre-FC12b back-compat), so defaulting rather than emptying would make "the resolved
    // CC declares no `vaListLayout`" indistinguishable downstream from "the resolved CC
    // declares SysVRegisterSave": a config gap would then arrive at the synth pass wearing a
    // legitimate model's face and be refused (or, one day, ACCEPTED) for the wrong reason,
    // with a diagnostic pointing at the wrong end of the pipeline. `synthesizeStdioShim`
    // fails loud on nullopt. Consulted only when a stdio recipe actually appears — an empty
    // recipe map is a clean no-op either way.
    std::optional<VaListLayout> vaListLayout;
};

// BUILD half: semantic analysis → HIR → FFI synthesis → MIR → optimize. Returns the
// `CuMirModule` carrying the optimized MIR + the SemanticModel (interner owner) +
// extern imports + the cuId/schema refs the lower half needs. Returns nullopt on any
// front-half tier failure (diagnostics emitted via `reporter`).
[[nodiscard]] DSS_EXPORT std::optional<CuMirModule>
buildCuMir(CompilationUnit const&         cu,
           GrammarSchema const&           grammar,
           TargetSchema const&            target,
           ObjectFormatSchema const&      format,
           std::uint16_t                  callingConventionIndex,
           DiagnosticReporter&            reporter,
           CompileOptions const&          opts);

// Run the configured optimizer pipeline over `mir` in place. Resolves the pipeline
// the same way `buildCuMir` always did: an explicit `opts.pipelineOverride` (the
// examples_runner's differential-verify arm + unit tests) takes precedence, else the
// shipped JSON pipeline named by `resolvePipelineName(opts.config)` (Debug→"debug",
// Release→"release"). Runs `opt::optimize(mir, target, interner, pipeline, reporter)`
// and returns `optResult.ok && tierClean(reporter, entryCount)`. Fails loud on an
// out-of-range CompileConfig ordinal (`X_PipelineNameResolutionFailed`) or a pipeline
// load failure (config diagnostics drained) — both return false.
//
// Extracted from `buildCuMir` (Cycle 26) so the N>1 whole-program path
// (`Program::compileOneTarget`) can run the SAME pipeline resolution + optimize over
// the MERGED module — where cross-CU calls (made intra-module DIRECT by the cycle-25
// merge) become inline-eligible (`D-OPT7-1`). Pure code-motion: the per-CU output is
// byte-identical (`buildCuMir` calls this with the same arguments it used inline).
// `interner` is the type space the module's TypeIds index into — the per-CU lattice's
// interner for `buildCuMir`, the merged host lattice's interner for the merged path.
//
// ── P10 stage topology (D-OPT7-CROSSCU-LTO-SINGLE-OPTIMIZE, operator ruling
// 2026-08-18: "first class LTO implementation, 100% config driven") ──
// The DRIVER owns the slots (which call sites exist is structural knowledge);
// the DOCUMENT owns what runs in each. `Unit` runs wherever a CU is built
// (buildCuMir); `Program` runs wherever a FINAL module is about to be lowered
// — the N>1 merged module, the N==1 sole CU, and each static-archive member.
// WHICH pipeline a stage runs: `opts.pipelineOverride` wins at every site
// (unchanged semantics); else the config's document, and — at the Unit stage
// only — if that document declares a top-level `"unitPipeline": "<name>"`,
// THAT named pipeline runs instead. Absent key ⇒ same document at both
// stages (the pre-P10 behavior, and what `debug` ships). N==1 needs no
// special case: its buildCuMir call is a Unit site, its pre-lower call is a
// Program site, and both fall out of the config — never a driver `if` on
// stage content.
enum class PipelineStage : std::uint8_t {
    Unit = 0,     // per-CU build (buildCuMir)
    Program = 1,  // final module pre-lower (merged / N==1 / archive member)
};

[[nodiscard]] DSS_EXPORT bool
optimizeModule(Mir&                  mir,
               TargetSchema const&   target,
               TypeInterner const&   interner,
               CompileOptions const& opts,
               PipelineStage         stage,
               DiagnosticReporter&   reporter,
               // D-CSUBSET-INLINE-FUNCTION-NO-EXTERNAL-DEFINITION-EMITTED: the
               // module's extern table. `opt::optimize`'s unconditional strip
               // epilogue reads it to find the C99 6.7.4p7 inline definitions
               // that must not be emitted (a function whose SymbolId is also an
               // extern's). BOTH call sites pass their own module's table —
               // per-CU here, whole-program in `Program::compileOneTarget` —
               // because a body surviving either optimize reaches codegen.
               std::span<ExternImport const> externImports = {});

// LOWER half: MIR → LIR → liveness → regalloc → rewrite → legalize → callconv →
// assemble → symbol-table populate → user-entry scan. Consumes the `CuMirModule`
// (its `externImports` are MOVED into MIR→LIR; its `mir` + `model` are read). Returns
// nullopt on any back-half tier failure (diagnostics emitted via `reporter`).
//
// UCRT-P4 (was c111): the four format-declared blocks the entry spine reads, passed
// as values rather than as an `ObjectFormatSchema&` so this driver half keeps its
// existing dependency shape (the same reason `librarySynthesis` /
// `cSymbolDecoration` / `vaListLayout` already arrive through `CuMirModule`):
//   * `processArgs`    — the program-entry argument MECHANISM (nullopt when the
//                        format declares none, which is correct on Mach-O: dyld
//                        delivers argc/argv in the argument registers).
//   * `entryVerbs`     — the program-entry materialization VERBS this format
//                        realizes. Intersected with the language's declared entry
//                        rows to SELECT the program entry; non-empty on every exec
//                        format and empty on every other (both load-enforced), so
//                        emptiness is also the "this build needs no entry"
//                        predicate.
//   * `sehPersonality` — the declared unwinder-personality routine + its image;
//                        `synthesizeSehFunclets` fails loud on a resolved SEH region
//                        when this is nullopt.
//   * `formatName`     — names the format in those two diagnostics, so the reader
//                        knows WHICH `.format.json` to open.
// After the user entry is resolved this drives `realizeEntryShape` — the single-CU
// counterpart of the merge-path pass in `program.cpp` — which materializes the
// entry's arguments per the resolved verb × the format's declared mechanism.
[[nodiscard]] DSS_EXPORT std::optional<AssembledModule>
lowerCuMirToAssembly(CuMirModule&                       cuMir,
                     std::optional<ProcessArgs> const& processArgs,
                     std::span<EntryMaterialization const> entryVerbs,
                     std::optional<SehPersonality> const& sehPersonality,
                     std::string_view                  formatName,
                     ObjectFormatKind                  fmtKind,
                     DiagnosticReporter&               reporter);

// ── D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE: PROGRAM-ENTRY RESOLUTION ───────────────
//
// ★★★ THE SINGLE OWNER of "which function is this program's entry". Both driver
// paths call it — the single-CU/archive path from `lowerCuMirToAssembly` and the
// N>1 merged path from `Program::compileOneTarget` — because a program has ONE
// entry and the rule for picking it must not exist twice. Before this existed the
// two paths disagreed in a way nothing caught: the single-CU scan refused
// ambiguity while the merge's scan took the FIRST name it happened to walk past,
// so `main` in a.c and `wmain` in b.c silently picked one.
//
// A candidate is a function DEFINITION whose name the language declares as a
// program-entry spelling and whose signature matched one of that name's declared
// rows — the semantic tier already decided that and recorded the row's VERB, so
// this function classifies nothing. What it adds is the FORMAT half: a candidate
// is REALIZABLE only if `formatVerbs` contains its verb. That intersection is the
// whole mechanism by which `wmain` is a program entry on Windows and an ordinary
// function everywhere else, with no format-identity branch.
//
// Outcomes, all fail-loud except the first:
//   * exactly one realizable candidate → returned.
//   * `formatVerbs` EMPTY → this format starts no program (a relocatable `.o` /
//     staticlib / library image). Entry selection is then by NAME only, with no
//     verb and no requirement, preserving the behaviour object-file builds have
//     always had: `main` is resolved if present and is otherwise just a function.
//   * zero realizable candidates on an exec format → `K_ProgramEntryUndefined`,
//     naming the NEAR-MISSES (a defined entry name whose verb this format cannot
//     realize) because that is the only thing that explains WHY.
//   * more than one → `K_ProgramEntryAmbiguous`. Never first-match-wins: a
//     silently chosen program entry is the worst outcome available here.
struct DSS_EXPORT EntryCandidate {
    // ★★ OWNING, NOT A `string_view` INTO THE MODEL, AND THIS COST IS BOUGHT
    // DELIBERATELY. It was a `string_view` aliasing `SymbolRecord::name`, and that
    // ALREADY caused a MEASURED silent wrong-answer: `collectEntryCandidates` wrote
    // `auto const recs = model.symbols();`, and because that accessor returns
    // `std::vector<SymbolRecord> const&` while `auto const` deduces a VALUE, the
    // records were COPIED and every view dangled the moment the function returned.
    // The single-CU path never noticed — it identifies its winner by record INDEX and
    // never reads `.name` — while the merged path, which needs the NAME to key the
    // merge, read freed memory and got `__fu`. 9 cross-CU examples and the link-writer
    // suites went red for a lifetime bug that no amount of reasoning about mangling
    // could have found, because the two strings were never the same string.
    //
    // Fixing only the `auto const` would restore correctness and leave the TRAP: the
    // next caller to obtain records any other way re-arms it, and the failure mode is
    // silent garbage in a program-ENTRY decision. An owning string makes the type
    // incapable of dangling regardless of how a caller sourced the record. There are
    // 0-2 candidates in a program, so the allocation is not a cost worth reasoning
    // about.
    std::string                         name;
    std::optional<EntryMaterialization> verb;   // as stamped by the semantic tier
    // ⚠ DELIBERATELY NO SPAN. Both diagnostics this feeds report WHOLE-PROGRAM
    // facts — "this program defines no entry", "two rival entries are realizable" —
    // so there is no single declaration at fault to point at, and `SemanticModel`
    // exposes no TreeId->Tree resolver from which one could be recovered anyway.
    // Carrying an unset span field would be dead config, and filling it with the
    // first candidate's location would be a FABRICATION: a wrong location is
    // trusted, a missing one is not. The per-definition entry check that DOES have
    // a declaration site is `S_EntryShapeNotDeclared` at the semantic tier, which
    // carries a real span at the declarator.
};

struct DSS_EXPORT ResolvedEntry {
    std::size_t          index = 0;   // index into the candidate span
    EntryMaterialization verb  = EntryMaterialization::None;
};

// Returns the resolved entry, or nullopt. `nullopt` with `ok == true` means "no
// entry, and that is legitimate" (a library/object build); `nullopt` with
// `ok == false` means a diagnostic was reported and the build must stop.
[[nodiscard]] DSS_EXPORT std::optional<ResolvedEntry>
resolveProgramEntry(std::span<EntryCandidate const>      candidates,
                    std::span<EntryMaterialization const> formatVerbs,
                    std::string_view                     formatName,
                    DiagnosticReporter&                  reporter,
                    bool&                                ok);

// Collect this model's program-entry candidates in symbol-record order. Shared by
// both driver paths so "what counts as a candidate" is also single-owner; the
// returned `EntryCandidate::index` position corresponds to `outSymbolIndex[i]`,
// the record's index in `model.symbols()` (which IS its SymbolId on the single-CU
// path).
DSS_EXPORT void collectEntryCandidates(
    SemanticModel const& model, std::vector<EntryCandidate>& out,
    std::vector<std::uint32_t>& outSymbolIndex);

// LOWER half for the MERGED whole-program module (Cycle 25 Stage C). Drives the
// single module `mergeCuMirs` produced (N CUs unified, cross-CU calls already DIRECT,
// resolved externs stripped, user-entry pre-computed) through the SAME lowering body
// `lowerCuMirToAssembly` uses, yielding ONE AssembledModule. The N>1 driver
// (`Program::compileOneTarget`) then `linkAndWrite`s that single module — so the
// linker takes its single-CU path and never mints a cross-CU thunk (the cross-CU call
// is an intra-module direct call). `merged` is mutated (its `Mir` interns lowered
// types; its `externImports` are moved out). `cuId` is the merged image's id (CU0's).
[[nodiscard]] DSS_EXPORT std::optional<AssembledModule>
lowerMergedToAssembly(MergedMirModule&    merged,
                      GrammarSchema const& grammar,
                      TargetSchema const&  target,
                      DataModel            dataModel,
                      BitFieldStrategy     bitFieldStrategy,
                      std::uint16_t        callingConventionIndex,
                      CompilationUnitId    cuId,
                      std::optional<ExternCallDispatch> externCallDispatch,
                      std::optional<DataImportBinding> dataImportBinding,
                      // D-LK-ARM64-EXTERN-DATA-ADDR-PIE-GOT (TF-C52): the
                      // format's extern-ADDRESS binding (nullopt = this leg
                      // has no GOT-address model — an `&extern` value takes
                      // the ordinary lea).
                      std::optional<ExternAddrBinding> externAddrBinding,
                      // TLS C1 (D-CSUBSET-THREAD-LOCAL): the format's
                      // thread-local access block (nullopt = this leg has
                      // no TLS machinery — MIR→LIR fails loud on a
                      // thread-local access).
                      std::optional<TlsAccessInfo> tlsAccess,
                      // c116 (D-WIN64-SEH-FUNCLETS): SEH scope records from
                      // `synthesizeSehFunclets` (empty for a non-SEH program).
                      std::vector<MirSehScope> sehScopes,
                      // D-CSUBSET-LONG-DOUBLE-IEEE128-ARITH (LD-2): the F128
                      // softcall runtime library, pre-resolved in program.cpp
                      // (no ObjectFormatKind in scope in the merge lower body).
                      std::optional<std::string> wideFloatSoftcallLibrary,
                      DiagnosticReporter&  reporter);

// Link N assembled CUs into one image + commit to `outPath` (the shared half of
// `compileSingleUnit`). N==1 is the v1 single-CU path; N>1 triggers the linker's
// cross-CU merge (LK11a resolution + LK11b byte emission). Returns true iff the
// image is `ok()`, no link-tier error fired, and `writeImage` committed bytes.
// `request` carries the per-PROGRAM image knobs (D-SQLITE-PE64-FULL-TIER-
// STACK-DEPTH); it is forwarded verbatim to `linker::link`, which gates it
// against the format's DECLARED capability. Defaults to empty.
[[nodiscard]] DSS_EXPORT bool
linkAndWrite(std::span<AssembledModule const> modules,
             TargetSchema const&              target,
             ObjectFormatSchema const&        format,
             std::filesystem::path const&     outPath,
             DiagnosticReporter&              reporter,
             ImageRequest const&              request = {});

// -- c165 (D-LK-STATIC-LINK): STATIC linking against `ar` archives --------------
//
// The consume half of the "write + consume static and dynamic libs" goal. DSS
// links DYNAMICALLY today (import tables); this adds the STATIC path: on an
// unresolved extern, pull the defining member out of a `.a` and MERGE its code
// INTO the output image (a self-contained executable -- the extern's definition
// is IN the exe, not a runtime DT_NEEDED). Reuses the c154 cross-CU merge
// (`linker::link`'s `mergeModules`) fed from archive members instead of sibling
// CUs -- the reference resolves exactly as it does for a sibling translation unit.

// Does the file at `path` OPEN + begin with the 8-byte GNU/SysV `ar` global
// magic ("!<arch>\n")? This is the ar-vs-dynamic dispatch predicate for the
// `--resolve-library` surface: an `ar` archive routes to the STATIC pull+merge
// below; anything else stays on the dynamic export-reader path. The dispatch is
// by MAGIC BYTES (agnostic), never a `.a`/`.lib` extension. A path that cannot
// be opened/read returns false (it is NOT diverted to the static path -- it stays
// on the dynamic path, whose eager open-probe fails loud `F_FileOpenFailed`, so
// an unreadable `--resolve-library` path is never silently dropped).
[[nodiscard]] DSS_EXPORT bool
isArArchiveFile(std::filesystem::path const& path);

// Pull the archive members that define `clientModule`'s (transitively)
// unresolved externs, each parsed back into a mergeable `AssembledModule` via
// the c164 ELF ET_REL reader (`elf::readRelocatableObject`). Two-pass LAZY-pull
// (plan-11 Q5): a member is pulled only when a collected extern name resolves to
// it through the archive symbol index (armap); a pulled member's OWN unresolved
// externs feed the next pass (a member may reference another member). Members no
// reference reaches are NEVER pulled (lazy, not whole-archive). Each pulled
// member gets a fresh, process-unique `CompilationUnitId` (the merge keys by
// `(cuId, SymbolId)`, so no member collides with the client or another member).
//
// Returns the pulled modules on success (an EMPTY vector is valid -- the archives
// defined nothing the client referenced), or `nullopt` on any file-open /
// archive-parse / member-read failure (fail loud via `reporter`). The reader's
// `ok()` is a tautology for reader output (see elf_object_reader.hpp), so this
// consumes the reader's `optional` return as the read-success signal -- never
// `module.ok()`. Member parsing dispatches on the member's own
// `ObjectFormatKind` (ELF / Mach-O / PE) through one shared chokepoint; a format
// whose kind has no reader arm fails loud rather than mis-parsing a member.
//
// ── `dynamicLibraries` (D-LK-ARCHIVE-MEMBER-EXTERN-CANNOT-BIND-A-RESOLVE-
// LIBRARY) ──────────────────────────────────────────────────────────────────
// An object file records an undefined symbol's NAME and nothing else -- no
// format has anywhere to say WHICH library owns it. So a member whose extern was
// bound to a library when it was COMPILED reads back unbound, and the binding
// must be re-derived here, before the merge, from the same authorities that
// derived it the first time: the binaries the operator NAMED (this parameter,
// which WINS) and then the shipped-descriptor corpus (the platform default).
//
// ⚠ Pass the DYNAMIC half of `--resolve-library` -- the same partition the
// driver hands to the per-CU build. The `ar` archives in that list are
// `archivePaths` here: they are merged INTO the image and record no import at
// all, and feeding one to the export reader is refused loud (correctly), which
// would fail an otherwise good build. Empty is a valid no-op.
[[nodiscard]] DSS_EXPORT std::optional<std::vector<AssembledModule>>
pullStaticArchiveMembers(AssembledModule const&                 clientModule,
                         std::span<std::filesystem::path const> archivePaths,
                         std::span<ResolveLibrarySpec const>    dynamicLibraries,
                         TargetSchema const&                    target,
                         ObjectFormatSchema const&              format,
                         DiagnosticReporter&                    reporter);

// The members extracted from one-or-more input static archives (parallel):
// `modules[i]` is a mergeable relocatable object, `names[i]` its `ar` file name.
struct ExtractedArchiveMembers {
    std::vector<AssembledModule> modules;
    std::vector<std::string>     names;   // parallel to `modules`
};

// Extract EVERY member of each input static `ar` archive (whole-archive), each
// parsed into a mergeable `AssembledModule` via the same per-format reader the
// lazy pull uses. Unlike `pullStaticArchiveMembers` (which pulls only the
// referenced members for an exe/final LINK), this carries ALL members -- a
// static LIBRARY is a package: a DOWNSTREAM link must be able to pull any of
// them, so dropping an unreferenced member would silently ship an incomplete
// library. This is how a merged/"fat" static library bundles the input archives
// it was handed (D-FF1-STATICLIB-FAT-ARCHIVE): the driver appends these to its
// own CU-derived members before `linkAndWriteStaticArchive`. Returns `nullopt`
// (fail loud via `reporter`) on any file-open / archive-parse / member-read
// failure -- never a silent member omission. An empty `archivePaths` yields an
// empty result (a valid no-op).
[[nodiscard]] DSS_EXPORT std::optional<ExtractedArchiveMembers>
extractStaticArchiveMembers(std::span<std::filesystem::path const> archivePaths,
                            TargetSchema const&                    target,
                            ObjectFormatSchema const&              format,
                            DiagnosticReporter&                    reporter);

// Link `clientModule` against zero-or-more static `ar` archives, then write the
// image to `outPath`. When `staticArchives` is empty this is exactly
// `linkAndWrite({clientModule})` (byte-identical to the pre-c165 path). Otherwise
// it pulls the referenced members (`pullStaticArchiveMembers`) and links the
// COMBINED span `[clientModule, pulled...]` -- the N>1 `linker::link` path, whose
// `mergeModules` binds each archive reference to the pulled member's definition
// (stripping the extern import) exactly as it does a sibling-CU reference.
// Returns true iff the pull, merge, link, and write all succeeded.
//
// `dynamicLibraries` rides through to `pullStaticArchiveMembers` -- see its
// contract above for why a pulled member's library binding has to be re-derived
// and why this must be the DYNAMIC half of `--resolve-library`. It is NOT
// defaulted: every route that produces an image has to answer the question, and
// a default would let a new route silently inherit the gap this parameter
// closes.
[[nodiscard]] DSS_EXPORT bool
linkAndWriteWithStaticArchives(AssembledModule                        clientModule,
                               std::span<std::filesystem::path const> staticArchives,
                               std::span<ResolveLibrarySpec const>    dynamicLibraries,
                               TargetSchema const&                    target,
                               ObjectFormatSchema const&              format,
                               std::filesystem::path const&           outPath,
                               DiagnosticReporter&                    reporter,
                               ImageRequest const&                    request = {});

// c163 (D-LK-STATIC-ARCHIVE-WRITER, the writer half of D-FF1-AR-WRITER-STATIC-
// LINK): link N assembled CUs into N RELOCATABLE object members and bundle them
// into ONE GNU/System V `ar` static archive (`.a`) committed to `outPath`. The
// static-library counterpart of `linkAndWrite` (which emits ONE image).
//
// Each module is linked INDEPENDENTLY (a 1-element `link()`, NOT the cross-CU
// merge) to its own `.o` bytes -- an archive PACKAGES separate objects, it does
// not merge them; the FINAL (foreign) linker pulls + merges the members it
// needs. Each member's armap symbol set is its DEFINED, externally-visible
// symbols (`AssembledModule.symbols` filtered by `isExternallyVisible` -- the
// same on-binary names the object writer put in the member's symbol table, so
// the armap round-trips against the member). `format` MUST be a RELOCATABLE
// object format (ELF ET_REL / Mach-O MH_OBJECT); an image-flavor format would
// bundle non-relocatable images (a foreign linker cannot pull from those) and
// fails loud. `memberNames` is parallel to `modules` (each member's archived
// file name, e.g. "lib.o"). Returns true iff every member linked ok, the
// archive built, and the bytes committed. The `ar` framing itself is
// FORMAT-BLIND -- see `link/format/ar.hpp`.
//
// The DRIVER/CLI request surface (a project `artifactProfile: "staticlib"` or a
// `--emit staticlib` flag routing here + the `.a`/`.lib` extension policy +
// member naming) is the named follow-up D-FF1-AR-STATICLIB-DRIVER-WIRING; this
// is the shipped composition it will call.
//
// `request` (D-SQLITE-PE64-FULL-TIER-STACK-DEPTH) is forwarded to EVERY member
// link so the capability gate fires here too. No relocatable/archive format
// declares a stack-reserve capability -- a static archive carries no image
// headers at all -- so a request routed here is REFUSED. That is the point:
// accepting the parameter and ignoring it is what would let the request vanish
// on a `staticlib` target.
[[nodiscard]] DSS_EXPORT bool
linkAndWriteStaticArchive(std::span<AssembledModule const> modules,
                          std::span<std::string const>     memberNames,
                          TargetSchema const&              target,
                          ObjectFormatSchema const&        format,
                          std::filesystem::path const&     outPath,
                          DiagnosticReporter&              reporter,
                          ImageRequest const&              request = {});

} // namespace dss
