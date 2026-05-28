#pragma once

#include "core/types/source_span.hpp"   // SourceSpan
#include "core/types/strong_ids.hpp"     // BufferId / InvalidBuffer

// Source-provenance side-table value (HR5). Attached per-node via
// `HirAttribute<HirSourceLoc>` (aliased `HirSourceMap` in hir_attrs.hpp) to
// preserve where each HIR node came from: the byte range AND the originating
// source buffer.
//
// Why bundle the buffer rather than store a bare `SourceSpan`? A `SourceSpan` is
// only a byte range — it does not name a file. A diagnostic (`ParseDiagnostic`)
// needs both a `BufferId` and a `SourceSpan` to render a location, and a single
// HIR module can mix nodes from DIFFERENT source files (a multi-language CU lowers
// several files into one HIR program — HR11 / CU5), so a per-module buffer would
// be wrong. `HirSourceLoc` keeps each node self-describing. It mirrors
// `RelatedLocation` (parse_diagnostic.hpp), which pairs `{BufferId, SourceSpan}`
// for the same reason.
//
// Population is the CST→HIR lowering's job (HR8): each emitted node records the
// span of the CST node it lowered from; synthetic structured-CF scaffolding
// (plan §2.3) inherits the span of the construct that spawned it (the
// `SourceSpan::join` discipline `TreeBuilder` already uses for synthetic Missing
// parents), so the real pipeline leaves no node unmapped. HR5 establishes the
// home + the `HirSourceMap` alias; this header has no `Hir` dependency on purpose
// — consumers bind it as `HirAttribute<HirSourceLoc>`.

namespace dss {

struct HirSourceLoc {
    // Originating source file. `InvalidBuffer` means "no known buffer" — paired
    // with an empty span it is the honest "this node has no source location"
    // value (a verifier run without a source map, or a node lowering left
    // unmapped). The buffer resolves through a `BufferRegistry` at render time.
    BufferId buffer = InvalidBuffer;

    // Byte range within `buffer`. Defaults to empty; `SourceSpan` is factory-only
    // so there is no other zero-state to spell.
    SourceSpan span = SourceSpan::empty(0);
};

} // namespace dss
