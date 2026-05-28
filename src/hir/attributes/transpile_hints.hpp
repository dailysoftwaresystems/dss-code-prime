#pragma once

#include <cstdint>
#include <string>

// Transpile-hint side-table value (HR5). Attached per-node via
// `HirAttribute<TranspileHint>` (aliased `HirTranspileMap` in hir_attrs.hpp) to
// steer HIR→HIR source-to-source translation (`10-source-translation-plan`) when
// the language-pair map's default mapping for a node would be a poor fit for the
// target. A hint is advisory: the translator falls back to the pair map's default
// whenever no hint applies.
//
// Population is the source language's lowering (HR8/HR9, reading the `hirLowering`
// config) and/or the transpile pass itself; HR5 establishes the home + shape. No
// `Hir` dependency — consumers bind it as `HirAttribute<TranspileHint>`.

namespace dss {

// A structural-idiom preference for value-/control-shaped nodes. The translator
// honours it only when the target language can express the idiom; otherwise it
// silently keeps the default lowering. `Default` = no preference.
enum class TranspileIdiom : std::uint8_t {
    Default = 0,
    EarlyReturn,   // prefer guarded early returns over a trailing nested else
    GuardClause,   // prefer a negated guard + early exit over wrapping the body
    TernaryExpr,   // prefer `cond ? a : b` over an if/else assigning a value
    RangeFor,      // prefer a range/for-each loop over an index-counted loop
    WhileLoop,     // prefer a `while` over a `for` with empty clauses
};

struct TranspileHint {
    // Target language this hint applies to (matches the `.map.json` pair target,
    // e.g. "javascript"). Empty = applies to every transpile target.
    std::string targetLanguage;

    // Explicit override of the target construct, named in the target language's
    // kind/idiom vocabulary (e.g. "JsDot" instead of the default `MemberAccess`
    // mapping). Empty = use the pair map's default mapping for this node's kind.
    std::string overrideKind;

    // Structural idiom preference (see `TranspileIdiom`).
    TranspileIdiom idiom = TranspileIdiom::Default;
};

} // namespace dss
