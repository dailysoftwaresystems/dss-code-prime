#pragma once

#include "core/export.hpp"
#include "hir/hir.hpp"
#include "hir/hir_attrs.hpp"                 // HirSourceMap
#include "hir/hir_literal_pool.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// CST→HIR lowering (plan 09 HR8) — the single, language-agnostic engine.
//
// `lowerToHir` reads a `SemanticModel` (one CompilationUnit, all its trees) plus
// its schema's `hirLowering` + `semantics` config blocks, and produces one frozen
// HIR `Module`. It NEVER branches on `schema.name()`: every per-language mapping
// is sourced from the config (a new language lowers by adding a `hirLowering`
// block, no engine edits — master-plan thesis decision #4).
//
// The result is heap-stable (`unique_ptr`) because `HirSourceMap` binds to `&hir`
// by raw pointer (same discipline as `HirParseResult` in hir_text.hpp): a stable
// address keeps the source-map valid. Access the module as `result->hir`.
//
// Discipline: collect-all, abort-free. A construct with no `hirLowering` mapping
// (or a known-deferred one — extern / compound-assign / ++ / arrays / strings,
// owned by later plans) yields an `Error` HIR node + an
// `H_UnsupportedLoweringForKind` diagnostic, never a silent skip or a miscompile.
// Verify-on-load: `HirVerifier` runs on the produced module before returning.

