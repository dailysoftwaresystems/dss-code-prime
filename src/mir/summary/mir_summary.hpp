#pragma once

#include "core/export.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/symbol_attrs.hpp"
#include "mir/mir.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════════
// OPT11 — THE PER-TU MODULE SUMMARY (plan 22 §0.2)
// ═══════════════════════════════════════════════════════════════════════════
//
// DSS today merges every CU's MIR into ONE module and runs one optimizer over
// it — full-LTO semantics on every build, and SERIAL. ✔MEASURED (P36, the
// 103-TU full-source sqlite corpus, re-derived in this lane from lane-f's raw
// `remap-j4.log`): the single `program`-stage optimizer invocation costs
// 4766 ms of pass cpu against 1353 ms across all 103 pooled `unit`
// invocations — **77.9% of ALL optimizer cpu in ONE serial call**, while the
// CU pool next to it runs at 3.96x.
//
// The replacement, and the whole point of this file: each TU keeps its OWN
// MIR and emits a compact SUMMARY — its call graph plus its symbol/linkage
// facts, and NO BODIES. A cheap global pass (`summary_index.hpp`) reads only
// the summaries and decides what should be imported where; every TU is then
// optimized IN PARALLEL, importing on demand only the callees it was told
// about. The whole-program DECISION is decoupled from the whole-program
// TRANSFORMATION.
//
// ★★★ THE INVARIANT THAT DEFINES THE DESIGN: **NO OPTIMIZER EVER SEES ALL THE
// MIR.** A summary must therefore be a strict *projection* — every field here
// is a fact some decision cannot be made without, and a body is never one of
// them. If this type ever grows a field that requires reading another TU's
// instructions, the architecture has been lost and the merge has merely been
// done N times instead of once.
//
// ── WHAT A SUMMARY IS FOR, FIELD BY FIELD ─────────────────────────────────
//
// Three consumers, and every field below serves one of them:
//
//   1. THE CROSS-TU SYMBOL RESOLUTION — `binding` + `visibility` + the
//      declared NAME feed `resolveCrossCuDefs`, the SAME single source of
//      truth `mergeCuMirs` already uses, so the index and the merge cannot
//      disagree about which definition wins.
//   2. THE WHOLE-PROGRAM LIVENESS BFS — `scanLiveSymbols` (opt/passes/dce.cpp)
//      runs an INTER-PROCEDURAL BFS with a fixpoint over global initializer
//      literals. ★ NO PER-TU PASS CAN COMPUTE THAT. `SummaryFunction::
//      symbolRefs` and `SummaryGlobal::initSymbolRefs` are the edge sets that
//      BFS walks; without them the index would silently ship dead code that
//      today's merge deletes — a regression, which the operator's "no trade
//      off" ruling forbids.
//   3. THE INLINE-CANDIDATE FILTER — everything else. The index uses these to
//      cheaply PRUNE callees `inlineLegalityGate` would certainly refuse, so
//      no body is paged in pointlessly.
//
// ★★ AND ON POINT 3, THE LOAD-BEARING ARCHITECTURAL CHOICE: the summary
// facts NEVER *decide* legality — they only decide AVAILABILITY. After a TU
// imports a body, the EXISTING, UNCHANGED `inlineLegalityGate` runs on the
// post-import module and has the final word. That split is what makes the
// quality bar a theorem instead of a hope: the gate is the same code making
// the same decision on the same shapes, so the only way the index can lose
// quality is by failing to make a body AVAILABLE. Every predicate here is
// therefore written to be PERMISSIVE — it may admit a callee the gate will
// later refuse (costing only a wasted import), and it must never exclude one
// the gate would have accepted.
//
// ★★★ TWO FACTS HERE ARE WHOLE-PROGRAM AND ARE A SILENT-MISCOMPILE HAZARD IF
// DROPPED. `inlineLegalityGate` rule 4 refuses a callee whose address escapes
// ANYWHERE IN THE MODULE — and today "the module" is the whole program. A
// per-TU analysis would see only its own escapes and would therefore be LESS
// conservative than today: a callee whose address is taken in TU B would be
// inlined in TU A, and its out-of-line body could then be dropped while an
// indirect call still reaches for it. `escapedSymbolNames` exists so the
// index can hand each TU the WHOLE-PROGRAM escape set. The same argument
// covers the call-graph SCC behind rule 3 (recursion), which the index
// reconstructs from `SummaryCallSite::calleeName` edges.
//
// ── AGNOSTIC ──────────────────────────────────────────────────────────────
// Nothing here branches on a language, a CPU target, or an object format.
// `targetIdentity` is carried as an OPAQUE STRING and is only ever compared
// for equality — the summary refuses to be mixed across targets without
// knowing what a target IS.

