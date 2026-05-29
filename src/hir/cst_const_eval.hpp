#pragma once

#include "core/export.hpp"
#include "core/types/number_decode.hpp"
#include "hir/const_eval.hpp"
#include "hir/hir_literal_pool.hpp"

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
//   - LogicalAnd / LogicalOr (operator-target name is "LogicalAnd"
//     / "LogicalOr" — short-circuit semantics matching the HIR walker)
//   - Ternary (cfg.ternaryExprRule when present)
//   - identifier Ref (when `resolveSymbolInit` is supplied — recurses
//     into the symbol's CST init expression with cycle detection)

namespace dss {

class GrammarSchema;
class Tree;

// Resolved-symbol record returned by the resolver. The engine
// recurses into `initExpr` and threads `initScopeOpaque` as the
// current scope context for any further identifier lookups inside
// that subtree. The opaque-scope handle is meaningful only to the
// resolver — the engine treats it as a uint32_t cookie carried
// through recursion.
struct CstResolvedSymbol {
    NodeId        initExpr{};
    std::uint32_t initScopeOpaque = 0;
};

// Caller-supplied identifier resolver: maps a CST identifier-token
// node + the current scope context to the CST `NodeId` of its
// defining expression PLUS the scope-context to use when recursing
// into it. Return `nullopt` when the name doesn't refer to a
// compile-time constant.
//
// Scope-context tracking (plan 12.5 §0.2 D7): the engine threads
// the resolver-supplied `initScopeOpaque` through recursion, so
// when `const X=1; const Y=X+1; void f(){ const X=Y; int a[X+1]; }`
// is evaluated, the inner-X's lookup uses the function scope (finds
// the inner X → recurses into Y), then Y's init `X+1` is evaluated
// with MODULE scope (where Y was declared) so the inner X is no
// longer in view — the outer X correctly resolves to 1.
//
// The engine guarantees `identTok` is a token CST node. Closure-
// carrying state goes through the std::function capture. Cycle
// safety: visited-set keyed on the RESOLVED init-NodeId so a chain
// like `const a=b; const b=a;` still trips at the second encounter.
using CstSymbolInitResolver =
    std::function<std::optional<CstResolvedSymbol>(NodeId, std::uint32_t)>;

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
evaluateConstantCst(NodeId                expr,
                    CstEvalContext const& ctx,
                    CstEvalEnvironment    env                = {},
                    EvalOptions           options            = {},
                    std::uint32_t         initialScopeOpaque = 0);

// Bridge a folded `HirLiteralValue` into a plain `int64_t`. Handles
// the three numeric variant arms (int64 / uint64 / bool); returns
// nullopt for non-numeric arms or for unsigned values that exceed
// int64's positive range. Shared by all 3 consumer callsites (array
// length, enumerator value, index designator) — was the byte-for-byte
// repeated post-fold extraction in each.
[[nodiscard]] DSS_EXPORT std::optional<std::int64_t>
asInt64Bridge(HirLiteralValue const& v) noexcept;

// Find the init-expression CST node inside a declaration rule node.
// Tries `DeclarationRule.initChild` (explicit positional index) first;
// falls back to role-based discovery — the init is the Internal child
// that is NOT the type / name / params / body / array-suffix. Shared
// by every CST-side Ref resolver (semantic-side scope walker AND
// HIR-lowering's frozen-model lookup). Callers must already have
// confirmed isConst / tree match.
//
// Forward-declared `DeclarationRule` is included via core/types; the
// helper definition resolves it from the loaded schema.
struct DeclarationRule;
[[nodiscard]] DSS_EXPORT std::optional<NodeId>
findInitExprInDecl(Tree const& tree, DeclarationRule const& decl,
                   NodeId declRuleNode);

} // namespace dss