namespace dss {

class SemanticModel;
class DiagnosticReporter;

// FF6 Slice 2 (2026-06-02): one row per source-declared extern
// produced by the lowerer. Caller-owning pair (HirNodeId + canonical
// name) — the lowerer is the single authority on the source-level
// identifier that maps to each extern HirNodeId. The FFI synthesis
// path (`synthesizeFfiFromSourceDecls` in `src/ffi/ingest.cpp`)
// reads the public `externDecls` field directly (via
// `compileSingleUnit` step 2.5), builds a transient
// `vector<ExternDeclRef>` view-list over the records, and produces
// one `FfiMetadata` entry per row (mangledName via FF4 +
// importLibrary via the per-language `externLibraryByFormat` map).
// Owning the canonical-name string here decouples the FFI stage
// from the lifetime of the analyzer's `SemanticModel` — the
// lifecycle assumption matches the rest of the result struct
// (lifetime tied to the unique_ptr returned by `lowerToHir`).
struct DSS_EXPORT HirExternRecord {
    HirNodeId   node;
    std::string canonicalName;  // undecorated source identifier
    // D-CSUBSET-EXTERN-LIBRARY-SYNTAX closure (step 13.3, 2026-06-02) +
    // Model 3 (2026-06-09): per-symbol import-library override, now a
    // per-OBJECT-FORMAT map keyed by `objectFormatKindName`
    // ("pe"/"elf"/"macho"/…). Two producers populate it target-agnostically:
    //   * a SHIPPED descriptor passes its per-format `library` map verbatim
    //     (Model 3 — different image per format);
    //   * a SOURCE-declared `extern "libname" …` override populates the SAME
    //     string under every known format key (the user's choice is
    //     format-independent), so the fold below yields it whatever the target.
    // EMPTY map ⇒ the FFI synthesize stage falls back to the language's
    // `externLibraryByFormat[format]` default. The map is folded to ONE string
    // for the ACTIVE target's format at `compile_pipeline` (step 2.5), where
    // the object format is in scope — keeping this lowering target-agnostic.
    std::unordered_map<std::string, std::string> libraryOverride;
    // c86 (D-CSUBSET-BARE-PROTO-EXTERN-SYNTHESIS): TRUE ⇒ this extern
    // deliberately carries NO import library — a bare-prototype cross-TU
    // reference (C 6.2.2p5). The FFI synthesize stage then leaves
    // `FfiMetadata.importLibrary` EMPTY (it does NOT fall back to the
    // format default — that fallback is exactly what this flag opts out
    // of), the HIR→MIR extern pre-pass admits the empty library, and the
    // LINKER resolves the surviving import against a sibling TU's
    // definition or rejects it LOUD as an undefined symbol. Mutually
    // exclusive with a non-empty `libraryOverride` by construction (the
    // bare-proto producer sets exactly one of the two).
    bool noLibraryBinding = false;
    // c156 (D-LK-ELF-SYMBOL-VERSIONING): the REQUIRED ELF symbol version
    // (e.g. "GLIBC_2.3"), already resolved for the active (arch, format) by the
    // descriptor reader. Unlike `libraryOverride` (a per-format MAP folded
    // downstream), the version is a plain per-symbol STRING — resolved once at
    // read time — so it rides the `canonicalName` rail, not the folded-map rail.
    // Empty ⇒ unversioned. Threaded to FfiMetadata.version → the ELF writer.
    std::string version;
    // D-LINK-EXTERN-IMPORT-REFERENCE-GATE: TRUE ⇒ this is an EAGER import — a
    // shipped-library descriptor symbol (producer C, `model.shippedExterns()`)
    // that DSS imports even when UNREFERENCED (the D-FFI-DESCRIPTOR-EAGER-IMPORT
    // invariant: a `#include`d descriptor lists a real library export the linker
    // must bind whether or not this TU calls it). The linker's reference gate
    // (`rejectOrDropUnreferencedExterns`) KEEPS an eager row unconditionally;
    // a NON-eager import (producers A/B — a source `extern` decl or a bare-proto
    // synthesis) survives ONLY when referenced (gcc's unused-decl-emits-nothing
    // rule). INVARIANT: isEagerImport ⟹ library-bound (a descriptor always ships
    // a per-format `library` map); the flag never rides an empty-library row.
    bool isEagerImport = false;
};

struct DSS_EXPORT CstToHirResult {
    Hir            hir;
    HirSourceMap   sourceMap;     // bound to `hir` — must stay at a stable address
    HirLinkageMap  linkageMap;    // bound to `hir` — native-decl binding/visibility
                                  // (D-CSUBSET-LINKAGE-SPECIFIERS); read at HIR→MIR
    HirMutabilityMap mutabilityMap; // bound to `hir` — native-global const-ness
                                  // (D-LK4-DATA-PRODUCER-MUTABLE-GLOBAL); read at
                                  // HIR→MIR to pick `.rodata` vs writable `.data`
    HirThreadLocalMap threadLocalMap; // bound to `hir` — declaration thread-storage
                                  // duration (TLS C1, D-CSUBSET-THREAD-LOCAL); read
                                  // at HIR→MIR to stamp MirGlobal.isThreadLocal /
                                  // ExternImport.isThreadLocal (→ .tdata/.tbss)
    HirVolatileMap volatileMap;   // bound to `hir` — per-ACCESS volatility (c21,
                                  // D-CSUBSET-VOLATILE-QUALIFIER); read at HIR→MIR
                                  // to OR MirInstFlags::Volatile onto Load/Store
    HirReturnsTwiceMap returnsTwiceMap; // bound to `hir` — per-CALL returns-twice
                                  // (FC17.9(c), D-CSUBSET-SETJMP); read at HIR→MIR to
                                  // OR MirInstFlags::ReturnsTwice onto a setjmp Call
    HirAlignmentMap alignmentMap; // bound to `hir` — per-DECLARATION explicit
                                  // `alignas` (D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN);
                                  // read at HIR→MIR to raise a global's data-item
                                  // alignment + a local's effective alloca alignment
    // VLA C1a/C3 (D-CSUBSET-VLA, Fork A out-of-band size side-table): a block-scope
    // variable-length array's LOWERED size-expression HIR nodes, keyed by the
    // declared local's SymbolId (`.v`). The array suffixes are normally SKIPPED at
    // CST→HIR; for a VLA declarator EVERY suffix is un-skipped and its length expr
    // lowered to a floating HIR node (valid in `hir`, reachable only via this map —
    // the arena keeps it; no pass walks to it). The vector holds one node per
    // dimension in OUTER→INNER (source) order — a 1-D VLA has one entry, `int a[n][m]`
    // has [n, m]. HIR→MIR's `allocaForLocal`/`vlaAllocaForLocal` re-lowers each to a
    // runtime MIR value at the DECL point and forms the cumulative row strides + the
    // total byte size for the Alloca's runtime operand. Empty for every non-VLA TU.
    std::unordered_map<std::uint32_t, std::vector<HirNodeId>> vlaSizeExprBySymbol;
    // VLA C2 (D-CSUBSET-VLA): a `sizeof <vla-object>` HirNode → the VLA operand's
    // SymbolId (`.v`), keyed by the SizeOf HIR node's id (`.v`). Populated at CST→HIR
    // ONLY when the sizeof operand resolves to a VLA-typed symbol; the SizeOf node's
    // TypeRef child is LEFT as the `vlaArray` type UNCHANGED, so every const-eval
    // consumer keeps declining a VLA sizeof (never a wrong constant). HIR→MIR reads
    // this to emit a runtime Load of the decl-frozen size instead of a static fold.
    // Empty for every non-VLA translation unit.
    std::unordered_map<std::uint32_t, std::uint32_t> sizeofVlaSymbol;
    // VLA C4b (D-CSUBSET-VLA): a VLA-TYPEDEF OBJECT's SymbolId.v → its typedef
    // origin `R`'s SymbolId.v (`typedef int R[n]; R a;` → a.v → R.v). Populated at
    // CST→HIR from the object symbol's `SymbolRecord.vlaTypedefOrigin` (set
    // semantically). The object's OWN size capture is SKIPPED (its declarator
    // carries no `[n]`); R's bound was captured under R's own SymbolId instead
    // (`vlaSizeExprBySymbol[R.v]`). HIR→MIR reads this to, at `R a;`'s alloca, copy
    // R's decl-frozen per-level size slots DOWN into a's own `(a, levelType)` slots
    // (so `a[i]` / `sizeof a` read them) and size a's runtime alloca from R's
    // whole-object slot. Empty for every non-VLA-typedef translation unit.
    std::unordered_map<std::uint32_t, std::uint32_t> typedefVlaOriginBySymbol;
    // FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER): a pe64 <threads.h> shim symbol's
    // SymbolId.v → its synth-recipe id (== the symbol name). Populated at CST→HIR when
    // a `synthesize`-tagged `ShippedExternSymbol` is reached: instead of an
    // ExternFunction/HirExternRecord (which would eager-import a non-exported kernel32
    // name — the c101 0xC0000139 law), the symbol is recorded HERE. HIR→MIR reads it
    // to SEED `functionSymbols` so the user's `mtx_lock(&m)` call lowers to
    // GlobalAddr(sym) against a not-yet-defined callee (MirVerifier tolerates that),
    // and `synthesizeThreadsShim` (mir/merge) supplies the definition before link.
    // Empty for every non-threads (and every elf/macho) translation unit.
    std::unordered_map<std::uint32_t, std::string> synthRecipeBySymbol;
    HirLiteralPool literalPool;   // decoded literal values, indexed by literalIndex
    // True iff neither lowering nor the verify-on-load pass emitted an
    // Error-severity diagnostic (delta-computed, so prior diagnostics on the
    // shared reporter don't taint the verdict).
    bool           ok = false;
    // FF6 Slice 2 (2026-06-02): the source-declared externs the
    // lowerer produced. Populated by `lowerExternDeclInto` in
    // declaration order (one per declarator). Consumed by the FFI synthesis stage at
    // `compileSingleUnit` between HIR and MIR lowering. Empty
    // when the source declares no externs (every existing
    // pre-FF6 test fixture).
    std::vector<HirExternRecord> externDecls;

    // `hir` is declared first so the maps bind to the constructed module.
    CstToHirResult(Hir h, HirLiteralPool lp)
        : hir(std::move(h)), sourceMap(hir), linkageMap(hir),
          mutabilityMap(hir), threadLocalMap(hir), volatileMap(hir),
          returnsTwiceMap(hir), alignmentMap(hir), literalPool(std::move(lp)) {}

    CstToHirResult(CstToHirResult const&)            = delete;
    CstToHirResult& operator=(CstToHirResult const&) = delete;
    CstToHirResult(CstToHirResult&&)                 = delete;
    CstToHirResult& operator=(CstToHirResult&&)      = delete;
};

// Lower `model`'s compilation unit to one HIR module. Lowering + verify-on-load
// diagnostics go to `reporter`. `model.lattice().interner()` must outlive the
// returned result (the module's TypeIds and the verifier reference it).
[[nodiscard]] DSS_EXPORT std::unique_ptr<CstToHirResult>
lowerToHir(SemanticModel& model, DiagnosticReporter& reporter);

} // namespace dss
