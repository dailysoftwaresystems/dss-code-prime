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
};

struct DSS_EXPORT CstToHirResult {
    Hir            hir;
    HirSourceMap   sourceMap;     // bound to `hir` — must stay at a stable address
    HirLinkageMap  linkageMap;    // bound to `hir` — native-decl binding/visibility
                                  // (D-CSUBSET-LINKAGE-SPECIFIERS); read at HIR→MIR
    HirMutabilityMap mutabilityMap; // bound to `hir` — native-global const-ness
                                  // (D-LK4-DATA-PRODUCER-MUTABLE-GLOBAL); read at
                                  // HIR→MIR to pick `.rodata` vs writable `.data`
    HirVolatileMap volatileMap;   // bound to `hir` — per-ACCESS volatility (c21,
                                  // D-CSUBSET-VOLATILE-QUALIFIER); read at HIR→MIR
                                  // to OR MirInstFlags::Volatile onto Load/Store
    HirLiteralPool literalPool;   // decoded literal values, indexed by literalIndex
    // True iff neither lowering nor the verify-on-load pass emitted an
    // Error-severity diagnostic (delta-computed, so prior diagnostics on the
    // shared reporter don't taint the verdict).
    bool           ok = false;
    // FF6 Slice 2 (2026-06-02): the source-declared externs the
    // lowerer produced. Populated by `lowerExternDecl` in
    // declaration order. Consumed by the FFI synthesis stage at
    // `compileSingleUnit` between HIR and MIR lowering. Empty
    // when the source declares no externs (every existing
    // pre-FF6 test fixture).
    std::vector<HirExternRecord> externDecls;

    // `hir` is declared first so the maps bind to the constructed module.
    CstToHirResult(Hir h, HirLiteralPool lp)
        : hir(std::move(h)), sourceMap(hir), linkageMap(hir),
          mutabilityMap(hir), volatileMap(hir), literalPool(std::move(lp)) {}

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
