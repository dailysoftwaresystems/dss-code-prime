#pragma once

#include "core/export.hpp"
#include "hir/hir.hpp"
#include "hir/hir_attrs.hpp"                 // HirSourceMap
#include "hir/hir_literal_pool.hpp"

#include <memory>

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

struct DSS_EXPORT CstToHirResult {
    Hir            hir;
    HirSourceMap   sourceMap;     // bound to `hir` — must stay at a stable address
    HirLiteralPool literalPool;   // decoded literal values, indexed by literalIndex
    // True iff neither lowering nor the verify-on-load pass emitted an
    // Error-severity diagnostic (delta-computed, so prior diagnostics on the
    // shared reporter don't taint the verdict).
    bool           ok = false;

    // `hir` is declared first so the maps bind to the constructed module.
    CstToHirResult(Hir h, HirLiteralPool lp)
        : hir(std::move(h)), sourceMap(hir), literalPool(std::move(lp)) {}

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
