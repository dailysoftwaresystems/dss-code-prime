#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"               // CompilationUnitId
#include "core/types/type_lattice/type_interner.hpp" // TypeInterner (by-value in the parse result)
#include "hir/hir.hpp"                              // Hir
#include "hir/hir_attrs.hpp"                        // the five HirAttribute<T> side-table aliases
#include "hir/hir_literal_pool.hpp"                 // HirLiteralPool (literal values)

#include <memory>
#include <string>
#include <string_view>
#include <vector>

// HIR text format `.dsshir` (HR7) — a round-trippable, human-readable serialization
// of a frozen `Hir`. The format is the debug + test-fixture surface for the whole
// HIR pipeline: `emitHir` renders a module to text, `parseHir` rebuilds the module
// (re-interning types, re-registering extension kinds/ops/intrinsics, repopulating
// the side-tables) and runs `HirVerifier` on load. The contract is byte-identical
// round-trip: `emitHir(parseHir(emitHir(h)))` reproduces the same bytes (plan §5).
//
// What lives where (and why the API takes a context):
//   - The node tree, every node's `typeId`, `flags`, payload, and the three
//     extension registries are IN the `Hir`, so they always round-trip.
//   - Types are CU-ephemeral `TypeId`s; the text renders them STRUCTURALLY
//     (`i64`, `ptr<i64>`, `fn(i32)->void`), so emit/parse both need a
//     `TypeInterner` to decode / re-intern — exactly as `HirVerifier` takes one.
//   - Symbol names are NOT in the `Hir` (a node carries only an opaque `SymbolId`);
//     they are first-class file content sourced from an injected name table on
//     emit, and reconstructed into `HirParseResult::symbolNames` on parse so a
//     re-emit reproduces them. Absent a name table, the emitter writes a stable
//     synthetic handle (`%s7`) and the file is still self-contained + round-trips.
//   - Literal VALUES live in a `HirLiteralPool` OUTSIDE the `Hir` (the node
//     carries only its pool index in `payload`). Like the side-tables, emit
//     takes the pool by pointer and parse hands a rebuilt one back. When a pool
//     is supplied, a `Literal` renders its VALUE inline (`lit int 42 : i32`,
//     `lit str "hi" : arr<char,3>`) so the value round-trips; absent a pool it
//     falls back to the bare index form (`lit #<index> : <type>`) — still
//     self-contained and re-parseable, just value-less (used by hand-built test
//     modules that have no pool).
//   - The five per-node side-tables (source-loc / ffi / shader / transpile / diag)
//     live OUTSIDE the `Hir`, so emit takes them by pointer and parse hands them
//     back, bound to the rebuilt module.

namespace dss {

class DiagnosticReporter;

// ── HirTextContext ────────────────────────────────────────────────────────────
//
// Non-owning enrichment for the emitter, mirroring `HirVerifier`'s optional
// injections (it must not outlive the objects it points at). Every field is
// optional; a fully-null context still produces a complete, self-contained,
// re-parseable file (synthetic symbol handles, and `?` types with a warning when
// the interner is missing).
struct DSS_EXPORT HirTextContext {
    // Decodes each node's `TypeId` into structural text. The real pipeline always
    // supplies the interner the semantic phase produced. Its `owner()` must match
    // the CU the module's `TypeId`s were interned against (the cross-arena guard
    // enforces this on first decode).
    TypeInterner const* interner = nullptr;

    // SymbolId.v → human name. Index 0 is the invalid-symbol slot; an id past the
    // end (or an empty entry) falls back to the synthetic `%s<v>` handle. A
    // production caller fills this from the CU's symbol table; a unit test may
    // leave it null.
    std::vector<std::string> const* symbolNames = nullptr;

    // Decoded literal values, indexed by a `Literal` node's `payload`. When set,
    // each literal renders its value inline; null ⇒ the bare `#<index>` form.
    // The lowering supplies `&CstToHirResult::literalPool`.
    HirLiteralPool const* literalPool = nullptr;

    // The five side-tables to serialize. Null = nothing of that kind is emitted.
    HirSourceMap     const* sourceMap     = nullptr;
    HirFfiMap        const* ffiMap        = nullptr;
    HirShaderMap     const* shaderMap     = nullptr;
    HirTranspileMap  const* transpileMap  = nullptr;
    HirDiagnosticMap const* diagnosticMap = nullptr;
};

// Serialize `hir` to canonical `.dsshir` text. Pure function of (hir, ctx): the
// canonical-format rules apply unconditionally so the output is always ready to
// round-trip. Internal-consistency problems (a typed node with no interner to
// decode it) are reported into `reporter` (Warning) and rendered as `?`; the call
// never aborts and never throws.
[[nodiscard]] DSS_EXPORT std::string emitHir(Hir const& hir, HirTextContext const& ctx,
                                             DiagnosticReporter& reporter);

// ── HirParseResult ────────────────────────────────────────────────────────────
//
// The product of a parse. Heap-allocated and returned by `unique_ptr` because the
// five side-table maps bind to `&hir` (an `ArenaAttribute` holds a raw arena
// pointer): a stable heap address keeps those pointers valid, which a value-moved
// struct could not guarantee. Access the module as `result->hir`.
//
// `interner` owns the types re-interned from the text; pass `&result->interner`
// back into a follow-up `emitHir` (with `&result->symbolNames`) to reproduce the
// source bytes. The maps are populated from the text's inline `@…` annotations.
struct DSS_EXPORT HirParseResult {
    Hir                      hir;
    TypeInterner             interner;
    std::vector<std::string> symbolNames;   // SymbolId.v → name; slot 0 unused

    HirSourceMap     sourceMap;
    HirFfiMap        ffiMap;
    HirShaderMap     shaderMap;
    HirTranspileMap  transpileMap;
    HirDiagnosticMap diagnosticMap;
    // Literal values rebuilt from the text's inline `lit <value>` forms (empty
    // when the source used the bare `#<index>` form). Indexed by `payload`.
    HirLiteralPool   literalPool;

    // True iff neither the parse nor the verify-on-load pass emitted an
    // Error-severity diagnostic (computed by delta on the reporter, so prior
    // diagnostics don't taint the verdict).
    bool ok = false;

    // Members init in declaration order: `hir` first, so the maps bind to the
    // already-constructed module. Non-copyable/movable (the maps' arena pointer);
    // it only ever lives behind the returned `unique_ptr`.
    HirParseResult(Hir h, TypeInterner ti, std::vector<std::string> names)
        : hir(std::move(h)), interner(std::move(ti)), symbolNames(std::move(names)),
          sourceMap(hir), ffiMap(hir), shaderMap(hir), transpileMap(hir),
          diagnosticMap(hir) {}

    HirParseResult(HirParseResult const&)            = delete;
    HirParseResult& operator=(HirParseResult const&) = delete;
    HirParseResult(HirParseResult&&)                 = delete;
    HirParseResult& operator=(HirParseResult&&)      = delete;
};

// Parse `.dsshir` text into a frozen module + its side-tables, then run
// `HirVerifier` on the result (verify-on-load). All parse and verify diagnostics
// go to `reporter`; `result->ok` reflects whether any were Error-severity. Types
// are re-interned into a fresh interner tagged with `cuId`. Collect-all: a
// malformed token is reported and parsing recovers to the next construct rather
// than aborting. On an unrecoverable header error the returned module is empty.
[[nodiscard]] DSS_EXPORT std::unique_ptr<HirParseResult> parseHir(
    std::string_view text, CompilationUnitId cuId, DiagnosticReporter& reporter);

} // namespace dss
