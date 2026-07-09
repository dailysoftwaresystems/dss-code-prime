#include "hir/cst_const_eval.hpp"

#include "core/types/char_decode.hpp"         // FC17 F2: shared narrow char-literal decode
#include "core/types/decl_prefix_strip.hpp"   // declRoleChildren (shared specifier-prefix strip)
#include "core/types/declarator_walk.hpp"     // FC4: collectDeclarators / declaratorNameNode
#include "core/types/grammar_schema.hpp"
#include "core/types/hir_lowering_config.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/tree.hpp"
#include "hir/const_eval_arith.hpp"
#include "hir/const_eval_operators.hpp"   // shared opFromName / opEntryFor seams
#include "hir/hir_op.hpp"

#include <cassert>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

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

// c43 (D-CSUBSET-ADDRESS-CONSTANT-FOLD / Option A): address-value helpers.
[[nodiscard]] inline HirAddressValue const* asAddress(HirLiteralValue const& v) {
    return std::get_if<HirAddressValue>(&v.value);
}
[[nodiscard]] inline HirLiteralValue makeAddress(HirAddressValue a) {
    HirLiteralValue v;
    v.core  = TypeKind::Ptr;
    v.value = a;
    return v;
}
// Convert a folded integer to a target integer width (C 6.3.1.3): for a narrowing
// target, mask to `bits` and sign-extend if the target is signed; `bits >= 64` is
// identity. Used by the integer-target cast arm so the `(size_t)` that terminates
// the offsetof idiom (and any other const-expr int cast) folds to the right value.
[[nodiscard]] inline std::int64_t narrowIntToBits(std::int64_t v, int bits, bool isSigned) {
    if (bits >= 64 || bits <= 0) return v;
    std::uint64_t const mask = (std::uint64_t{1} << bits) - 1;
    std::uint64_t m = static_cast<std::uint64_t>(v) & mask;
    if (isSigned && (m & (std::uint64_t{1} << (bits - 1)))) m |= ~mask;  // sign-extend
    return static_cast<std::int64_t>(m);
}

// `opFromName` (config target name -> HirOpKind) now lives in the shared
// `hir/const_eval_operators.hpp` so the C-preprocessor `#if` evaluator reuses
// the identical bridge. Reachable here as `dss::opFromName` (unqualified).

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

// `opEntryFor` (token-keyed operator-entry lookup) now lives in the shared
// `hir/const_eval_operators.hpp` (see opFromName note above). Reachable here as
// `dss::opEntryFor` (unqualified).

// Forward decl: the public entry `evaluateConstantCst` drives an explicit heap
// work-stack (plan 24 Stage 6 — D-PARSE-DEEP-NEST-RECURSION-MEMORY). The per-
// node body (`evalNode` below) re-enters it for the children of every DELEGATED
// arm, so a deeply-nested sub-expression inside a shallow complex arm still
// flattens.
[[nodiscard]] ConstEvalResult
evalImpl(NodeId                              expr,
         CstEvalContext const&               ctx,
         CstEvalEnvironment const&           env,
         EvalOptions const&                  options,
         std::uint32_t                       currentScopeOpaque,
         std::unordered_set<std::uint32_t> & visitedInitNodes);

// ── Plan 24 Stage 6 — straight-line CST const-fold epilogues ───────────────
// Each `combine*` is the BYTE-IDENTICAL slice of a flattened arm AFTER its
// child operand(s) have been folded (their `ConstEvalResult`(s) passed in).
// Shared by `evalNode`'s recursive arms AND the driver's frames — ONE source of
// truth. A child-failure short-circuit returns the child's result VERBATIM,
// matching the recursive `if (!x.value.has_value()) return x;`.