namespace dss::mirsum {

// The summary format's own version. Bumped whenever the ENCODING changes in a
// way a previous reader would misread. A decoder that sees a version it does
// not know REFUSES (returns nullopt) rather than guessing — a summary is an
// input to codegen decisions, so a misread field is a miscompile.
inline constexpr std::uint32_t kSummaryFormatVersion = 1;

// One call site, recorded because the index's call graph is built from these.
struct SummaryCallSite {
    // The callee's declared NAME — the SAME cross-CU key `mergeCuMirs` uses
    // (`MergeCuInput::nameOf` of the `GlobalAddr`'s symbol). EMPTY iff the
    // call is indirect or the callee symbol has no name.
    std::string   calleeName;
    // True iff the callee operand is a `GlobalAddr` naming a symbol — i.e.
    // this edge is a DIRECT call the index may reason about. An indirect call
    // still gets a row (it is a `hasIndirectCall` fact for the caller and a
    // reason the escape set matters) but contributes no call-graph edge.
    bool          direct = false;
    // COST / HOTNESS proxy: the loop-nest depth of the block holding the call,
    // computed from the function's own natural loops. A call at depth 2 runs
    // (heuristically) far more often than one at depth 0, which is what a
    // profitability policy wants to know and is the ONLY hotness signal
    // available without profile data. Recorded, not yet consumed — the shipped
    // policy is size-based (`inlineThreshold`), exactly as the in-module gate
    // is today. Kept in the summary because it is FREE to compute here
    // (the loop analysis already runs) and IMPOSSIBLE to recover later.
    std::uint32_t loopDepth = 0;
};

// A DEFINED function's summary row. One integer (`instCount`) replaces the
// body for cost purposes, because the shipped cost model IS a MIR instruction
// count — see `inlineLegalityGate` rule 6.
struct SummaryFunction {
    // The declared name — the cross-CU key. EMPTY for a symbol `nameOf` does
    // not name, which is treated as module-private and never matched across
    // TUs (`mergeCuMirs` applies the identical rule).
    std::string      name;
    // This TU's own `SymbolId.v`. Meaningful ONLY within this summary's
    // module — the index never compares raw symbol values across TUs, it
    // compares NAMES, exactly as the linker does.
    std::uint32_t    symbol = 0;
    SymbolBinding    binding    = SymbolBinding::Local;
    SymbolVisibility visibility = SymbolVisibility::Default;
    // Total MIR instructions across every block. The cost model's whole input.
    std::uint32_t    instCount  = 0;
    // How many distinct `Arg` positions the body references, expressed as the
    // highest flat call-operand position + 1 (0 if it references none). The
    // gate's arity check is per-CALL-SITE and stays there; this is the cheap
    // summary-level screen.
    std::uint32_t    argExtent  = 0;
    std::uint32_t    blockCount = 0;

    // ── source-declared directives the gate OBEYS (never heuristics) ──
    bool noInline     = false;   // gate rule 2b — unconditional refusal
    bool alwaysInline = false;   // gate rule 6 — waives the size bound only
    bool noOptimize   = false;   // gate rule 2c — refused in BOTH directions

    // ── shape facts the gate refuses on, pre-computed so no body is needed ──
    // Any `BlockAddress` / `IndirectBr` (computed goto): renumbering the
    // callee's blocks into a caller invalidates both the captured `&&label`
    // symbols and the indirect branch's successor set.
    bool hasComputedGoto = false;
    // Any SEH region opcode. Splicing a `__try` collides function-scoped
    // region ids and would need callee scope-table entries merged into the
    // caller's `.xdata`.
    bool hasSeh = false;
    // The UNION of every remaining opcode-level refusal in `inlineLegalityGate`
    // rule 5 — a frame- or ABI-sensitive construct that binds to the CALLEE's
    // own frame and would silently bind to the caller's if spliced:
    // `ReadIndirectResult` / multi-piece `Return` (x8-sret and register-pair
    // struct returns), `RecvByValueStackParam`, the three va_start area leaves,
    // a returns-twice `Call` (setjmp), and `StackSave`/`StackRestore` (VLA
    // teardown). ONE flag rather than seven because the index's only question
    // is "can this ever be spliced?" — the gate still names the precise reason
    // when it re-checks post-import, and keeping seven booleans in the wire
    // format would invite them to drift out of sync with the gate.
    bool frameBound = false;
    // At least one `Return`. A callee with NO returning path leaves the
    // splice's continuation block predecessor-less, which the MirVerifier
    // rejects — turning a valid program into a build error.
    bool hasReturn = false;
    // Any indirect `Call` in the body. Not a refusal; a fact the index needs
    // to know its call graph is INCOMPLETE for this function, which is what
    // makes the whole-program escape set load-bearing rather than optional.
    bool hasIndirectCall = false;

