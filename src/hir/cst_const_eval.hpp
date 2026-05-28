#pragma once

#include "core/export.hpp"
#include "core/types/number_decode.hpp"
#include "hir/const_eval.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <unordered_set>

// CST-side const-eval engine (plan 12.5 §0.2 D6). Companion to the HIR-
// side `evaluateConstant` — same `ConstEvalResult` return shape, same
// arithmetic core (via `const_eval_arith.hpp`), but consumes a CST
// (`Tree` + `NodeId`) directly instead of HIR. Used by semantic Pass 1
// (`int a[N+1]` array length), Pass 1.5 (`enum E { A = 1+1 }`), and the
// HIR-lowering brace-init site (`{ [1+1] = 5 }` index designator) —
// every site that needs a compile-time integer BEFORE HIR exists.
//
// Lives in the `hir` library because it returns `HirLiteralValue` and
// shares the arithmetic engine with the HIR walker. Takes no HIR, no
// TypeInterner, no SemanticModel — pure CST + config + callbacks.
//
// Coverage (cycle 1):
//   - integer literal token (decoded via `decodeInteger`)
//   - wrapper rules (any single-meaningful-child internal — descends)
//   - parenthesized expressions (a wrapper-shape special case)
//   - UnaryOp (cfg.unaryExprRule + token in cfg.unaryOps)
//   - BinaryOp (cfg.binaryExprRule + token in cfg.binaryOps)
//   - LogicalAnd / LogicalOr (operator name is "And" / "Or" — short-
//     circuit semantics matching the HIR walker)
//   - Ternary (cfg.ternaryExprRule when present)
//   - identifier Ref (when `resolveSymbolInit` is supplied — recurses
//     into the symbol's CST init expression with cycle detection)

namespace dss {

class GrammarSchema;
class Tree;

// Caller-supplied identifier resolver: maps a CST identifier-token
// node to the CST `NodeId` of its defining expression (e.g. the `5`
// CST node in `const int N = 5;`). Return `nullopt` when the name
// doesn't refer to a compile-time constant. The callback receives the
// identifier-token CST node so it can dispatch via either:
//   - semantic-time scope-chain walking (read `tree.text(identTok)`),
//   - frozen-SemanticModel lookup (`model.symbolAt(identTok)`).
// The engine guarantees `identTok` is a token CST node. Closure-
// carrying state goes through the std::function capture. Cycle safety:
// the engine tracks a per-call visited-name set keyed on identifier
// text so `const int a = b; const int b = a;` surfaces
// `NotAConstantExpression` at the second encounter.
using CstSymbolInitResolver =
    std::function<std::optional<NodeId>(NodeId)>;

struct CstEvalEnvironment {
    CstSymbolInitResolver resolveSymbolInit{};
};

// Static recognition context. All fields are non-owning references
// to caller-owned state. The engine reads them per-call but never
// mutates them; safe to construct once at consumer init and reuse
// across many `evaluateConstantCst` calls.
struct CstEvalContext {
    Tree const&                              tree;
    GrammarSchema const&                     schema;
    // Tokens that decode as integer literals (`decodeInteger`-eligible).
    // The set is small (1–5 entries per language) so the hash-lookup
    // cost is negligible. Built once by the caller from
    // `SemanticConfig::literalTypes` filtered to integer cores.
    std::unordered_set<std::uint32_t> const& integerLiteralTokens;
    // May be null (matches `decodeInteger`'s nullable contract); a
    // schema without a configured numeric-literal style still parses
    // bare digit strings.
    NumberStyle const*                       numberStyle = nullptr;
};

// Evaluate `expr` to a compile-time `HirLiteralValue`. Pure function;
// no diagnostics, no Tree mutation. Returns the same
// `ConstEvalResult` shape the HIR-side engine uses so consumers can
// share diagnostic-mapping code paths.
[[nodiscard]] DSS_EXPORT ConstEvalResult
evaluateConstantCst(NodeId               expr,
                    CstEvalContext const& ctx,
                    CstEvalEnvironment   env     = {},
                    EvalOptions          options = {});

} // namespace dss
