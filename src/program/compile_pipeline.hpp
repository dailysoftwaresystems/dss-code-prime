#pragma once

#include "analysis/semantic/semantic_model.hpp"  // SemanticModel (CuMirModule member, move-only)
#include "asm/asm.hpp"  // AssembledModule (assembleUnit return + linkAndWrite span)
#include "core/export.hpp"
#include "core/types/data_model.hpp"  // DataModel (CuMirModule member + lowerMergedToAssembly arg)
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"  // ExternImport (CuMirModule member)
#include "core/types/grammar_schema.hpp"
#include "core/types/strong_ids.hpp"  // CompilationUnitId (CuMirModule member)
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"  // TypeInterner (optimizeModule arg)
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
    std::vector<std::filesystem::path> resolveLibraries;
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

[[nodiscard]] DSS_EXPORT bool
compileSingleUnit(CompilationUnit const&         cu,
                  GrammarSchema const&           grammar,
                  TargetSchema const&            target,
                  ObjectFormatSchema const&      format,
                  std::uint16_t                  callingConventionIndex,
                  std::filesystem::path const&   outPath,
                  DiagnosticReporter&            reporter,
                  CompileOptions const&          opts = {});

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
             CompileOptions const&          opts = {});

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
    // FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER): pe64 <threads.h> shim SymbolId.v →
    // recipe id, carried from the BUILD half's `CstToHirResult.synthRecipeBySymbol` so
    // the LOWER half's `synthesizeThreadsShim` (fired at the synthesizePeStartup seam)
    // can supply each shim function's definition. Empty for every elf/macho + every
    // non-threads pe TU (the overwhelming majority — a bare default-constructed map).
    std::unordered_map<std::uint32_t, std::string> threadsRecipes;
    // D-CSUBSET-C11-THREADS-MACHO: the active format's shipped-library synth vehicle
    // (win32 / pthread + import library), captured here for the SAME reason as
    // `tlsAccess` — the LOWER half sees only this struct, and `synthesizeThreadsShim`
    // reads it to pick the primitive family it emits over. nullopt on elf (direct FFI →
    // `threadsRecipes` is empty there anyway); a non-empty `threadsRecipes` with a
    // nullopt vehicle is a fail-loud in the synth pass (never a silently-assumed vehicle).
    std::optional<LibrarySynthesis> librarySynthesis;
    // D-CSUBSET-C11-THREADS-MACHO: the active object format's kind, captured here so the
    // LOWER half's `synthesizeThreadsShim` can C-mangle its native helper-import names for
    // the format (macho prepends `_`) — the SAME applyCMangling the FFI ingest uses.
    ObjectFormatKind objectFormat = ObjectFormatKind::Unknown;
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
           CompileOptions const&          opts = {});

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
[[nodiscard]] DSS_EXPORT bool
optimizeModule(Mir&                  mir,
               TargetSchema const&   target,
               TypeInterner const&   interner,
               CompileOptions const& opts,
               DiagnosticReporter&   reporter);

// LOWER half: MIR → LIR → liveness → regalloc → rewrite → legalize → callconv →
// assemble → symbol-table populate → user-entry scan. Consumes the `CuMirModule`
// (its `externImports` are MOVED into MIR→LIR; its `mir` + `model` are read). Returns
// nullopt on any back-half tier failure (diagnostics emitted via `reporter`).
//
// c111 (D-RUNTIME-PE-MAIN-ARGS): `processArgs` is the target format's declared
// program-entry argument mechanism (nullopt when the format declares none). After the
// user entry is resolved, this drives `synthesizePeStartup` — the single-CU counterpart
// of the merge-path synth in `program.cpp` — which, for the CRT out-parameter mechanism,
// appends the pre-main init that fetches argc/argv and retargets the entry to it.
[[nodiscard]] DSS_EXPORT std::optional<AssembledModule>
lowerCuMirToAssembly(CuMirModule&                       cuMir,
                     std::optional<ProcessArgs> const& processArgs,
                     ObjectFormatKind                  fmtKind,
                     DiagnosticReporter&               reporter);

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
[[nodiscard]] DSS_EXPORT bool
linkAndWrite(std::span<AssembledModule const> modules,
             TargetSchema const&              target,
             ObjectFormatSchema const&        format,
             std::filesystem::path const&     outPath,
             DiagnosticReporter&              reporter);

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
// `module.ok()`. SCOPE: ELF (the reader is ELF-only; a non-ELF `format` makes
// `readRelocatableObject` fail loud -- Mach-O/COFF are named follow-ups).
[[nodiscard]] DSS_EXPORT std::optional<std::vector<AssembledModule>>
pullStaticArchiveMembers(AssembledModule const&                 clientModule,
                         std::span<std::filesystem::path const> archivePaths,
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
[[nodiscard]] DSS_EXPORT bool
linkAndWriteWithStaticArchives(AssembledModule                        clientModule,
                               std::span<std::filesystem::path const> staticArchives,
                               TargetSchema const&                    target,
                               ObjectFormatSchema const&              format,
                               std::filesystem::path const&           outPath,
                               DiagnosticReporter&                    reporter);

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
[[nodiscard]] DSS_EXPORT bool
linkAndWriteStaticArchive(std::span<AssembledModule const> modules,
                          std::span<std::string const>     memberNames,
                          TargetSchema const&              target,
                          ObjectFormatSchema const&        format,
                          std::filesystem::path const&     outPath,
                          DiagnosticReporter&              reporter);

} // namespace dss