// Plain-binary epilogue (lhs+rhs already folded, IN THAT ORDER — the recursive
// form folds `a = evalImpl(lhsN)` then `b = evalImpl(rhsN)`, two sequential
// statements, left-to-right). `e` is the resolved operator entry (already known
// NOT to be Assign/compound/logical — the driver only flattens plain ops; the
// logical/assign cases stay in `evalNode`). Mirrors the recursive plain-arith
// tail EXACTLY (float vs int routing, result-core tagging, failure codes).
[[nodiscard]] ConstEvalResult
combineBinaryCst(NodeId expr, HirOperatorEntry const& e, EvalOptions const& options,
                 ConstEvalResult a, ConstEvalResult b) {
    if (!a.value.has_value()) return a;
    if (!b.value.has_value()) return b;
    auto opK = opFromName(e.target);
    if (!opK.has_value()) {
        return fail(ConstEvalFailure::UnsupportedOperator, expr);
    }
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
    // c43 (D-CSUBSET-ADDRESS-CONSTANT-FOLD / Option A): address arithmetic. A
    // pointer DIFFERENCE of two NULL-relative addresses with the SAME base folds
    // to the integer byte difference — the offsetof terminator
    // `(char*)&((T*)0)->M - (char*)0` (both `char*`, so the byte difference IS the
    // member offset). `address ± integer` (`&g + N`) is a GLOBAL-init concern with
    // a real element stride — the HIR engine owns it; fail loud here rather than
    // guess a stride in this array-dimension context.
    {
        auto const* la = asAddress(*a.value);
        auto const* ra = asAddress(*b.value);
        if (la != nullptr && ra != nullptr) {
            if (e.target != "Sub" || la->base != ra->base) {
                return fail(ConstEvalFailure::NotAConstantExpression, expr);
            }
            HirLiteralValue v;
            v.core  = TypeKind::I64;
            v.value = la->byteOffset - ra->byteOffset;
            return ok(std::move(v));
        }
        if (la != nullptr || ra != nullptr) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
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

// Unary epilogue (operand already folded to `inner`). `e` is the resolved
// operator entry (already known NOT to be AddressOf/Deref). Byte-identical to
// the recursive unary tail.
[[nodiscard]] ConstEvalResult
combineUnaryCst(NodeId expr, HirOperatorEntry const& e, EvalOptions const& options,
                ConstEvalResult inner) {
    if (!inner.value.has_value()) return inner;
    auto opK = opFromName(e.target);
    if (!opK.has_value()) {
        return fail(ConstEvalFailure::UnsupportedOperator, expr);
    }
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

// Resolve a plain-binary node to its [lhsN, rhsN] operands IFF it is a binary-
// rule node whose operator is foldable AND NOT Assign/compound/logical — the
// EXACT set the driver flattens. Returns nullopt for every other shape (wrong
// rule, malformed operands, unknown operator, Assign/compound, LogicalAnd/Or),
// which then DELEGATES to `evalNode` and is handled there byte-identically (the
// logical-short-circuit + every fail-loud path). Shared by the driver's
// classifier AND `evalNode`'s plain-binary fallback so the two never diverge.
// On a flattenable hit, `outEntry` receives the resolved operator entry (so the
// epilogue need not re-look-it-up).
struct CstBinaryPlan { NodeId lhsN, rhsN; HirOperatorEntry const* entry; };
[[nodiscard]] std::optional<CstBinaryPlan>
plainBinaryPlan(Tree const& tree, HirLoweringConfig const& cfg, NodeId expr,
                std::vector<NodeId> const& kids) {
    if (!cfg.binaryExprRule.valid() || tree.rule(expr).v != cfg.binaryExprRule.v) {
        return std::nullopt;
    }
    NodeId lhsN{}, rhsN{}, opTok{};
    for (NodeId c : kids) {
        if (tree.kind(c) == NodeKind::Token) {
            if (!opTok.valid()) opTok = c;
        } else if (!lhsN.valid()) lhsN = c;
        else if (!rhsN.valid()) rhsN = c;
    }
    if (!opTok.valid() || !lhsN.valid() || !rhsN.valid()) return std::nullopt;
    HirOperatorEntry const* e = opEntryFor(cfg.binaryOps, tree.tokenKind(opTok));
    if (e == nullptr) return std::nullopt;
    // Assignment / compound-assign and the short-circuiting logical ops are NOT
    // flattened (the recursive `evalNode` owns their exact semantics).
    if (e->target == "Assign" || !e->compoundBase.empty()) return std::nullopt;
    if (e->target == "LogicalAnd" || e->target == "LogicalOr") return std::nullopt;
    return CstBinaryPlan{lhsN, rhsN, e};
}

// Resolve a unary node to its operand NodeId IFF it is a unary-rule node whose
// operator is foldable AND NOT AddressOf/Deref — the set the driver flattens.
// Returns nullopt otherwise (→ delegate, byte-identical fail-loud in evalNode).
struct CstUnaryPlan { NodeId operandN; HirOperatorEntry const* entry; };
[[nodiscard]] std::optional<CstUnaryPlan>
unaryPlan(Tree const& tree, HirLoweringConfig const& cfg, NodeId expr,
          std::vector<NodeId> const& kids) {
    if (!cfg.unaryExprRule.valid() || tree.rule(expr).v != cfg.unaryExprRule.v) {
        return std::nullopt;
    }
    NodeId opTok{}, operandN{};
    for (NodeId c : kids) {
        if (tree.kind(c) == NodeKind::Token) {
            if (!opTok.valid()) opTok = c;
        } else if (!operandN.valid()) operandN = c;
    }
    if (!opTok.valid() || !operandN.valid()) return std::nullopt;
    HirOperatorEntry const* e = opEntryFor(cfg.unaryOps, tree.tokenKind(opTok));
    if (e == nullptr) return std::nullopt;
    if (e->target == "AddressOf" || e->target == "Deref") return std::nullopt;
    return CstUnaryPlan{operandN, e};
}

// Resolve a transparent WRAPPER node to its single child to descend into (the
// deep-parens axis `((((expr))))`), IFF it is NOT one of the rule-dispatched
// arms (sizeof / binary / unary / ternary) — those are matched first in
// `evalNode` and must not be wrapper-peeled. Mirrors the recursive tail:
// exactly one meaningful Internal child → that child; else zero internals and
// exactly one token child → that token; else nullopt (→ delegate, which yields
// the same NotAConstantExpression). Shared by the driver + the recursive
// fallback so the peel decision never diverges.
[[nodiscard]] NodeId
wrapperChild(Tree const& tree, HirLoweringConfig const& cfg, NodeId expr,
             std::vector<NodeId> const& kids) {
    RuleId const rule = tree.rule(expr);
    bool const isDispatched =
        (cfg.sizeofRule.valid()      && rule.v == cfg.sizeofRule.v)      ||
        // C11/C23 6.5.3.4: `_Alignof(T)` — like sizeof, its ONE meaningful
        // Internal child is a castTypeRef the peel would wrongly descend into;
        // keep it for evalNode's alignof arm (mirror sizeof).
        (cfg.alignofRule.valid()     && rule.v == cfg.alignofRule.v)     ||
        (cfg.binaryExprRule.valid()  && rule.v == cfg.binaryExprRule.v)  ||
        (cfg.unaryExprRule.valid()   && rule.v == cfg.unaryExprRule.v)   ||
        (cfg.ternaryExprRule.valid() && rule.v == cfg.ternaryExprRule.v) ||
        // c43 (D-CSUBSET-ADDRESS-CONSTANT-FOLD): a cast / postfix-member node has
        // a type-ref or follower child the peel would mishandle — keep them for
        // evalNode's address arms (mirror sizeof).
        (cfg.castRule.valid()        && rule.v == cfg.castRule.v)        ||
        (cfg.postfixExprRule.valid() && rule.v == cfg.postfixExprRule.v);
    if (isDispatched) return NodeId{};
    NodeId onlyInternal{};
    int    internalCount = 0;
    for (NodeId c : kids) {
        if (tree.kind(c) == NodeKind::Internal) { ++internalCount; onlyInternal = c; }
    }
    if (internalCount == 1) return onlyInternal;
    if (internalCount == 0) {
        NodeId onlyTok{};
        int tokCount = 0;
        for (NodeId c : kids) {
            if (tree.kind(c) == NodeKind::Token) { ++tokCount; onlyTok = c; }
        }
        if (tokCount == 1) return onlyTok;
    }
    return NodeId{};
}

// Internal per-node fold body. `visitedInitNodes` carries the per-call
// cycle-detection set, keyed on the RESOLVED init-expression NodeId
// (NOT on identifier text — text-keyed detection produces
// false-positive cycles under shadowing: outer `const X=1; const Y=X+1;`
// + inner `const X=Y;` would trigger a fake cycle on the outer X
// when evaluating the inner X's init. Keying on init-expression
// identity sidesteps shadowing because each distinct symbol
// declaration has its own init NodeId.).
//
// Plan 24 Stage 6: the deep STRAIGHT-LINE arms (plain BinaryOp, unary, and the
// transparent WRAPPER/paren descent) are flattened onto the `evalImpl` work-
// stack driver and reach their epilogue THERE; this handler keeps them as the
// dead-via-driver recursive fallback (calling the SAME epilogues / peel helper).
// DELEGATED here (re-entering the driver for their children): the token leaves
// (integer / direct-value / init-resolver — the cycle-set Ref analog), sizeof,
// LogicalAnd/Or short-circuit, and ternary. Output-identity holds because the
// delegated arms keep their subtle evaluation-order / cycle-set / short-circuit
// semantics verbatim; only their CHILDREN re-enter the driver.
[[nodiscard]] ConstEvalResult
evalNode(NodeId                              expr,
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
        // FC17 F2 (D-CSUBSET-CONSTEXPR): a FIXED-VALUE keyword literal
        // (`true`/`false` — `literalTypes` `value:` rows) carries its
        // config-declared value; its TEXT must never be decoded as a number
        // (mirrors the CST→HIR `litFixed_` discipline, and is checked FIRST
        // for the same reason — `decodeInteger("true")` has no digits and
        // would fail where the config declares a real value). The caller
        // built the map filtered to INTEGER-VALUED cores, so a NullptrT
        // `nullptr` row is structurally absent — `nullptr` stays
        // non-foldable (loud) in every integer const-expr context.
        if (ctx.fixedValueTokens != nullptr) {
            if (auto it = ctx.fixedValueTokens->find(tk.v);
                it != ctx.fixedValueTokens->end()) {
                return ok(HirLiteralValue{it->second});
            }
        }
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
        // FC17 (D-CSUBSET-CONSTEXPR): a FLOAT literal leaf — ONLY when the
        // caller opted in by populating `floatLiteralTokens` (the float-capable
        // constexpr-initializer consumer). Every integer-required consumer
        // (array dims / enums / static_assert / designators) leaves the set
        // null, so this arm is structurally unreachable there — `int a[1.5+1.5]`
        // / `_Static_assert(1.5>1.0,"")` keep failing loud. Decodes via the
        // SAME config-aware `decodeFloat` the CST→HIR literal lowering uses
        // (suffix strip per numberStyle; a malformed/undecodable text fails
        // loud, never a silent zero).
        if (ctx.floatLiteralTokens != nullptr
            && ctx.floatLiteralTokens->contains(tk.v)) {
            bool decodeOk = true;
            double const d = decodeFloat(tree.text(expr), ctx.numberStyle, decodeOk);
            if (!decodeOk) {
                return fail(ConstEvalFailure::NotAConstantExpression, expr);
            }
            HirLiteralValue lv;
            lv.core  = TypeKind::F64;  // the fold-arithmetic core; consumers
                                       // read `.value` (see the I32 note above)
            lv.value = d;
            return ok(std::move(lv));
        }
        // Item 1 DIRECT-VALUE path: a named constant whose value is carried
        // INLINE on its symbol (an enum enumerator or a shipped-descriptor
        // constant) has no defining init-expression CST to recurse into — it
        // resolves DIRECTLY to its literal. Tried BEFORE the init-resolver so
        // `int a[CHAR_BIT]` / `int b[ENUM_VAL]` fold in const-expr position.
        if (env.resolveSymbolValue) {
            assert(tree.kind(expr) == NodeKind::Token);
            if (auto direct = env.resolveSymbolValue(expr, currentScopeOpaque)) {
                return ok(std::move(*direct));
            }
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

    // FC6: `sizeof(T)` in a const-expr context (e.g. an array dimension).
    // Dispatched by rule-id HERE — ahead of the wrapper-peel below — because a
    // sizeof node has exactly one meaningful Internal child (its type-ref /
    // operand), so the peel would otherwise transparently descend into it and
    // either reject the type-ref as non-constant OR (value form) fold the
    // operand to its VALUE instead of its size. The size itself is computed by
    // the caller's `resolveSizeof` closure (it owns the type resolver + the
    // target's layout params, which this engine must not depend on). Absent
    // closure or un-sizeable operand ⇒ NotAConstantExpression (fail loud).
    if (cfg.sizeofRule.valid() && rule.v == cfg.sizeofRule.v) {
        if (!env.resolveSizeof) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        auto const sz = env.resolveSizeof(expr);
        if (!sz.has_value()) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        HirLiteralValue v;
        v.core  = TypeKind::U64;   // size_t
        v.value = static_cast<std::uint64_t>(*sz);
        return ok(std::move(v));
    }

    // C11/C23 6.5.3.4: `_Alignof(T)` in a const-expr context (an array dimension
    // `int a[_Alignof(T)]`, `_Static_assert(_Alignof(T)==N,...)`). Dispatched by
    // rule-id HERE — ahead of the wrapper-peel — for the SAME reason as sizeof (an
    // alignof node's one meaningful Internal child is its castTypeRef, which the
    // peel would descend into and reject). An ADDITIVE mirror of the sizeof
    // dispatch reading the caller's `resolveAlignof` closure. Absent closure or
    // un-alignable operand ⇒ NotAConstantExpression (fail loud).
    if (cfg.alignofRule.valid() && rule.v == cfg.alignofRule.v) {
        if (!env.resolveAlignof) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        auto const al = env.resolveAlignof(expr);
        if (!al.has_value()) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        HirLiteralValue v;
        v.core  = TypeKind::U64;   // size_t
        v.value = static_cast<std::uint64_t>(*al);
        return ok(std::move(v));
    }

    // c43 (D-CSUBSET-ADDRESS-CONSTANT-FOLD / Option A): a CAST in a const-expr.
    // Dispatched by rule-id ahead of the wrapper-peel (a cast has a type-ref AND an
    // operand). Folds the offsetof spine's casts: `(T*)0` (int 0 → a NULL address),
    // `(char*)x` (ptr→ptr identity, retype pointee), and the terminating
    // `(size_t)(... - (char*)0)` (integer target). The TARGET is classified by the
    // caller's `resolveCastTarget` closure (this engine is interner-free). Absent
    // closure / unresolved target ⇒ non-foldable (fail loud — the prior behaviour).
    if (cfg.castRule.valid() && rule.v == cfg.castRule.v) {
        if (!env.resolveCastTarget) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        auto const tgt = env.resolveCastTarget(expr);
        if (!tgt.has_value()) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        // The cast operand is the LAST visible child (after `( typeRef )`) — a
        // token (`0`) or an internal (`(expr)`).
        NodeId operandN{};
        for (NodeId c : kids) operandN = c;
        if (!operandN.valid()) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        ConstEvalResult inner =
            evalImpl(operandN, ctx, env, options, currentScopeOpaque, visitedInitNodes);
        if (!inner.value.has_value()) return inner;
        if (tgt->isPointer) {
            if (auto const* a = asAddress(*inner.value)) {
                HirAddressValue out = *a;           // ptr→ptr: identity, retype pointee
                out.pointeeType = tgt->pointeeType;
                return ok(makeAddress(out));
            }
            auto const iv = asInt64(*inner.value);
            if (iv.has_value() && *iv == 0) {       // (T*)0 → a NULL-base address
                return ok(makeAddress(HirAddressValue{HirAddressValue::kNullBase, 0,
                                                      tgt->pointeeType}));
            }
            return fail(ConstEvalFailure::NotAConstantExpression, expr);  // (T*)<nonzero>
        }
        if (tgt->isInteger) {
            if (auto const* a = asAddress(*inner.value)) {
                // address → integer: legal ONLY for a NULL-base address (a pure
                // compile-time offset). A symbol-based address is a relocation,
                // not an integer constant — fail loud.
                if (a->base != HirAddressValue::kNullBase) {
                    return fail(ConstEvalFailure::NotAConstantExpression, expr);
                }
                HirLiteralValue v;
                v.core  = tgt->intSigned ? TypeKind::I64 : TypeKind::U64;
                std::int64_t const nv = narrowIntToBits(a->byteOffset, tgt->intBits,
                                                        tgt->intSigned);
                if (tgt->intSigned) v.value = nv;
                else                v.value = static_cast<std::uint64_t>(nv);
                return ok(std::move(v));
            }
            auto const iv = asInt64(*inner.value);
            if (!iv.has_value()) {
                return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
            }
            HirLiteralValue v;
            v.core  = tgt->intSigned ? TypeKind::I64 : TypeKind::U64;
            std::int64_t const nv = narrowIntToBits(*iv, tgt->intBits, tgt->intSigned);
            if (tgt->intSigned) v.value = nv;
            else                v.value = static_cast<std::uint64_t>(nv);
            return ok(std::move(v));
        }
        // A float / aggregate cast target is not part of the address surface.
        return fail(ConstEvalFailure::NotAConstantExpression, expr);
    }

    // c43 (D-CSUBSET-ADDRESS-CONSTANT-FOLD / Option A): a POSTFIX member access in
    // a const-expr — the offsetof spine's `((T*)0)->M` / `.M`. The base folds to an
    // address; the field's byte offset (from the container's layout, via
    // `resolveFieldOffset`) is added — yielding the ADDRESS of the member (an `&`
    // around it is then a no-op, see the AddressOf arm). Index `[i]` and call
    // `(...)` are not part of the array-dim offsetof surface (`&arr[i]` constants
    // are a GLOBAL-init concern → the HIR engine) → fail loud.
    if (cfg.postfixExprRule.valid() && rule.v == cfg.postfixExprRule.v) {
        NodeId baseN{}, opTok{};
        std::vector<NodeId> followers;
        for (NodeId c : kids) {
            if (tree.kind(c) == NodeKind::Token) {
                if (!opTok.valid()) opTok = c;
            } else if (!baseN.valid()) {
                baseN = c;
            } else {
                followers.push_back(c);
            }
        }
        if (!baseN.valid() || !opTok.valid()) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        HirOperatorEntry const* e = opEntryFor(cfg.postfixOps, tree.tokenKind(opTok));
        if (e == nullptr
            || (e->target != "MemberAccess" && e->target != "MemberAccessThruPtr")) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        if (!env.resolveFieldOffset) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        // The field-name token lives in the follower subtree (the first token
        // within it — mirrors cst_to_hir's member-access lowering).
        NodeId const followerN = followers.empty() ? NodeId{} : followers.front();
        NodeId fieldTok{};
        if (followerN.valid()) {
            for (NodeId c : visibleChildren(tree, followerN)) {
                if (tree.kind(c) == NodeKind::Token) { fieldTok = c; break; }
                if (!fieldTok.valid()) fieldTok = c;
            }
        }
        if (!fieldTok.valid()) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        ConstEvalResult base =
            evalImpl(baseN, ctx, env, options, currentScopeOpaque, visitedInitNodes);
        if (!base.value.has_value()) return base;
        auto const* a = asAddress(*base.value);
        if (a == nullptr) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        auto const fr = env.resolveFieldOffset(a->pointeeType, fieldTok);
        if (!fr.has_value()) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        return ok(makeAddress(HirAddressValue{
            a->base, a->byteOffset + static_cast<std::int64_t>(fr->offset),
            fr->fieldType}));
    }

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
        // Plain arithmetic / bitwise / comparison. DEAD-VIA-DRIVER fallback
        // (the driver flattens this case): fold LHS then RHS (left-to-right)
        // then route through the SHARED epilogue.
        ConstEvalResult a = evalImpl(lhsN, ctx, env, options, currentScopeOpaque, visitedInitNodes);
        ConstEvalResult b = evalImpl(rhsN, ctx, env, options, currentScopeOpaque, visitedInitNodes);
        return combineBinaryCst(expr, *e, options, std::move(a), std::move(b));
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
        // c43 (D-CSUBSET-ADDRESS-CONSTANT-FOLD / Option A): `&<lvalue>` of an
        // address-fold spine (`&((T*)0)->M`). The member arm already accumulated
        // the member's byte offset into an address value; the address OF that
        // lvalue IS that address (address-of an lvalue at a known offset = the
        // offset). Pass the address through. A non-address operand (taking the
        // address of a plain value) is not const-foldable.
        if (e->target == "AddressOf") {
            ConstEvalResult inner = evalImpl(operandN, ctx, env, options,
                                             currentScopeOpaque, visitedInitNodes);
            if (!inner.value.has_value()) return inner;
            if (asAddress(*inner.value) != nullptr) return inner;
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        // Deref is not const-foldable.
        if (e->target == "Deref") {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        // DEAD-VIA-DRIVER fallback: fold the operand then route through the
        // SHARED epilogue.
        ConstEvalResult inner = evalImpl(operandN, ctx, env, options, currentScopeOpaque, visitedInitNodes);
        return combineUnaryCst(expr, *e, options, std::move(inner));
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

    // FC17 F2 (D-CSUBSET-CONSTEXPR / the pre-existing `_Static_assert('a'==97)`
    // gap): a NARROW character constant (`'a'`, `'\n'`, `'\xFF'`) in const-expr
    // position — C 6.4.4.4 makes it an integer constant expression. SHAPE-keyed,
    // not rule-keyed (the [opener, body] pair is the tokenizer contract): exactly
    // two visible tokens, the first the NARROW `charStartToken`, the second the
    // `charBodyToken` — a shape the generic wrapper-peel below cannot descend
    // (two tokens, zero internals ⇒ it would fail NotAConstantExpression).
    // Gated on the body token being INTEGER-cored in this consumer's set (C's
    // `'x'` is int-typed via `literalTypes`; a language whose char constants are
    // not integers never folds them here). Decodes via the SHARED
    // `decodeCharLiteralBody` — the EXACT decode the CST→HIR narrow value path
    // (`lowerCharLiteral`) runs, so const-expr and value positions can never
    // disagree; empty / multi-char / malformed-escape bodies fail loud. A
    // WIDE/UTF opener (`L'`/`u'`/`U'`/`u8'`) deliberately does NOT match: its
    // element core is FORMAT-keyed and semantic-stamped (pe wchar_t = u16), and
    // its `\x`-escape surface is a named deferral — folding it here would
    // silently accept what the value tier fails loud on. It falls through to
    // the generic fail below (loud, as today).
    if (cfg.charStartToken.valid() && cfg.charBodyToken.valid()
        && kids.size() == 2
        && tree.kind(kids[0]) == NodeKind::Token
        && tree.kind(kids[1]) == NodeKind::Token
        && tree.tokenKind(kids[0]).v == cfg.charStartToken.v
        && tree.tokenKind(kids[1]).v == cfg.charBodyToken.v
        && ctx.integerLiteralTokens.contains(cfg.charBodyToken.v)) {
        auto const cp = decodeCharLiteralBody(tree.text(kids[1]));
        if (!cp.has_value()) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        HirLiteralValue lv;
        lv.core  = TypeKind::I32;  // C 6.4.4.4: a char constant has type `int`;
                                   // consumers read `.value` (the leaf-arm note)
        lv.value = static_cast<std::int64_t>(*cp);
        return ok(std::move(lv));
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

// ── Plan 24 Stage 6 — the iterative CST const-fold driver ──────────────────
// A POD work-stack frame for ONE flattened straight-line arm. `phase`
// 0 = enter the (last/only) child; Binary phase 1 = enter the second child
// (after stashing the first folded result in `c0`); the final phase pops and
// combines into `result`. PassThrough (wrapper/paren descent) and Unary are
// single-child (phase 0 enters, phase 1 combines/forwards). `entry` points into
// the loaded config (stable for the whole call) — safe to stash. Mirrors the
// Stage-4 hir_to_mir `ValueFrame` idiom + realloc-safe rule (copy frame fields
// to locals and advance `phase` BEFORE any `enter`; copy `result` out before
// `pop_back`).
struct CstFoldFrame {
    enum class Kind : std::uint8_t { PassThrough, Unary, Binary } kind;
    NodeId                  node;
    std::uint8_t            phase;
    HirOperatorEntry const* entry;  // Unary/Binary: resolved operator entry
    ConstEvalResult         c0;     // Binary: the folded LHS (between phase 1 and 2)
};

[[nodiscard]] ConstEvalResult
evalImpl(NodeId                              expr,
         CstEvalContext const&               ctx,
         CstEvalEnvironment const&           env,
         EvalOptions const&                  options,
         std::uint32_t                       currentScopeOpaque,
         std::unordered_set<std::uint32_t> & visitedInitNodes) {
    Tree const&              tree = ctx.tree;
    HirLoweringConfig const& cfg  = ctx.schema.hirLowering();

    std::vector<CstFoldFrame> work;
    // `enter` ALWAYS assigns `result` for a delegated node, and every pushed
    // frame delivers into `result` before it is read (then popped), so this
    // sentinel never leaks.
    ConstEvalResult result = fail(ConstEvalFailure::NotAConstantExpression, expr);

    // Classify `n`: push a frame for a flattenable straight-line arm (plain
    // BinaryOp / unary / transparent wrapper), else fold it here via `evalNode`
    // (delegating; its children re-enter this driver). The plan helpers gate on
    // the EXACT same conditions `evalNode`'s arms would, so a malformed /
    // logical / assign / non-foldable shape delegates and is handled there
    // byte-identically. NOTE: `enter` (push) MUST be the LAST action of any
    // caller path that copied out its frame fields — `work.back()` may dangle.
    auto const enter = [&](NodeId n) {
        if (n.valid() && tree.kind(n) == NodeKind::Internal) {
            auto kids = visibleChildren(tree, n);
            if (auto bp = plainBinaryPlan(tree, cfg, n, kids)) {
                // Operands (lhsN/rhsN) are re-derived from `node` in the loop via
                // the SAME deterministic plan helper (realloc-safe: no dangling
                // child NodeId stored across a `work` realloc).
                work.push_back({.kind = CstFoldFrame::Kind::Binary,
                                .node = n, .phase = 0, .entry = bp->entry});
                return;
            }
            if (auto up = unaryPlan(tree, cfg, n, kids)) {
                work.push_back({.kind = CstFoldFrame::Kind::Unary,
                                .node = n, .phase = 0, .entry = up->entry});
                return;
            }
            if (NodeId const child = wrapperChild(tree, cfg, n, kids); child.valid()) {
                work.push_back({.kind = CstFoldFrame::Kind::PassThrough,
                                .node = child, .phase = 0, .entry = nullptr});
                return;
            }
        }
        // Delegate (token leaf / sizeof / logical short-circuit / ternary /
        // malformed straight-line arm → fail loud, byte-identical to recursive).
        result = evalNode(n, ctx, env, options, currentScopeOpaque, visitedInitNodes);
    };

    enter(expr);
    while (!work.empty()) {
        CstFoldFrame& f = work.back();
        switch (f.kind) {
        case CstFoldFrame::Kind::PassThrough:
            // The wrapper's single child IS `f.node` (stored at push). Enter it
            // and pop in one step — its folded `result` is the wrapper's result.
            {
                NodeId const childN = f.node;
                work.pop_back();
                enter(childN);   // may push a deeper frame or set `result`
            }
            break;
        case CstFoldFrame::Kind::Unary:
            if (f.phase == 0) {
                f.phase = 1;
                // Re-derive the operand from the node (realloc-safe: kids is a
                // fresh local; the plan helper is deterministic).
                auto kids = visibleChildren(tree, f.node);
                NodeId const operandN = unaryPlan(tree, cfg, f.node, kids)->operandN;
                enter(operandN);            // build operand — may invalidate `f`
            } else {
                NodeId const          node2 = f.node;
                HirOperatorEntry const* e   = f.entry;
                ConstEvalResult operand = std::move(result);
                work.pop_back();
                result = combineUnaryCst(node2, *e, options, std::move(operand));
            }
            break;
        case CstFoldFrame::Kind::Binary:
            // LHS first (phase 0→1), then RHS (phase 1→2) — matching the
            // recursive `a = evalImpl(lhsN); b = evalImpl(rhsN);` (two
            // SEQUENTIAL statements → left-to-right, platform-independent).
            if (f.phase == 0) {
                f.phase = 1;
                auto kids = visibleChildren(tree, f.node);
                NodeId const lhsN = plainBinaryPlan(tree, cfg, f.node, kids)->lhsN;
                enter(lhsN);                // build LHS — may invalidate `f`
            } else if (f.phase == 1) {
                f.c0 = std::move(result);   // LHS result
                f.phase = 2;
                auto kids = visibleChildren(tree, f.node);
                NodeId const rhsN = plainBinaryPlan(tree, cfg, f.node, kids)->rhsN;
                enter(rhsN);                // build RHS — may invalidate `f`
            } else {
                NodeId const          node2 = f.node;
                HirOperatorEntry const* e   = f.entry;
                ConstEvalResult lhs = std::move(f.c0);
                ConstEvalResult rhs = std::move(result);
                work.pop_back();
                result = combineBinaryCst(node2, *e, options,
                                          std::move(lhs), std::move(rhs));
            }
            break;
        }
    }
    return result;
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
                   NodeId declRuleNode, NodeId nameNode) {
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
    // FC4 c1: declarator-mode rows — locate the init-declarator whose
    // declarator names `nameNode` (the symbol's declNode anchor) and
    // return ITS init subtree: the visible Internal child that is not
    // the declarator (`[declarator, '=', initValue]`). Multi-declarator
    // lists carry one independent init per name; without the name match
    // the FIRST declarator's init would silently stand in for every
    // symbol in the list.
    if (decl.isDeclaratorMode()) {
        auto const& dcOpt = tree.schema().semantics().declarators;
        if (!dcOpt.has_value() || !nameNode.valid()) return std::nullopt;
        auto const carrier = decl.declaratorListChild.has_value()
                                 ? decl.declaratorListChild
                                 : decl.declaratorChild;
        if (!carrier.has_value() || *carrier >= kids.size()) {
            return std::nullopt;
        }
        std::vector<NodeId> declarators;
        collectDeclarators(tree, kids[*carrier], *dcOpt, declarators);
        for (NodeId d : declarators) {
            if (declaratorNameNode(tree, d, *dcOpt).v != nameNode.v) continue;
            if (tree.rule(d) != dcOpt->initDeclaratorRule) return std::nullopt;
            for (auto const& c : tree.children(d)) {
                if (isEmptySpace(tree.flags(c))) continue;
                if (tree.kind(c) != NodeKind::Internal) continue;
                if (tree.rule(c) == dcOpt->declaratorRule) continue;
                return c;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }
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
