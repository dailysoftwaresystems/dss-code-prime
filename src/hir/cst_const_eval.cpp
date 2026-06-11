#include "hir/cst_const_eval.hpp"

#include "core/types/decl_prefix_strip.hpp"   // declRoleChildren (shared specifier-prefix strip)
#include "core/types/grammar_schema.hpp"
#include "core/types/hir_lowering_config.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/tree.hpp"
#include "hir/const_eval_arith.hpp"
#include "hir/hir_op.hpp"

#include <cassert>
#include <limits>
#include <string>
#include <unordered_set>

namespace dss {

namespace {

using detail::applyBinaryFloat;
using detail::applyBinaryInt;
using detail::applyUnaryFloat;
using detail::applyUnaryInt;
using detail::asBool;
using detail::asInt64;
using detail::ceFail;
using detail::ceOk;
using detail::isFloatValue;
using detail::makeBoolLiteral;

[[nodiscard]] inline ConstEvalResult fail(ConstEvalFailure why, NodeId blamed) {
    return ceFail(why, HirNodeId{blamed.v});
}
[[nodiscard]] inline ConstEvalResult ok(HirLiteralValue v) {
    return ceOk(std::move(v));
}

[[nodiscard]] std::optional<HirOpKind> opFromName(std::string const& s) {
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(HirOpKind::Count_); ++i) {
        auto const op = static_cast<HirOpKind>(i);
        if (opName(op) == s) return op;
    }
    return std::nullopt;
}

// Non-EmptySpace children — the same indexing convention semantic Pass
// 1.5 and HIR lowering use. Duplicated here (rather than calling into
// the semantic library) to keep the cst_const_eval engine self-contained
// in the `hir` library — no cross-library dep beyond `core`.
[[nodiscard]] std::vector<NodeId>
visibleChildren(Tree const& tree, NodeId parent) {
    std::vector<NodeId> out;
    for (auto const& child : tree.children(parent)) {
        if (!isEmptySpace(tree.flags(child))) out.push_back(child);
    }
    return out;
}

// Map an `HirOperatorEntry` (token-keyed) for the given token kind in
// the supplied table. Linear scan; tables are ≤ ~16 entries.
[[nodiscard]] HirOperatorEntry const*
opEntryFor(std::vector<HirOperatorEntry> const& table, SchemaTokenId tok) {
    for (auto const& e : table) {
        if (e.token.v == tok.v) return &e;
    }
    return nullptr;
}

// Internal recursive impl. `visitedInitNodes` carries the per-call
// cycle-detection set, keyed on the RESOLVED init-expression NodeId
// (NOT on identifier text — text-keyed detection produces
// false-positive cycles under shadowing: outer `const X=1; const Y=X+1;`
// + inner `const X=Y;` would trigger a fake cycle on the outer X
// when evaluating the inner X's init. Keying on init-expression
// identity sidesteps shadowing because each distinct symbol
// declaration has its own init NodeId.).
[[nodiscard]] ConstEvalResult
evalImpl(NodeId                              expr,
         CstEvalContext const&               ctx,
         CstEvalEnvironment const&           env,
         EvalOptions const&                  options,
         std::uint32_t                       currentScopeOpaque,
         std::unordered_set<std::uint32_t> & visitedInitNodes) {
    if (!expr.valid()) return fail(ConstEvalFailure::NotAConstantExpression, expr);
    Tree const& tree = ctx.tree;
    HirLoweringConfig const& cfg = ctx.schema.hirLowering();

    // ── Token leaves ────────────────────────────────────────────────
    if (tree.kind(expr) == NodeKind::Token) {
        SchemaTokenId const tk = tree.tokenKind(expr);
        if (ctx.integerLiteralTokens.contains(tk.v)) {
            auto iv = decodeInteger(tree.text(expr), ctx.numberStyle);
            if (!iv.has_value()) {
                // Decode failure (overflow / malformed text) — not a
                // compile-time integer; surfaces NotAConstantExpression
                // so callers can route to their language-specific
                // "out-of-range / malformed-literal" diagnostic.
                return fail(ConstEvalFailure::NotAConstantExpression, expr);
            }
            HirLiteralValue lv;
            lv.core  = TypeKind::I32;  // see header note: default core
                                       // suffices for the int64-arm value;
                                       // consumers use `.value`, not `.core`.
            lv.value = static_cast<std::int64_t>(*iv);
            return ok(std::move(lv));
        }
        // Identifier-Ref path: if the caller supplied a resolver,
        // pass the identifier-token NodeId and recurse into the
        // resolved init expression. Cycle detection is keyed on the
        // RESOLVED init-NodeId (not on identifier text) — shadowing
        // means the same name can refer to different symbols across
        // scopes; text-keying would false-positive a cycle on
        // legitimate cross-scope ref chains.
        if (env.resolveSymbolInit) {
            assert(tree.kind(expr) == NodeKind::Token);
            auto resolved = env.resolveSymbolInit(expr, currentScopeOpaque);
            if (!resolved.has_value() || !resolved->initExpr.valid()) {
                return fail(ConstEvalFailure::NotAConstantExpression, expr);
            }
            if (visitedInitNodes.contains(resolved->initExpr.v)) {
                return fail(ConstEvalFailure::NotAConstantExpression, expr);
            }
            visitedInitNodes.insert(resolved->initExpr.v);
            // Recurse with the SYMBOL's scope (resolved->initScopeOpaque),
            // not the original use-site scope — closes D7 cross-scope
            // shadowing.
            ConstEvalResult inner = evalImpl(resolved->initExpr, ctx, env, options,
                                             resolved->initScopeOpaque, visitedInitNodes);
            visitedInitNodes.erase(resolved->initExpr.v);
            if (!inner.value.has_value()) inner.blamedNode = HirNodeId{expr.v};
            return inner;
        }
        return fail(ConstEvalFailure::NotAConstantExpression, expr);
    }

    // ── Internal nodes — dispatch by rule ───────────────────────────
    if (tree.kind(expr) != NodeKind::Internal) {
        return fail(ConstEvalFailure::NotAConstantExpression, expr);
    }
    RuleId const rule = tree.rule(expr);
    auto kids = visibleChildren(tree, expr);

    // Binary expression: [lhs (internal), OP-token, rhs (internal)].
    if (cfg.binaryExprRule.valid() && rule.v == cfg.binaryExprRule.v) {
        NodeId lhsN{}, rhsN{}, opTok{};
        for (NodeId c : kids) {
            if (tree.kind(c) == NodeKind::Token) {
                if (!opTok.valid()) opTok = c;
            } else if (!lhsN.valid()) lhsN = c;
            else if (!rhsN.valid()) rhsN = c;
        }
        if (!opTok.valid() || !lhsN.valid() || !rhsN.valid()) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        HirOperatorEntry const* e = opEntryFor(cfg.binaryOps, tree.tokenKind(opTok));
        if (e == nullptr) {
            return fail(ConstEvalFailure::UnsupportedOperator, expr);
        }
        // Assignment / compound-assign are statement-like — never
        // const-foldable in const-expr position.
        if (e->target == "Assign" || !e->compoundBase.empty()) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        // LogicalAnd / LogicalOr: short-circuit; do NOT recurse into
        // rhs until lhs's truthiness is known. Matches the HIR walker's
        // C99 semantics exactly.
        if (e->target == "LogicalAnd" || e->target == "LogicalOr") {
            ConstEvalResult a = evalImpl(lhsN, ctx, env, options, currentScopeOpaque, visitedInitNodes);
            if (!a.value.has_value()) return a;
            auto aIsTrueOpt = asBool(*a.value, options.allowFloat);
            if (!aIsTrueOpt.has_value()) {
                return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
            }
            bool const aIsTrue = *aIsTrueOpt;
            bool const isAnd = (e->target == "LogicalAnd");
            bool const shortCircuits = isAnd ? !aIsTrue : aIsTrue;
            if (shortCircuits) return ok(makeBoolLiteral(aIsTrue ? 1 : 0));
            ConstEvalResult b = evalImpl(rhsN, ctx, env, options, currentScopeOpaque, visitedInitNodes);
            if (!b.value.has_value()) return b;
            auto bIsTrueOpt = asBool(*b.value, options.allowFloat);
            if (!bIsTrueOpt.has_value()) {
                return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
            }
            return ok(makeBoolLiteral(*bIsTrueOpt ? 1 : 0));
        }
        // Plain arithmetic / bitwise / comparison.
        auto opK = opFromName(e->target);
        if (!opK.has_value()) {
            return fail(ConstEvalFailure::UnsupportedOperator, expr);
        }
        ConstEvalResult a = evalImpl(lhsN, ctx, env, options, currentScopeOpaque, visitedInitNodes);
        if (!a.value.has_value()) return a;
        ConstEvalResult b = evalImpl(rhsN, ctx, env, options, currentScopeOpaque, visitedInitNodes);
        if (!b.value.has_value()) return b;
        bool const eitherFloat = isFloatValue(*a.value) || isFloatValue(*b.value);
        if (eitherFloat) {
            if (!options.allowFloat) {
                return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
            }
            ConstEvalFailure why = ConstEvalFailure::None;
            if (auto folded = applyBinaryFloat(*opK, *a.value, *b.value, why);
                folded.has_value()) {
                return ok(std::move(*folded));
            }
            return fail(why != ConstEvalFailure::None
                            ? why : ConstEvalFailure::UnsupportedOperator,
                        expr);
        }
        if (!asInt64(*a.value).has_value() || !asInt64(*b.value).has_value()) {
            return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        }
        ConstEvalFailure why = ConstEvalFailure::None;
        if (auto folded = applyBinaryInt(*opK, *a.value, *b.value, options, why);
            folded.has_value()) {
            // Tag the result core. Comparisons → Bool; arithmetic
            // inherits the LHS's core (no TypeInterner / commonType
            // available at the CST level — semantic-time consumers
            // only read `.value`, not `.core`).
            if (isComparison(*opK)) folded->core = TypeKind::Bool;
            return ok(std::move(*folded));
        }
        if (why != ConstEvalFailure::None) return fail(why, expr);
        return fail(ConstEvalFailure::UnsupportedOperator, expr);
    }

    // Unary expression: [OP-token, operand (internal)].
    if (cfg.unaryExprRule.valid() && rule.v == cfg.unaryExprRule.v) {
        NodeId opTok{}, operandN{};
        for (NodeId c : kids) {
            if (tree.kind(c) == NodeKind::Token) {
                if (!opTok.valid()) opTok = c;
            } else if (!operandN.valid()) operandN = c;
        }
        if (!opTok.valid() || !operandN.valid()) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        HirOperatorEntry const* e = opEntryFor(cfg.unaryOps, tree.tokenKind(opTok));
        if (e == nullptr) {
            return fail(ConstEvalFailure::UnsupportedOperator, expr);
        }
        // AddressOf / Deref aren't const-foldable.
        if (e->target == "AddressOf" || e->target == "Deref") {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        auto opK = opFromName(e->target);
        if (!opK.has_value()) {
            return fail(ConstEvalFailure::UnsupportedOperator, expr);
        }
        ConstEvalResult inner = evalImpl(operandN, ctx, env, options, currentScopeOpaque, visitedInitNodes);
        if (!inner.value.has_value()) return inner;
        if (isFloatValue(*inner.value)) {
            if (!options.allowFloat) {
                return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
            }
            ConstEvalFailure why = ConstEvalFailure::None;
            if (auto folded = applyUnaryFloat(*opK, *inner.value, why);
                folded.has_value()) {
                return ok(std::move(*folded));
            }
            return fail(why != ConstEvalFailure::None
                            ? why : ConstEvalFailure::UnsupportedOperator,
                        expr);
        }
        if (!asInt64(*inner.value).has_value()) {
            return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        }
        if (auto folded = applyUnaryInt(*opK, *inner.value); folded.has_value()) {
            return ok(std::move(*folded));
        }
        return fail(ConstEvalFailure::UnsupportedOperator, expr);
    }

    // Ternary expression: visible children [cond, '?', then, ':', else].
    if (cfg.ternaryExprRule.valid() && rule.v == cfg.ternaryExprRule.v) {
        NodeId condN{}, thenN{}, elseN{};
        for (NodeId c : kids) {
            if (tree.kind(c) == NodeKind::Token) continue;
            if (!condN.valid()) condN = c;
            else if (!thenN.valid()) thenN = c;
            else if (!elseN.valid()) elseN = c;
        }
        if (!condN.valid() || !thenN.valid() || !elseN.valid()) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        ConstEvalResult cond = evalImpl(condN, ctx, env, options, currentScopeOpaque, visitedInitNodes);
        if (!cond.value.has_value()) return cond;
        auto condTrueOpt = asBool(*cond.value, options.allowFloat);
        if (!condTrueOpt.has_value()) {
            return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        }
        NodeId const selected = *condTrueOpt ? thenN : elseN;
        return evalImpl(selected, ctx, env, options, currentScopeOpaque, visitedInitNodes);
    }

    // Wrapper rule: any internal node with exactly one meaningful child
    // (an Internal) and any number of pure tokens (parens, separators)
    // is treated as a transparent wrapper. Mirrors the existing peeling
    // pattern in `constIntLength` / `resolveIndexDesignatorLiteral`.
    NodeId onlyInternal{};
    int    internalCount = 0;
    for (NodeId c : kids) {
        if (tree.kind(c) == NodeKind::Internal) {
            ++internalCount;
            onlyInternal = c;
        }
    }
    if (internalCount == 1) {
        return evalImpl(onlyInternal, ctx, env, options, currentScopeOpaque, visitedInitNodes);
    }
    // Zero internals: a single token-leaf wrapper (e.g. an `operand`
    // rule whose sole content is an integer-literal token or an
    // identifier-Ref token). Recurse into the sole token regardless
    // of kind — the Token-leaf branch above dispatches integer-
    // literal vs identifier-Ref vs other.
    if (internalCount == 0) {
        NodeId onlyTok{};
        int tokCount = 0;
        for (NodeId c : kids) {
            if (tree.kind(c) == NodeKind::Token) {
                ++tokCount;
                onlyTok = c;
            }
        }
        if (tokCount == 1) {
            return evalImpl(onlyTok, ctx, env, options, currentScopeOpaque, visitedInitNodes);
        }
    }
    return fail(ConstEvalFailure::NotAConstantExpression, expr);
}

} // namespace

ConstEvalResult evaluateConstantCst(NodeId                expr,
                                    CstEvalContext const& ctx,
                                    CstEvalEnvironment    env,
                                    EvalOptions           options,
                                    std::uint32_t         initialScopeOpaque) {
    std::unordered_set<std::uint32_t> visitedInitNodes;
    return evalImpl(expr, ctx, env, options, initialScopeOpaque, visitedInitNodes);
}

std::optional<std::int64_t> asInt64Bridge(HirLiteralValue const& v) noexcept {
    return detail::asInt64(v);
}

std::optional<NodeId>
findInitExprInDecl(Tree const& tree, DeclarationRule const& decl,
                   NodeId declRuleNode) {
    if (!declRuleNode.valid()) return std::nullopt;
    // D-DECL-SPECIFIER-PREFIX-SUBSTRATE: role children = visible children with
    // a leading declaration-specifier prefix stripped — the SHARED
    // `declRoleChildren` (core/types/decl_prefix_strip.hpp,
    // D-DECL-PREFIX-STRIP-SHARED-HELPER) — so both the positional `initChild`
    // path and the role-fallback loop below resolve against the same
    // prefix-free child numbering the analyzer minted symbols against.
    // Otherwise a `static`/`__attribute__` prefix node shifts initChild AND
    // (being a non-skip-listed Internal child) would be wrongly returned as
    // the init by the fallback.
    auto kids = declRoleChildren(tree, declRuleNode, decl);
    // Explicit positional `initChild` wins when configured.
    if (decl.initChild.has_value()) {
        if (*decl.initChild >= kids.size()) return std::nullopt;
        return kids[*decl.initChild];
    }
    // Role-based fallback: the init is the Internal child that is
    // NOT the type / name / params / body / array-suffix. The full
    // role-skip list closes the latent bug where a `const`-qualified
    // function decl's `funcDefTail` body would have been mistaken
    // for the init expression. **Maintainer note**: adding any new
    // positional-child role to `DeclarationRule` (a future
    // `templateParamsChild`, `attributesChild`, `qualifiersChild`,
    // …) MUST extend this skip list — otherwise the first Internal
    // child under that new role becomes a silent false-positive init.
    RuleId const arraySufRule = decl.arraySuffix.has_value()
        ? decl.arraySuffix->rule : RuleId{};
    auto positional = [&](std::optional<std::uint32_t> pos, std::uint32_t i) {
        return pos.has_value() && *pos == i;
    };
    for (std::uint32_t i = 0; i < kids.size(); ++i) {
        if (tree.kind(kids[i]) != NodeKind::Internal) continue;
        if (positional(decl.typeChild,   i)) continue;
        if (positional(decl.nameChild,   i)) continue;
        if (positional(decl.paramsChild, i)) continue;
        if (positional(decl.bodyChild,   i)) continue;
        if (arraySufRule.valid() && tree.rule(kids[i]) == arraySufRule) continue;
        return kids[i];
    }
    return std::nullopt;
}

} // namespace dss