    // C99 6.7.4p7: this symbol is ALSO declared by an `ExternImport` row in
    // the same TU — an `inline` definition whose external definition lives
    // elsewhere. It is importable but must NOT be treated as the owning
    // definition of the symbol, so it is recorded distinctly rather than
    // being allowed to win cross-TU resolution by accident.
    bool isInlineDefinition = false;

    // Every symbol NAME this function references through a `GlobalAddr`,
    // sorted and deduplicated. The edge set of the whole-program liveness BFS
    // (consumer 2 above) — a superset of `calls`, since a `GlobalAddr` may
    // name a global, or a function whose address is taken rather than called.
    std::vector<std::string> symbolRefs;
    // Call sites in a deterministic walk order (blocks in natural module
    // order, instructions in block order). NOT sorted — the ORDER IS DATA
    // here: it is the order the in-module inliner would encounter these
    // sites, so a policy that spends a budget over them spends it the same
    // way the in-module pass would.
    std::vector<SummaryCallSite> calls;
};

// A DEFINED global's summary row.
struct SummaryGlobal {
    std::string      name;
    std::uint32_t    symbol = 0;
    SymbolBinding    binding    = SymbolBinding::Local;
    SymbolVisibility visibility = SymbolVisibility::Default;
    bool             isConst        = false;
    bool             isThreadLocal  = false;
    std::uint32_t    alignmentBytes = 0;
    // True iff a runtime initializer FUNCTION runs for this global. The index
    // needs the fact (such a global's initializer is a liveness root) but not
    // the function id, which is TU-local.
    bool             hasInitFunc = false;
    // Symbol-address targets embedded in this global's INITIALIZER LITERAL,
    // by name, sorted + deduplicated — including targets nested inside
    // AGGREGATE literals (a function-pointer table, a `&global` member).
    // ★ This is the edge set of `scanLiveSymbols` PHASE 3, the fixpoint that
    // exists precisely because a function reachable only through a data
    // relocation is otherwise wrongly deleted.
    std::vector<std::string> initSymbolRefs;
};

// An UNDEFINED symbol — one `ExternImport` row, projected.
struct SummaryImport {
    // ⚠ The identity of an import is the TRIPLE, never the name alone:
    // `foo` from `a.dll` and `foo` from `b.dll` are DIFFERENT dynamic symbols,
    // and so are `puts@GLIBC_2.2.5` and `puts@GLIBC_2.17`. The index keys on
    // `summaryImportKey` below, which is byte-for-byte the same
    // length-prefixed encoding `mergeCuMirs::ffiImportKey` uses — so the index
    // and the merge cannot disagree about what "one import" means.
    std::string   mangledName;
    std::string   libraryPath;
    std::string   version;
    std::uint32_t symbol = 0;
    bool          isData        = false;
    bool          isThreadLocal = false;
};

// One TU's complete summary.
struct ModuleSummary {
    // ── self-identity, so a summary can never be mixed across machines ──
    // This TU's Tier-1 key digest — an OPAQUE string supplied by the caller
    // (the compile driver, which owns `RuntimeObjectKey`). Kept opaque
    // deliberately: `mir_summary` is a leaf that must not reach up into the
    // driver, and the index only ever compares digests for equality.
    // ★★★ Tier 2 — the key of the POST-IMPORT object — is this digest
    // COMPOSED with the ordered set of (imported symbol, defining module's
    // Tier-1 digest) and the policy's own identity. KEYING THE POST-IMPORT
    // OBJECT ON THE TU ALONE IS A SILENT MISCOMPILE: edit a callee and a
    // stale inlined copy of its old body survives in the caller's cached
    // object. `SummaryIndex::tier2KeyInputs` (summary_index.hpp) computes
    // exactly that composition.
    std::string moduleDigest;
    // `<arch>:<format>` or any other opaque spelling the driver chooses.
    // Compared for EQUALITY ONLY — never parsed, never branched on.
    std::string targetIdentity;

