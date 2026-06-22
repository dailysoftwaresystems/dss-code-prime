#pragma once

// Shared operator-lookup seams used by BOTH the CST constant-expression
// evaluator (`cst_const_eval.cpp`) and the C-preprocessor `#if`
// integer-constant-expression evaluator (`analysis/preprocess/pp_if_eval.cpp`).
//
// These two functions bridge a CONFIG operator entry to a `HirOpKind`:
//
//   opFromName(target)  — maps an `HirOperatorEntry::target` string (e.g.
//                         "Add", "Shl", "Eq") to its core `HirOpKind`, so the
//                         shared arithmetic core (`const_eval_arith.hpp`) can
//                         fold it. The mapping is the inverse of `opName`.
//
//   opEntryFor(table, t)— finds the `HirOperatorEntry` keyed by a token kind
//                         in a `binaryOps` / `unaryOps` table (linear scan;
//                         tables are ≤ ~16 entries).
//
// Both were anonymous-namespace statics in `cst_const_eval.cpp`; they are
// lifted here VERBATIM (as `inline` free functions) so the `#if` evaluator can
// reuse the identical config→op bridge instead of duplicating it — a divergent
// copy would silently let the two evaluators disagree about which token is
// which operator. `cst_const_eval.cpp` includes this header and drops its local
// copies, so its behaviour is byte-identical.

#include "core/types/hir_lowering_config.hpp"  // HirOperatorEntry
#include "core/types/strong_ids.hpp"           // SchemaTokenId
#include "hir/hir_op.hpp"                       // HirOpKind, opName

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dss {

// Map a config operator TARGET name (the inverse of `opName`) to its core
// `HirOpKind`. Returns nullopt for a name that is not a core operator (e.g.
// "LogicalAnd"/"LogicalOr"/"Assign"/"AddressOf" — those are NOT `HirOpKind`s;
// callers handle them out of band).
[[nodiscard]] inline std::optional<HirOpKind> opFromName(std::string const& s) {
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(HirOpKind::Count_); ++i) {
        auto const op = static_cast<HirOpKind>(i);
        if (opName(op) == s) return op;
    }
    return std::nullopt;
}

// Map an `HirOperatorEntry` (token-keyed) for the given token kind in the
// supplied table. Linear scan; tables are ≤ ~16 entries.
[[nodiscard]] inline HirOperatorEntry const*
opEntryFor(std::vector<HirOperatorEntry> const& table, SchemaTokenId tok) {
    for (auto const& e : table) {
        if (e.token.v == tok.v) return &e;
    }
    return nullptr;
}

} // namespace dss