    std::vector<SummaryFunction> functions;
    std::vector<SummaryGlobal>   globals;
    std::vector<SummaryImport>   imports;

    // ★★★ THE WHOLE-PROGRAM FACT (see the header comment): every NAMED symbol
    // whose address ESCAPES in this TU — referenced by a `GlobalAddr` that is
    // used as anything other than the callee slot of a `Call`. Sorted +
    // deduplicated. The index unions these across TUs and hands the result to
    // every per-TU optimize, so rule 4 stays exactly as conservative as it is
    // today. Unnamed (module-private) escapes are deliberately absent — they
    // cannot be referenced from another TU, so the TU's own `analyzeModule`
    // already sees them.
    std::vector<std::string> escapedSymbolNames;
};

// The import-identity key — byte-for-byte the encoding
// `mir/merge/mir_merge.cpp::ffiImportKey` uses. LENGTH-PREFIXED, never
// separator-joined: a mangledName is arbitrary bytes from a descriptor, so any
// separator-joined encoding is non-injective and could fold two UNRELATED
// imports into one.
[[nodiscard]] DSS_EXPORT std::string summaryImportKey(SummaryImport const& e);
[[nodiscard]] DSS_EXPORT std::string summaryImportKey(ExternImport const& e);

// Everything one TU contributes. DECOMPOSED (rather than a driver type) for
// the same reason `MergeCuInput` is: a MIR-tier unit test can hand-build every
// field without a `SemanticModel`, and the summary stays testable in isolation.
//
// ⚠ `nameOf` MUST be the SAME name function the merge uses — the declared name
// that reaches the symbol table. A second spelling of the on-binary name WILL
// drift from the armap the archive already round-trips, and two TUs would then
// disagree about symbol identity.
struct SummaryCuInput {
    Mir const*                           mir = nullptr;
    std::function<std::string(SymbolId)> nameOf;
    std::span<ExternImport const>        externImports;
    std::string                          moduleDigest;
    std::string                          targetIdentity;
};

// Build one TU's summary. A PURE FUNCTION OF THAT TU — no other TU is an
// input, which is exactly what makes the result stable and cacheable under the
// Tier-1 key.
//
// ⓘ ON LIVENESS PRECISION. `scanLiveSymbols` filters its `GlobalAddr` scan by
// per-function DCE liveness; this walk records every `GlobalAddr` reference.
// The two agree in the shipped topology because a summary describes a module
// that has ALREADY been through its per-TU pipeline (which runs `Dce`), so a
// `GlobalAddr` whose only use is dead has already been deleted. The residual
// difference is second-order — a reference made dead only by a later
// import-driven simplification — and it errs toward KEEPING a symbol, never
// toward deleting a live one.
[[nodiscard]] DSS_EXPORT ModuleSummary buildModuleSummary(SummaryCuInput const& cu);

// ── the wire format (the payload of Lane H's `.dss.summary` section) ───────
//
// DETERMINISTIC AND HOST-INDEPENDENT BY CONSTRUCTION:
//   * every integer is written LITTLE-ENDIAN byte by byte — never a `memcpy`
//     of a struct and never a host-endianness assumption. ★ This is not
//     hypothetical tidiness: DSS has a live big-endian leg (s390x under
//     qemu-s390x), and a summary produced there must decode identically here.
//   * every collection is written in a deterministic order, fixed by
//     `buildModuleSummary` (module walk order for functions/globals/imports,
//     sorted for the reference and escape sets).
//   * no padding, no pointers, no addresses, no hash-map iteration.
// So `encode(build(m))` is byte-identical run to run for the same `m`, which
// is half of the determinism bar this arc must meet.
[[nodiscard]] DSS_EXPORT std::vector<std::uint8_t>
encodeModuleSummary(ModuleSummary const& s);

// Decode. Returns nullopt — never a partially-filled summary — on a bad magic,
// an unknown version, a truncated buffer, or any length field that overruns.
// FAIL-LOUD: a summary drives codegen decisions, so a misread field is a
// miscompile and guessing is never the safe option.
[[nodiscard]] DSS_EXPORT std::optional<ModuleSummary>
decodeModuleSummary(std::span<std::uint8_t const> bytes);

} // namespace dss::mirsum
