#include "hir/cst_const_eval.hpp"

#include "core/substrate/walk_steps.hpp"
#include "core/types/char_decode.hpp"         // FC17 F2: shared narrow char-literal decode
#include "core/types/decl_prefix_strip.hpp"   // declRoleChildren (shared specifier-prefix strip)
#include "core/types/declarator_walk.hpp"     // FC4: collectDeclarators / declaratorNameNode
#include "core/types/grammar_schema.hpp"
#include "core/types/hir_lowering_config.hpp"
#include "core/types/integer_literal_ladder.hpp"  // C4b: bitPreciseLiteralSignedness
#include "core/types/semantic_config.hpp"
#include "core/types/tree.hpp"
#include "core/types/wide_string_encode.hpp"  // decodeWideCharCodepoint (the value tier's decode)
// [[D-C-FLOAT-CAST-DOES-NOT-FOLD-IN-A-CONSTANT-EXPRESSION]]: for
// `classifyCstCastTarget` ONLY — the boundary function that turns a resolved
// TypeId into a `CstCastTarget`. `evaluateConstantCst` itself still touches no
// interner; see the note on the declaration.
#include "core/types/type_lattice/type_interner.hpp"
#include "hir/const_eval_arith.hpp"
#include "hir/const_eval_operators.hpp"   // shared opFromName / opEntryFor seams
#include "hir/hir_op.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
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
using detail::floatKindInfo;
using detail::isFloatValue;
using detail::makeBoolLiteral;
using detail::narrowToFloatWidth;

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

// ── THE ONE DRIVER, AND WHAT AN ARM ASKS IT FOR ──────────────────────────────────────
// The public entry `evaluateConstantCst` drives an explicit heap work-stack (plan 24 Stage 6 —
// D-PARSE-DEEP-NEST-RECURSION-MEMORY): `evalImpl` below, entered ONCE per evaluation. An arm of
// the per-node body (`evalNode`) that needs a child's value does not fold it: it fills a FRAME —
// which child, in which scope, and what its value becomes — which the driver pushes, folding
// the child as a frame of its own and then running the frame's epilogue. So the nesting of ANY
// arm costs heap, never a host frame.
// ★ P69 L2 (lane `lm`, D-HIR-CST-CONST-EVAL-RECURSES-PER-CONDITIONAL-AND-LOGICAL-LEVEL): the
// conditional and the logical operators, a cast, a compile-time selection, a builtin call's
// operands, a member access's base, `&`, a named constant's initializer and a constant
// subobject's were DELEGATED arms that folded their children by RE-ENTERING the driver — a
// fresh driver and two host frames per level (the row holds the measurement).
//
// A frame's fields, by kind (`phase` counts the children already asked for; the last phase pops
// and delivers into the driver's `result`; `scope` is the scope the node folds in):
//   PassThrough  `node` IS the child to fold (a wrapper's, a value step's, a selection's arm);
//   Unary        `child` the operand, `entry` the operator;
//   Binary       `child` and `second` the operands, `entry` the operator;
//   Conditional  `child` the condition, then the SELECTED arm; `second` the then-arm, then the
//                UNSELECTED one; `third` the else-arm; `c0` the selected arm's value; `target`
//                the tier's type of the conditional (`balanceConditionalArm`, C 6.5.15p5);
//   Logical      `child` and `second` the operands, `isAnd`;
//   Convert      `child` a cast's operand, `target` the cast's type;
//   Ref          `child` a named constant's initializer value, folded in `childScope`;
//                `initKey` its entry in the cycle set; `target` the declared type;
//   Subobject    as Ref, and `bitField` a bit-field member read at its width;
//   Member       `child` the base, `second` the member's name token;
//   AddressOf    `child` the operand;
//   Builtin      `call` the call, its operands folded one by one into `values` and `cores`.
struct CstFoldFrame {
    enum class Kind : std::uint8_t {
        PassThrough, Unary, Binary, Conditional, Logical, Convert, Ref, Subobject, Member,
        AddressOf, Builtin
    } kind;
    NodeId                         node;
    std::uint32_t                  phase = 0;
    HirOperatorEntry const*        entry = nullptr;   // loaded config: stable for the call
    ConstEvalResult                c0{};
    std::uint32_t                  scope = 0;
    NodeId                         child{};
    NodeId                         second{};
    NodeId                         third{};
    std::uint32_t                  childScope = 0;
    std::uint32_t                  initKey = 0;
    bool                           isAnd = false;
    std::optional<CstCastTarget>   target{};
    std::optional<CstBitFieldRead> bitField{};
    std::optional<CstBuiltinCall>  call{};
    std::vector<HirLiteralValue>   values{};
    std::vector<TypeKind>          cores{};
};

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
    // C4b (D-CSUBSET-BITINT-CONSTFOLD-LARGE): a `_BitInt`-involving binary op folds
    // via the shared wrap-aware bignum at the TRUE C23 UAC result width. A BitInt
    // operand is never float / address, so this is checked FIRST.
    if (auto bf = detail::foldBitIntBinary(*opK, *a.value, *b.value, options.charIsUnsigned); bf.applies) {
        if (bf.ok) return ok(std::move(bf.value));
        return fail(bf.failure, expr);
    }
    // D-CSUBSET-INT128-CONSTFOLD-WIDE (TF-C94) — the CRIT-3 belt, CST side. The HIR
    // walker's twin keys on the RESULT TypeId; this engine is interner-free, so it
    // keys on the OPERANDS instead: if either is 128-bit-cored and the bignum
    // above declined, the int64 path below would fold it mod 2^64 — silently. The
    // widened `foldBitIntBinary` entry should make this unreachable; the belt is
    // what converts a FUTURE routing miss into a loud failure rather than a wrong
    // constant that no test would notice.
    if (detail::isInt128Kind(a.value->core) || detail::isInt128Kind(b.value->core)) {
        return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
    }
    bool const eitherFloat = isFloatValue(*a.value) || isFloatValue(*b.value);
    if (eitherFloat) {
        if (!options.allowFloat) {
            return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        }
        // The kernel states the result's C type and rounds the value to it — the core the
        // balance of a conditional reads (`balanceConditionalArm`).
        ConstEvalFailure why = ConstEvalFailure::None;
        if (auto folded = applyBinaryFloat(*opK, *a.value, *b.value, options.charIsUnsigned, why);
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
    // `asIntBits` -- see the twin guard in const_eval.cpp
    // (D-HIR-CONSTEVAL-UNSIGNED-WRAPAROUND-NOT-MODULAR). "Is this an integer
    // operand" is the question here; "does it fit in a signed int64" is not.
    if (!detail::asIntBits(*a.value).has_value()
        || !detail::asIntBits(*b.value).has_value()) {
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
    // ── UNARY `+` IS A CONSTANT OPERATOR ──────────────────────────────────────
    // [[D-CE-CAST-OF-AN-UNSIGNED-CONSTANT-ABOVE-INT64-DOES-NOT-FOLD]] (its named
    // neighbour). It is no `HirOpKind` — the lowering turns `+x` into the
    // promotion itself — so `opFromName` has no answer for it and this walker
    // refused EVERY constant expression that held one. ✔MEASURED 2026-10-10:
    // `_Static_assert(+5U == 5U, "")`, an enumerator `= +1` and an array bound
    // `[+2]` were each "not an integer constant expression" (462 of the 468
    // assertion cells of the `+` group of the unary matrix), where cl, MinGW gcc,
    // gcc, clang and AppleClang accept every one. C 6.5.3.3p2: the result is the
    // value of the promoted operand. The name is the configured operator TARGET,
    // the one the `#if` evaluator and the typing rules read for this operator.
    if (e.target == "Pos") {
        HirLiteralValue const& v = *inner.value;
        // A bit-precise or 128-bit operand does not promote; a float keeps its value.
        if (std::holds_alternative<BitIntValue>(v.value) || detail::isInt128Kind(v.core)) {
            return inner;
        }
        if (isFloatValue(v)) {
            if (!options.allowFloat) return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
            return inner;
        }
        if (auto folded = detail::applyUnaryPlusInt(
                v, detail::intUnaryDomain(v, options.charIsUnsigned));
            folded.has_value()) {
            return ok(std::move(*folded));
        }
        return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
    }
    auto opK = opFromName(e.target);
    if (!opK.has_value()) {
        return fail(ConstEvalFailure::UnsupportedOperator, expr);
    }
    // C4b: a unary op on a `_BitInt` operand (`-5wb`, `~x`) folds via the bignum.
    if (auto uf = detail::foldBitIntUnary(*opK, *inner.value, options.charIsUnsigned); uf.applies) {
        if (uf.ok) return ok(std::move(uf.value));
        return fail(uf.failure, expr);
    }
    // D-CSUBSET-INT128-CONSTFOLD-WIDE (TF-C94): the unary twin of the binary belt above.
    if (detail::isInt128Kind(inner.value->core)) {
        return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
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
    // `asIntBits` — see the twin guard in const_eval.cpp: "is this an integer
    // operand", not "does it fit a signed int64".
    if (!detail::asIntBits(*inner.value).has_value()) {
        return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
    }
    // The promoted type of the operand's own core, through the verb the HIR walker
    // states its domain with.
    if (auto folded = applyUnaryInt(
            *opK, *inner.value,
            detail::intUnaryDomain(*inner.value, options.charIsUnsigned));
        folded.has_value()) {
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

// P31: is this rule one the config declares as folding to a compile-time value
// the SEMANTIC tier computed? A linear scan of a list that is 2 entries long in
// the shipped C document and empty in every other language — the same shape and
// the same cost as the rule-id comparisons around it, and a set would buy
// nothing but a second representation of a two-element list.
[[nodiscard]] bool
isFoldedConstantRule(HirLoweringConfig const& cfg, RuleId rule) {
    for (RuleId r : cfg.foldedConstantRules) {
        if (r.valid() && r.v == rule.v) return true;
    }
    return false;
}

// Resolve a transparent WRAPPER node to its single child to descend into (the
// deep-parens axis `((((expr))))`). A wrapper is what THE statement says it is
// (`nodeSaysSomethingItself`, core/types/decl_prefix_strip.hpp): every node that
// says something itself — an operator, `sizeof`, a cast, a compound literal, a
// selection, a statement expression, a brace list, a folded builtin — is `evalNode`'s
// to fold by its own arm or to refuse, never stepped through. Then: exactly one
// meaningful Internal child → that child; else zero internals and exactly one token
// child → that token; else invalid (→ delegate, which yields the same
// NotAConstantExpression). Shared by the driver + `evalNode`'s tail, so the peel
// decision never diverges.
// ★ P69 (lane `lm`, the review's B): THIS WAS A SECOND STATEMENT — a dispatch list of
// its own (sizeof, alignof, the operators, the cast, choose, `_Generic`, the folded
// builtins, the compound literals), which named neither the statement expression, the
// label address nor the brace list, and read the cast and `_Generic` rules from the
// other key. So a statement expression was peeled to its ONE item by count, whatever
// that item was: `({ { 5; } })` and `({ return 5; })` folded to 5 where C gives the
// construct no value (the value is the last item's only when that item is an
// expression statement), and a mutant taking the statement expression out of the
// statement changed no constant.
[[nodiscard]] NodeId
wrapperChild(Tree const& tree, HirLoweringConfig const& cfg, NodeId expr,
             std::vector<NodeId> const& kids) {
    if (nodeSaysSomethingItself(tree.schema().semantics(), cfg, tree, expr)) return NodeId{};
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

// THE VALUE STEP A DESCENT THAT ASKS A VALUE TAKES BY NAME (P69, lane `lm`): a statement
// expression of ONE item that is an expression statement has that expression's value
// (`soleExpressionOfStatementExpression`, the one place that says so); a body ending in
// anything else has no value, and is no constant.
// ★ A BRACE LIST IS NO STEP OF THIS ONE (P69 L2, the review of checkpoint 29, BLOCKER 2):
// its first element is a SCALAR's value only where a scalar is read — a reader that knows it
// takes that step (`scalarInitializerValue`) — and stepped here, at ANY brace list, it made
// an array named constant read whole its first element. A brace list the evaluator meets on
// its own is no constant (`evalNode`'s tail).
// The node to step to, or an invalid node. ★ THE DRIVER TAKES THE STEP AS A FRAME (the
// review's E): the fold that named this step took it in `evalNode`'s tail, one fresh driver
// and two host frames per level, so `({ ({ … ({ 0; }) … }); })` nested as deep as the parser
// admits recursed on the host stack where the peel by count it replaced had been a frame.
// Shared by the driver and `evalNode`'s tail, so the two never take different steps.
[[nodiscard]] NodeId
valueStepChild(Tree const& tree, HirLoweringConfig const& cfg, NodeId expr) {
    if (!expr.valid() || tree.kind(expr) != NodeKind::Internal) return NodeId{};
    return soleExpressionOfStatementExpression(cfg, tree, expr);
}

// ── A BIT-FIELD MEMBER IS READ AT ITS WIDTH ───────────────────────────────────────────
// (cycle P69 round 1, lane `lm`.)
//
// A bit-field holds its initializer converted to the field: the declared type
// RESTRICTED TO THE WIDTH (C 6.7.2.1p10) — the low `width` bits, sign-extended when the
// declared type is signed. The constant-subobject arm converted to the declared type
// and stopped: ✔MEASURED 2026-10-12 (pe64, both pipelines; clang 18 and AppleClang 21
// fold C's value in every cell), `{ unsigned b : 3; } g = { .b = -1 }` read as
// 4294967295 (7), `= { 9 }` as 9 (1), `int b : 3` given 7 as 7 (-1), `unsigned long
// long b : 40` given -1 as 2^64-1 (2^40-1). The object itself holds the reduced value,
// so the fold and the run-time read DISAGREED — and a condition folded so marks the
// code after its statement as never reached: `if (g.b != 7) return 7; return 42;` over
// the first of them died of an illegal instruction, in seven of the ten forms run.
//
// `atDeclaredType` holds the value already converted to the member's declared type
// (`declared`); the reduced value is handed on AT THAT TYPE. ★ THE READ DOES NOT
// PROMOTE; AN OPERATOR DOES (P69, lane `lm`, the review of the promotion fold). C
// 6.3.1.1p2 promotes a bit-field by its width where an operator reads it, and WHICH
// expression is a bit-field's value is the one statement the typer and the lowering ask
// (`bitFieldValueWidth`, decl_prefix_strip.hpp) — so this engine's operators ask it too,
// through the tier (`asBitFieldOperand` below), and the read stays at the declared type.
// The fold before this one converted every read to the promoted type here, and so
// promoted where the statement does not: ✔MEASURED 2026-10-15 (pe64, both pipelines),
// `if (({ g.b; }) - 6 < 0)` over a constant `unsigned b : 3` holding 5 folded TRUE and
// ran FALSE — the code after it marked never reached, the image dying of an illegal
// instruction — and `_Static_assert(({ g.b; }) - 6 < 0, "")` was accepted, where clang 18
// returns 7 and fails the assertion (a statement expression's value is its last
// expression after lvalue conversion: the declared type).
// A `_Bool` field's value is 0 or 1 at any width; it too is handed on at its type.
// Refused, as no constant: a field of a bit-precise or 128-bit type, a width the
// declared type cannot hold, and a plain `char` field whose sign the caller was not
// told — none of them a shape a reference was measured folding.
[[nodiscard]] ConstEvalResult
readThroughBitField(ConstEvalResult const& atDeclaredType, CstCastTarget const& declared,
                    CstBitFieldRead const& field, NodeId expr) {
    if (!atDeclaredType.value.has_value()) return atDeclaredType;
    if (declared.isBool) return atDeclaredType;
    std::uint32_t const width = field.width;
    if (!declared.isInteger || declared.intSignednessUnknown || declared.intBits > 64
        || width == 0 || width > static_cast<std::uint32_t>(declared.intBits)) {
        return fail(ConstEvalFailure::NotAConstantExpression, expr);
    }
    auto const bits = detail::asIntBits(*atDeclaredType.value);
    if (!bits.has_value()) return fail(ConstEvalFailure::NotAConstantExpression, expr);
    std::uint64_t const mask = width >= 64 ? ~std::uint64_t{0}
                                           : ((std::uint64_t{1} << width) - 1);
    std::uint64_t reduced = static_cast<std::uint64_t>(*bits) & mask;
    if (declared.intSigned && ((reduced >> (width - 1)) & 1u) != 0) reduced |= ~mask;
    HirLiteralValue atField = *atDeclaredType.value;   // the declared type's own core
    if (std::holds_alternative<std::uint64_t>(atField.value)) {
        atField.value = reduced;
    } else {
        atField.value = static_cast<std::int64_t>(reduced);
    }
    return ok(std::move(atField));
}

// ── A FOLDED VALUE CONVERTED TO A CLASSIFIED TARGET ──────────────────────────────
// The cast arm's whole meaning, lifted out VERBATIM (P69, lane `cs`) so a second reader
// applies the one conversion rather than a copy of it: a CONSTANT SUBOBJECT — a compound
// literal constant, or the `.member` of a structure or union constant (C23 6.6p6-p7) — is
// read AT ITS DECLARED TYPE, as `(T)v` converts its initializer's value (an object's
// value is its initializer converted to the object's type, C 6.7.9p11). `inner` holds a
// value; `expr` is the node a failure blames.
[[nodiscard]] ConstEvalResult
convertToCastTarget(ConstEvalResult const& inner, CstCastTarget const& target,
                    EvalOptions const& options, NodeId expr) {
    CstCastTarget const* const tgt = &target;
    // ── `(_Bool)x` IS A COMPARISON, NOT A ONE-BIT TRUNCATION ────────────────
    // [[D-C-FLOAT-CAST-DOES-NOT-FOLD-IN-A-CONSTANT-EXPRESSION]], C 6.3.1.2:
    // "the result is 0 if the value compares equal to 0; otherwise 1". This
    // arm is FIRST because a `_Bool` target used to be carried as a width-1
    // INTEGER and folded through `narrowIntToBits(v, 1, false)`, which keeps
    // the low bit — the two rules disagree on every even value. ✔MEASURED,
    // `int a[(_Bool)2 + 41];` built a 41-element array here and a 42-element
    // one on all four references; the same expression at the HIR tier was
    // already correct, so ONE value had TWO transforms and only one of them
    // was C. The truth question is `detail::asBool`, the same verb the HIR
    // cast arm and `asBoolBridge` ask.
    if (tgt->isBool) {
        // An ADDRESS keeps the capability the width-1 integer path had, and
        // the same boundary the integer arm below draws: a NULL-base address
        // is a pure compile-time offset with a defined truth value, while a
        // SYMBOL-based one is a relocation and is not a constant here.
        // ✔MEASURED, all four references accept `(_Bool)(void *)0 == 0`.
        if (auto const* a = asAddress(*inner.value)) {
            if (a->base != HirAddressValue::kNullBase) {
                return fail(ConstEvalFailure::NotAConstantExpression, expr);
            }
            return ok(makeBoolLiteral(a->byteOffset != 0 ? 1 : 0));
        }
        auto const b = asBool(*inner.value, options.allowFloat);
        if (!b.has_value()) {
            return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        }
        return ok(makeBoolLiteral(*b ? 1 : 0));
    }
    // ── A FLOAT TARGET ──────────────────────────────────────────────────────
    // `(double)3` / `(float)0.1` in a constant expression. The descriptor had
    // no float classification at all before this row, so the resolver returned
    // nullopt and the whole cast was non-constant.
    //
    // ⚠ THIS DOES NOT LET A FLOAT VALUE INTO AN INTEGER-REQUIRED CONSUMER. The
    // wall is `asInt64Bridge`, which answers only for integer arms, so a
    // float-armed RESULT comes back nullopt and the consumer fails loud —
    // ✔MEASURED, gcc 13.3.0, clang 18.1.3, mingw-w64 gcc 13.2.0 and MSVC 19.51
    // all reject `int a[(double)3];`, `enum E { A = (double)3 };` and
    // `struct S { int x : (double)3; };`, while all four accept
    // `_Static_assert((double)3 == 3.0, "")` and `(int)(double)3`. That wall was
    // put at the bridge — rather than at the leaf, which cannot see the result
    // type — by the CLOSED row
    // [[D-C-STATIC-ASSERT-REFUSES-A-LONG-DOUBLE-COMPARISON]].
    if (tgt->floatKind.has_value()) {
        TypeKind const toK = *tgt->floatKind;
        if (!options.allowFloat) {
            return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        }
        // F80/F128 target: carry the value at TRUE target precision in the
        // wide arm, never a binary64-rounded stand-in (LD-3's rule, and the
        // reason `floatKindInfo(F80).hostBacked` is false).
        if (WideFloatValue::isSupportedKind(toK)) {
            // An INTEGER source converts EXACTLY and by its core's signedness —
            // the shared verb `combineCast` calls. `toWideFloatOperand` is the
            // FLOAT operand's bridge: it takes an integer through a host `double`,
            // which rounds it above 2^53 and reads an unsigned 64-bit pattern as
            // negative.
            auto w = isFloatValue(*inner.value)
                ? detail::toWideFloatOperand(*inner.value, toK)
                : detail::integerConstantAsWideFloat(*inner.value, toK,
                                                     options.charIsUnsigned);
            if (!w.has_value()) {
                return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
            }
            HirLiteralValue v;
            v.core  = toK;
            v.value = *w;
            return ok(std::move(v));
        }
        auto const info = floatKindInfo(toK);
        if (!info.has_value() || !info->hostBacked) {
            return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        }
        double src = 0.0;
        if (auto const* wf = std::get_if<WideFloatValue>(&inner.value->value)) {
            src = wf->toDouble();          // F80/F128 → binary64, round-to-nearest
        } else if (isFloatValue(*inner.value)) {
            src = *detail::asDouble(*inner.value);
        } else {
            // Int → Float, through the SHARED verb, which reads the source's
            // signedness off its CORE and rounds ONCE, to the target's own
            // precision. Neither obvious bridge works here: `asIntBits` is an
            // int64 REINTERPRETATION and would fold `(double)18446744073709551615ULL`
            // to -1.0, while `asInt64` refuses the value outright — and all four
            // references fold it to 2^64 (✔MEASURED). Nor a `double` first: that is
            // two roundings (`integerConstantAsFloatKind` says what they cost).
            auto const iv = detail::integerConstantAsFloatKind(*inner.value, toK,
                                                               options.charIsUnsigned);
            if (!iv.has_value()) {
                return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
            }
            src = *iv;
        }
        HirLiteralValue v;
        v.core = toK;
        // ★ NARROW TO THE TARGET'S OWN WIDTH. An F32 target must round through
        // binary32 — the identical call the float LEAF gained when `0.1f` was
        // found carrying 53 significant bits under an F32 core; a cast that
        // merely relabelled the host double would reintroduce that same wrong
        // answer through a second door.
        v.value = narrowToFloatWidth(src, info->bits);
        return ok(std::move(v));
    }
    // C4b (D-CSUBSET-BITINT-CONSTFOLD-LARGE): a `(_BitInt(N))expr` cast folds via
    // the wrap-aware bignum `convertTo(N, signed)` (mod-2^N) — narrow AND wide.
    // Any integer / bit-precise operand converts; a non-integer operand fails loud.
    if (tgt->isBitPrecise) {
        // A FLOAT operand truncates per C 6.3.1.4p1 first (clang 18.1.3 folds
        // `(_BitInt(16))300.5`; gcc 13.3.0 and MSVC 19.51 have no `_BitInt`).
        if (auto const fv = detail::floatToWideIntTarget(*inner.value, tgt->bitWidth,
                                              tgt->bitSigned, options)) {
            if (!fv->value.has_value()) return fail(ConstEvalFailure::Overflow, expr);
            HirLiteralValue v;
            v.core  = TypeKind::BitInt;
            v.value = *fv->value;
            return ok(std::move(v));
        }
        auto bv = detail::asBitIntValue(*inner.value, options.charIsUnsigned);
        if (!bv.has_value()) {
            return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        }
        bv->convertTo(tgt->bitWidth, tgt->bitSigned);
        HirLiteralValue v;
        v.core  = TypeKind::BitInt;
        v.value = std::move(*bv);
        return ok(std::move(v));
    }
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
    // D-CSUBSET-INT128-CONSTFOLD-WIDE (TF-C94): a cast to a 128-bit integer routes
    // through the bignum, exactly like the `_BitInt` arm above — checked
    // BEFORE the generic `isInteger` arm, which would otherwise narrow the
    // value through `narrowIntToBits` into an int64 and tag it I64/U64. That
    // is the silent 64-bit wrap: `(__uint128_t)x` would claim a 128-bit type
    // while carrying 64 bits of value. The result keeps a STANDARD I128/U128
    // core — a `__int128` is not bit-precise, so tagging it `TypeKind::BitInt`
    // here would silently change the expression's type mid-fold.
    //
    // ★ PROVENANCE — this arm shipped DEAD and was revived by a self-audit.
    // As first written it could never fire: `semantic_analyzer.cpp`'s
    // `resolveCastTarget` had no I128/U128 row, so a 128-bit cast target fell to
    // its `default: return nullopt` and the whole cast was non-foldable. The
    // MEASURED symptom was that EVERY 128-bit integer constant expression
    // refused — `_Static_assert((__uint128_t)5 == 5, "")` included — while the
    // `_BitInt(128)` twin was clean. The resolver rows are now present and this
    // is the reachable 128-bit shape; the descriptor's own `intBits` is the
    // width because this engine is interner-free.
    //
    // ★ WHY THE WIDE RESULT CANNOT TRUNCATE DOWNSTREAM (measured, and the reason
    // reviving this arm is safe): the value it produces rides a 128-bit
    // `BitIntValue`, and `asInt64` admits one only when its VALUE is
    // representable in an int64 — never merely because the low limb looks
    // small (D-CE-ASINT64-REJECTS-BY-WIDTH-NOT-MAGNITUDE). So the two 64-bit ICE
    // slots — `asInt64Bridge` (array dimension / static-assert / enumerator) and
    // the `isInteger` narrowing arm just below — REFUSE a value that does not
    // fit instead of narrowing it. `int a[(__uint128_t)1 << 100];` fails loud
    // with S_NonConstantArrayLength, and so does the sharper
    // `int a[((__int128)1 << 100) + 3];`, whose low 64 bits are exactly 3;
    // neither ever becomes a truncated bound.
    //   ⓘ WHAT CHANGED, AND WHAT DID NOT: the rule used to be the declared
    // WIDTH (`width() > 64` ⇒ always nullopt), which ALSO refused
    // `int a[(__int128)2 + 1];` — a value both gcc 13.3.0 (`-std=c2x`) and
    // clang 18.1.3 (`-std=c23`) accept. Those now fold. The refusal of values
    // that genuinely exceed int64 is unchanged, and is what this note is about.
    if (tgt->isInteger && tgt->intBits == 128) {
        // A FLOAT operand truncates per C 6.3.1.4p1 through the SAME shared
        // verb the `_BitInt` arm and the HIR walker use — and this is the only
        // exact route for it: `(__int128)1e30` has no int64 rendering, so the
        // `asBitIntValue` bridge below refused a conversion gcc 13.3.0 and
        // clang 18.1.3 both fold (✔MEASURED; MSVC 19.51 has no `__int128`).
        if (auto const fv = detail::floatToWideIntTarget(*inner.value, 128u,
                                                         tgt->intSigned, options)) {
            if (!fv->value.has_value()) return fail(ConstEvalFailure::Overflow, expr);
            HirLiteralValue v;
            v.core  = tgt->intSigned ? TypeKind::I128 : TypeKind::U128;
            v.value = *fv->value;
            return ok(std::move(v));
        }
        auto bv = detail::asBitIntValue(*inner.value, options.charIsUnsigned);
        if (!bv.has_value()) {
            return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
        }
        bv->convertTo(128u, tgt->intSigned);
        HirLiteralValue v;
        v.core  = tgt->intSigned ? TypeKind::I128 : TypeKind::U128;
        v.value = std::move(*bv);
        return ok(std::move(v));
    }
    if (tgt->isInteger) {
        // ── THE CAST'S CORE IS ITS DECLARED WIDTH, NOT A GENERIC 64 ─────
        // D-HIR-CONSTEVAL-UNSIGNED-WRAPAROUND-NOT-MODULAR. Both arms below
        // used to stamp `I64`/`U64` on EVERY integer cast result whatever
        // `intBits` said, while `narrowIntToBits` correctly reduced the
        // VALUE to the declared width. Value and label disagreed, and once
        // the usual arithmetic conversions started reading the label, the
        // disagreement became a wrong answer: `(unsigned int)0 - 1u` would
        // compute its common type as 64-bit and wrap to
        // 0xffffffffffffffff instead of 0xffffffff.
        //
        // ★ THIS IS ROW D-CSUBSET-INT128-NARROWING-CAST-SITE-INCOMPLETE'S
        // DEFECT CLASS EXACTLY, one tier up: a result typed from a generic
        // KIND instead of from the DECLARED type. It stayed invisible for
        // the same reason -- the widths whose kind and declared type
        // coincide (I64/U64) worked, so every 64-bit probe passed.
        // `intKindFromWidth` is the interner-free spelling of the declared
        // width, and is the same verb `packBitIntResult` already uses.
        auto const castCore = [&] {
            return detail::intKindFromWidth(
                static_cast<std::uint32_t>(tgt->intBits), tgt->intSigned);
        };
        // [[D-CSUBSET-CONST-EVAL-CHAR-SIGNEDNESS]]: a plain-`char` target
        // whose signedness was never supplied is answerable ONLY while the
        // two readings agree — i.e. while the truncated byte's high bit is
        // clear. `(char)300` is 44 either way and folds; `(char)200` is -56
        // or 200 depending on the target and must NOT be guessed. VALUE-
        // scoped, not type-scoped, so a target-less const-expr (the LSP, the
        // FFI header parser) keeps every answer that is not in doubt — the
        // identical rule `narrowCharConstantSignednessMatters` applies to a
        // character constant. Called from BOTH cast arms below; a guard on
        // one arm only is the partial fix that reads as a complete one.
        auto const charSignUnknownAndObservable =
            [&](std::int64_t truncated) {
                return tgt->intSignednessUnknown
                    && (static_cast<std::uint64_t>(truncated) & 0x80u) != 0u;
            };
        // ── FLOAT → INTEGER, C 6.3.1.4p1 ────────────────────────────────────
        // [[D-C-FLOAT-CAST-DOES-NOT-FOLD-IN-A-CONSTANT-EXPRESSION]], and the
        // shape the row is named for. `(int)1.5` reached the `asIntBits`
        // bridge below, which answers only for integer arms, then the
        // `asBitIntValue` fallback, which does the same — so the fold ended in
        // `UnsupportedTypeKind` and every ICE position refused a cast all four
        // references accept (✔MEASURED: array bound, enumerator, bit-field
        // width, `_Alignas`, static assertion, index designator).
        //
        // ⚠ THE TARGET'S OWN WIDTH DECIDES, NOT int64's, and the predicates are
        // the ones `combineCast` calls — the SAME arithmetic, so the CST fold of
        // `(unsigned long long)1.8446744e19` cannot disagree with the HIR fold
        // of the same text.
        //
        // ★ OUT OF RANGE IS A LOUD REFUSAL, DELIBERATELY, AND THIS IS THE ONE
        // PLACE THE FOUR REFERENCES DO NOT AGREE. 6.3.1.4p1 makes the value
        // UNDEFINED when the truncated integral part does not fit, and
        // ✔MEASURED separately: `(int)1e30` folds to INT_MAX on gcc 13.3.0,
        // clang 18.1.3 and mingw-w64 gcc 13.2.0 but to 0 on MSVC 19.51;
        // `(char)300.5` is 127 on the first three and 44 on MSVC;
        // `(unsigned)-3.5` is 0 on the first three and 4294967293 on MSVC. There
        // is no union answer to bake — and none of the four DIAGNOSES the
        // saturation (measured with `-Wall -Wextra`), so a silently wrong array
        // bound is exactly what they ship. DSS refuses instead, which is a
        // diagnostic C 6.6 already contemplates for a constant expression
        // outside its type's range, and which cannot become a wrong program.
        if (auto const fv = detail::floatToWideIntTarget(
                *inner.value, static_cast<std::uint32_t>(tgt->intBits),
                tgt->intSigned, options)) {
            if (!fv->value.has_value()) return fail(ConstEvalFailure::Overflow, expr);
            if (charSignUnknownAndObservable(fv->value->asI64())) {
                return fail(ConstEvalFailure::NotAConstantExpression, expr);
            }
            HirLiteralValue v;
            v.core = castCore();
            // Width ≤ 64 here (the 128-bit target was handled above), so the
            // bignum's own extraction is the exact value: `asI64` for a signed
            // target, `low64` for an unsigned one — the same split
            // `packBitIntResult` makes for a standard-width result.
            if (tgt->intSigned) v.value = fv->value->asI64();
            else                v.value = fv->value->low64();
            return ok(std::move(v));
        }
        if (auto const* a = asAddress(*inner.value)) {
            // address → integer: legal ONLY for a NULL-base address (a pure
            // compile-time offset). A symbol-based address is a relocation,
            // not an integer constant — fail loud.
            if (a->base != HirAddressValue::kNullBase) {
                return fail(ConstEvalFailure::NotAConstantExpression, expr);
            }
            HirLiteralValue v;
            v.core  = castCore();
            std::int64_t const nv = narrowIntToBits(a->byteOffset, tgt->intBits,
                                                    tgt->intSigned);
            if (tgt->intSigned) v.value = nv;
            else                v.value = static_cast<std::uint64_t>(nv);
            return ok(std::move(v));
        }
        // `asIntBits`: a cast READS its operand's bits and re-labels
        // them at the declared width -- `(unsigned long long)-1` and
        // `(long long)0xffffffffffffffffull` are both well-defined C
        // (6.3.1.3p2), so refusing the operand for not fitting a SIGNED
        // int64 refused legal conversions.
        auto iv = detail::asIntBits(*inner.value);
        if (!iv.has_value()) {
            // ── A WIDE OPERAND NARROWED BY AN EXPLICIT CAST IS NOT A REFUSAL ──
            // D-CSUBSET-INT128-ICE-CONTEXT-REFUSED / D-CSUBSET-INT128-CONSTFOLD-WIDE.
            //
            // `asIntBits` nullopts for a `BitIntValue` wider than 64 bits — the
            // right answer to ITS question ("what is this value as an int64?").
            // It is the wrong answer HERE, because the programmer WROTE a
            // narrowing cast, and C 6.3.1.3p2 defines exactly what that yields:
            // the value reduced modulo 2^N for an unsigned target, and the
            // implementation-defined two's-complement answer for a signed one —
            // which is the very rule `narrowIntToBits` below already implements
            // for a 64-bit source.
            //
            // ✔MEASURED at `301e2a63`, DSS vs clang 18.1.3 (`-std=c23`) and gcc
            // 13.3.0 (`-std=c2x`), probed SEPARATELY — both references fold all
            // four, DSS refused all four:
            //     _Static_assert((int)((__uint128_t)5) == 5, "");        S0029
            //     _Static_assert((unsigned long long)((__uint128_t)5) == 5, "");
            //     enum Q { QA = (int)(__int128)7 };                      S0029
            //     int b[(int)(__uint128_t)4];                            S000B
            // ★ THE ARM DIRECTLY ABOVE ALREADY DID THIS. A `(_BitInt(8))` target
            // routes a wide operand through the bignum's own wrap-aware
            // `convertTo`, so `_Static_assert((_BitInt(8))((__uint128_t)5) == 5)`
            // was CLEAN while the `int` twin was refused — one law, two arms, and
            // only one of them knew it. This reuses the SAME verb rather than
            // adding a second narrowing rule.
            //
            // ⚠ THE ROW'S INVARIANT IS UNTOUCHED: "a value exceeding int64 can
            // never SILENTLY truncate into an ICE". Nothing here is silent — this
            // path is reached only through a cast the programmer wrote, whose
            // whole meaning is the narrowing. A BARE wide value used as an array
            // bound or enumerator still goes through `asInt64`, which admits it
            // only when its VALUE fits an int64
            // (D-CE-ASINT64-REJECTS-BY-WIDTH-NOT-MAGNITUDE), so
            // `int a[(__uint128_t)1 << 100];` keeps failing loud rather than
            // becoming a truncated bound.
            auto wide = detail::asBitIntValue(*inner.value, options.charIsUnsigned);
            if (!wide.has_value()) {
                return fail(ConstEvalFailure::UnsupportedTypeKind, expr);
            }
            // Reduce to the low 64 bits FIRST (mod 2^64, exact and total), then
            // let `narrowIntToBits` apply the declared width exactly as it does
            // for every other source. Composing the two is the same modular
            // reduction as converting straight to `intBits`, because `intBits`
            // here is ≤ 64 (the 128-bit target was handled above).
            wide->convertTo(64u, /*isSigned=*/false);
            iv = static_cast<std::int64_t>(wide->low64());
        }
        // [[D-CSUBSET-CONST-EVAL-CHAR-SIGNEDNESS]]: a plain-`char` target
        // whose signedness was never supplied is answerable ONLY while the
        // two readings agree — i.e. while the truncated byte's high bit is
        // clear. `(char)300` is 44 either way and folds; `(char)200` is -56
        // or 200 depending on the target and must NOT be guessed. Scoped to
        // the VALUE rather than to the type so a target-less const-expr keeps
        // every answer that is not actually in doubt.
        if (charSignUnknownAndObservable(*iv)) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        HirLiteralValue v;
        v.core  = castCore();
        std::int64_t const nv = narrowIntToBits(*iv, tgt->intBits, tgt->intSigned);
        if (tgt->intSigned) v.value = nv;
        else                v.value = static_cast<std::uint64_t>(nv);
        return ok(std::move(v));
    }
    // What is left is a target the classifier admitted but no arm above folds.
    // ⚠ THE OLD SENTENCE HERE SAID "a float / aggregate cast target", AND HALF
    // OF IT WENT FALSE ABOVE: since
    // [[D-C-FLOAT-CAST-DOES-NOT-FOLD-IN-A-CONSTANT-EXPRESSION]] a FLOAT target
    // folds in its own arm, and `classifyCstCastTarget` refuses an AGGREGATE
    // before the fold is ever entered — so neither of the two shapes that
    // sentence named still arrives here. It is kept in the past tense because
    // one live consumer's comment still cites this arm by that name:
    // `SemanticAnalyzerC.ComplexConstexprInitializerRefusesToFold` relies on a
    // `_Complex` cast target being non-foldable, and it is — through the
    // classifier's `default`, not through here.
    return fail(ConstEvalFailure::NotAConstantExpression, expr);
}

// ── AN OPERAND THAT IS A BIT-FIELD'S VALUE, AT ITS PROMOTED TYPE ───────────────────────
// (cycle P69 round 1, lane `lm`, the review of the promotion fold.)
//
// C 6.3.1.1p2: an operator reads a bit-field's value at its type promoted BY ITS WIDTH,
// and WHICH operand is one is the ONE statement the typer and the lowering ask
// (`bitFieldValueWidth`) — the tier answers it here (`resolveBitFieldValueType`), so this
// engine converts an operand exactly where those two convert it. A read that reaches an
// operator through a statement expression (`({ g.b; })`, no bit-field's value) keeps the
// member's declared type, as it does at run time. Asked only of an integer operand a
// promotion can change: a plain `int` never changes, and a floating value or an address
// is no bit-field's.
[[nodiscard]] bool
operandMayBePromoted(ConstEvalResult const& v, CstEvalEnvironment const& env) {
    return v.value.has_value() && env.resolveBitFieldValueType
        && v.value->core != TypeKind::I32 && detail::asIntBits(*v.value).has_value();
}

[[nodiscard]] ConstEvalResult
asBitFieldOperand(NodeId operandN, ConstEvalResult v, CstEvalEnvironment const& env,
                  EvalOptions const& options, std::uint32_t scopeOpaque) {
    if (!operandMayBePromoted(v, env) || !operandN.valid()) return v;
    auto const promoted = env.resolveBitFieldValueType(operandN, scopeOpaque);
    if (!promoted.has_value()) return v;
    return convertToCastTarget(v, *promoted, options, operandN);
}

// ── A CONDITIONAL'S ARMS, BALANCED ─────────────────────────────────────────────────────
// (cycle P69 round 1, lane `lm`, the review of the promotion fold.)
//
// C 6.5.15p5: when both arms are arithmetic, the conditional has the type the usual
// arithmetic conversions give the two — `1 ? 5 : 0u` is an `unsigned int`, so
// `(1 ? 5 : 0u) - 6 < 0` is false. This engine handed on the SELECTED arm at its own type:
// ✔MEASURED 2026-10-15/16 (pe64, both pipelines), `if ((1 ? 5 : 0u) - 6 < 0)`,
// `if ((1 ? g.b : 0u) - 6 < 0)` over a constant `unsigned b : 3` holding 5 and
// `if ((1 ? 5 : xu) - 6 < 0)` over an `unsigned xu` folded TRUE and died of an illegal
// instruction (the code after them marked never reached), `_Static_assert((1 ? 5 : 0u) -
// 6 > 0, "")` failed and `_Static_assert((1 ? -1 : 0u) > 0, "")` failed, where clang 18
// and gcc 13 run 2 and accept both — a non-constant unselected arm balanced by its TYPE.
// The conversion the pair needs is the TIER's type of the conditional when the tier has
// one (`resolveExpressionType`: the typer's own answer, the one the run time converts
// to); else the arms' balance computed here by the verb this engine's binary operators
// perform in (`bitIntUac`, C 6.3.1.8) — a floating arm by its format's width — the
// unselected arm's type its folded value's, or, when it does not fold, the tier's type of
// that arm; else the conditional is no constant. ★ WHERE BOTH ARE KNOWN — every constant
// conditional the semantic tier folds, before its second pass too (its hook types the
// literals first), and every one the lowering folds — BOTH ARE COMPUTED, and two different
// answers stop the process (`failOnDivergentConditionalType`): the balance below is the
// statement the evaluator stands on where the tier gives no type (no hook, or an expression
// its typer cannot type), and it is proved against the typer on every conditional the
// suite folds rather than trusted.
struct CstArithmeticType {
    std::optional<detail::BitIntOperandType> integer{};
    std::optional<TypeKind>                  floating{};
};

// The type a folded value's CORE states, for the usual arithmetic conversions; nullopt for
// an address and for a plain `char` whose sign the caller was not told.
[[nodiscard]] std::optional<CstArithmeticType>
arithmeticTypeOfValue(HirLiteralValue const& v, EvalOptions const& options) {
    if (isFloatValue(v)) {
        if (!floatKindInfo(v.core).has_value()) return std::nullopt;
        return CstArithmeticType{std::nullopt, v.core};
    }
    if (auto const t = detail::bitIntOperandType(v, options.charIsUnsigned); t.has_value()) {
        return CstArithmeticType{*t, std::nullopt};
    }
    return std::nullopt;
}

// The type a classified target states; nullopt for a pointer and for a `char` of unknown sign.
[[nodiscard]] std::optional<CstArithmeticType>
arithmeticTypeOfTarget(CstCastTarget const& t) {
    if (t.floatKind.has_value()) {
        if (!floatKindInfo(*t.floatKind).has_value()) return std::nullopt;
        return CstArithmeticType{std::nullopt, *t.floatKind};
    }
    if (t.isBitPrecise) {
        return CstArithmeticType{detail::BitIntOperandType{t.bitWidth, t.bitSigned, true},
                                 std::nullopt};
    }
    if (t.isBool) return CstArithmeticType{detail::BitIntOperandType{1, false, false}, std::nullopt};
    if (t.isInteger && !t.intSignednessUnknown && t.intBits > 0) {
        return CstArithmeticType{
            detail::BitIntOperandType{static_cast<std::uint32_t>(t.intBits), t.intSigned, false},
            std::nullopt};
    }
    return std::nullopt;
}

// The usual arithmetic conversions of the pair, as the conversion to make.
[[nodiscard]] CstCastTarget
usualArithmeticTarget(CstArithmeticType const& a, CstArithmeticType const& b) {
    CstCastTarget t;
    if (a.floating.has_value() || b.floating.has_value()) {
        TypeKind k = a.floating.has_value() ? *a.floating : *b.floating;
        if (a.floating.has_value() && b.floating.has_value()
            && floatKindInfo(*b.floating)->bits > floatKindInfo(*a.floating)->bits) {
            k = *b.floating;
        }
        t.floatKind = k;
        return t;
    }
    detail::BitIntOperandType const r = detail::bitIntUac(*a.integer, *b.integer);
    if (r.isBitPrecise) {
        t.isBitPrecise = true;
        t.bitWidth     = r.width;
        t.bitSigned    = r.isSigned;
    } else {
        t.isInteger = true;
        t.intBits   = static_cast<int>(r.width);
        t.intSigned = r.isSigned;
    }
    return t;
}

[[nodiscard]] bool
sameArithmeticTarget(CstCastTarget const& a, CstCastTarget const& b) {
    if (a.floatKind.has_value() || b.floatKind.has_value()) return a.floatKind == b.floatKind;
    if (a.isBitPrecise || b.isBitPrecise) {
        return a.isBitPrecise == b.isBitPrecise && a.bitWidth == b.bitWidth
            && a.bitSigned == b.bitSigned;
    }
    return a.isInteger == b.isInteger && a.intBits == b.intBits && a.intSigned == b.intSigned;
}

[[nodiscard]] std::string
describeArithmeticTarget(CstCastTarget const& t) {
    if (t.floatKind.has_value()) return std::string{typeKindNameOrEmpty(*t.floatKind)};
    if (t.isBitPrecise) {
        return std::string{t.bitSigned ? "_BitInt(" : "unsigned _BitInt("}
             + std::to_string(t.bitWidth) + ")";
    }
    return std::string{t.intSigned ? "a signed " : "an unsigned "} + std::to_string(t.intBits)
         + "-bit integer";
}

// The typer's type of a conditional and the arms' balance computed here disagree: one of
// two statements of C 6.5.15p5 is wrong, and the value folded would be the wrong one's.
// Never the program's fault, so never a diagnostic: the process stops LOUD, naming the
// node and both types (the internal-consistency stop of this tree's other walks).
[[noreturn]] void
failOnDivergentConditionalType(Tree const& tree, NodeId conditional,
                               CstCastTarget const& tier, CstCastTarget const& balanced) {
    std::string text{tree.text(conditional)};
    if (text.size() > 80) text = text.substr(0, 80) + "...";
    std::fprintf(stderr,
                 "dss fatal: the conditional at node %u (`%s`) has two types — the typer's %s "
                 "and the arms' balance %s — two statements of C 6.5.15p5 disagree\n",
                 static_cast<unsigned>(conditional.v), text.c_str(),
                 describeArithmeticTarget(tier).c_str(),
                 describeArithmeticTarget(balanced).c_str());
    std::abort();
}

// The selected arm `arm` (folded, a bit-field's value already promoted) of the conditional
// `conditional`, converted to the conditional's type. `tierType` is the tier's type of the
// conditional, asked before the unselected arm folds (a pointer conditional's — handed on as it
// is — folds no other arm); `other` is the unselected arm and `otherValue` its fold, a
// bit-field's value promoted. The DRIVER folds that arm, as a frame of its own (P69 L2: this
// function folded it by re-entering the driver, a host frame per nested level).
[[nodiscard]] ConstEvalResult
balanceConditionalArm(NodeId conditional, NodeId other, ConstEvalResult const& otherValue,
                      ConstEvalResult arm, std::optional<CstCastTarget> const& tierType,
                      CstEvalContext const& ctx, CstEvalEnvironment const& env,
                      EvalOptions const& options, std::uint32_t scopeOpaque) {
    bool const armIsAddress = asAddress(*arm.value) != nullptr;
    // nullopt for an address, and for a `char` of unknown sign (then no balance: refused)
    std::optional<CstArithmeticType> const armType = arithmeticTypeOfValue(*arm.value, options);
    // The unselected arm's type: its folded value's, else the tier's type of it.
    std::optional<CstArithmeticType> otherType;
    bool otherIsAddress = false;
    if (otherValue.value.has_value()) {
        otherIsAddress = asAddress(*otherValue.value) != nullptr;
        if (!otherIsAddress) otherType = arithmeticTypeOfValue(*otherValue.value, options);
    } else if (!tierType.has_value() && env.resolveExpressionType) {
        if (auto const t = env.resolveExpressionType(other, scopeOpaque); t.has_value()) {
            otherIsAddress = t->isPointer;
            if (!otherIsAddress) otherType = arithmeticTypeOfTarget(*t);
        }
    }
    std::optional<CstCastTarget> const balanced =
        (armType.has_value() && otherType.has_value())
            ? std::optional<CstCastTarget>{usualArithmeticTarget(*armType, *otherType)}
            : std::nullopt;
    if (tierType.has_value()) {
        if (balanced.has_value() && !sameArithmeticTarget(*tierType, *balanced)) {
            failOnDivergentConditionalType(ctx.tree, conditional, *tierType, *balanced);
        }
        return convertToCastTarget(arm, *tierType, options, conditional);
    }
    if (armIsAddress || otherIsAddress) return arm;   // a pointer conditional
    if (!balanced.has_value()) return fail(ConstEvalFailure::NotAConstantExpression, conditional);
    return convertToCastTarget(arm, *balanced, options, conditional);
}

// What `evalNode` returns when it fills a frame instead of folding: the driver reads the frame
// (`request.node` valid) and never this value.
[[nodiscard]] ConstEvalResult suspended(NodeId expr) {
    return fail(ConstEvalFailure::NotAConstantExpression, expr);
}

// The core a builtin call's operand or result is folded at, by the type the call converts it to.
[[nodiscard]] TypeKind builtinOperandCore(CstCastTarget const& t) {
    if (t.isBool) return TypeKind::Bool;
    if (t.isInteger && t.intBits >= 8)
        return detail::intKindFromWidth(static_cast<std::uint32_t>(t.intBits), t.intSigned);
    if (t.isPointer) return TypeKind::Ptr;
    if (t.floatKind.has_value()) return *t.floatKind;
    return TypeKind::Void;
}

// The epilogue of a member access in an address fold (`((T*)0)->M`): the base, folded, is an
// address; the field's byte offset (`resolveFieldOffset`) is added — the ADDRESS of the member.
[[nodiscard]] ConstEvalResult
memberAddressCst(NodeId expr, NodeId fieldTok, ConstEvalResult base,
                 CstEvalEnvironment const& env) {
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
// transparent WRAPPER/paren descent) are taken by the driver's own plans first; reached here
// anyway they ask for the same frames. EVERY arm with a child to fold asks the driver for it
// through `request` (P69 L2: the frame kinds above) and returns `suspended`; an arm with no
// child to fold (a leaf, sizeof, a folded constant, a builtin's compile-time answer) returns
// its value and leaves `request.node` invalid. Output-identity holds because each arm's
// evaluation order, cycle-set bracket, short-circuit and epilogue are the frame's, verbatim.
[[nodiscard]] ConstEvalResult
evalNode(NodeId                              expr,
         CstEvalContext const&               ctx,
         CstEvalEnvironment const&           env,
         EvalOptions const&                  options,
         std::uint32_t                       currentScopeOpaque,
         std::unordered_set<std::uint32_t> & visitedInitNodes,
         CstFoldFrame&                       request) {
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
            // C23 6.4.4.1 (D-CSUBSET-BITINT-WIDE-LITERAL / Fork-2c): a `wb`/`uwb`
            // bit-precise literal folds to a `BitIntValue` leaf of the magnitude-
            // derived width (arbitrary magnitude via `decodeBigInteger`), so a
            // `_Static_assert(15wb + 1 == 16)` / `...688uwb` const-expr sees the
            // true bit-precise value. Gated on the consumer supplying the
            // `integerLiteralTyping` rules (the opt-in, mirroring floatLiteralTokens).
            if (!ctx.integerLiteralTyping.empty()) {
                if (auto const bpSigned = bitPreciseLiteralSignedness(
                        tree.text(expr), ctx.numberStyle, ctx.integerLiteralTyping)) {
                    auto mag = decodeBigInteger(tree.text(expr), ctx.numberStyle);
                    if (!mag.has_value()) {
                        return fail(ConstEvalFailure::NotAConstantExpression, expr);
                    }
                    BitIntValue bv = BitIntValue::fromLiteralMagnitude(*mag, *bpSigned);
                    if (bv.width() > kBitIntMaxWidth) {
                        // The typing tier already emitted S_BitIntWidthExceedsMax;
                        // the fold declines (a pure function emits no diagnostic).
                        return fail(ConstEvalFailure::NotAConstantExpression, expr);
                    }
                    HirLiteralValue lv;
                    lv.core  = TypeKind::BitInt;
                    lv.value = std::move(bv);
                    return ok(std::move(lv));
                }
            }
            auto iv = decodeInteger(tree.text(expr), ctx.numberStyle);
            if (!iv.has_value()) {
                // Decode failure (overflow / malformed text) — not a
                // compile-time integer; surfaces NotAConstantExpression
                // so callers can route to their language-specific
                // "out-of-range / malformed-literal" diagnostic.
                return fail(ConstEvalFailure::NotAConstantExpression, expr);
            }
            HirLiteralValue lv;
            // C4b (Fork-2c): stamp the TRUE data-model core (int / long / long long)
            // via the SAME `typeIntegerLiteral` ladder the typed side runs — so a
            // mixed `_BitInt op <long literal>` const-expr computes the C23 UAC width
            // correctly (a `long` literal must NOT be treated as `int`). The int64
            // arm PRESERVES the bit pattern, so `asBitIntValue` recovers the value at
            // the true (width, signed); absent rules ⇒ the I32 default (inert for
            // every non-BitInt fold — the leaf's other consumers read `.value`).
            lv.core = TypeKind::I32;
            std::uint64_t bits = *iv;
            if (!ctx.integerLiteralTyping.empty()) {
                auto const r = typeIntegerLiteral(tree.text(expr), ctx.numberStyle,
                                                  ctx.integerLiteralTyping,
                                                  ctx.dataModel, *iv);
                if (r.status == IntegerLadderStatus::Typed) {
                    lv.core = r.kind;
                    // D-C-MSVC-SIZED-INTEGER-SUFFIXES-REFUSED: the VALUE is the
                    // magnitude reduced to the type — the identity for a ladder
                    // type, MSVC's `wrap` for a fixed one, so
                    // `_Static_assert(300i8 == 44, "")` holds (✔MEASURED, MSVC
                    // 19.51). The SAME shared reduction the CST→HIR literal and
                    // phase 4 perform; a `char`-typed literal above 0x7F with no
                    // target answer is not a constant this fold can decide.
                    auto const reduced = reducedIntegerLiteralBits(
                        r.kind, *iv, options.charIsUnsigned, r.outOfRange);
                    if (!reduced.has_value()) {
                        return fail(ConstEvalFailure::NotAConstantExpression, expr);
                    }
                    bits = *reduced;
                }
            }
            lv.value = static_cast<std::int64_t>(bits);
            return ok(std::move(lv));
        }
        // FC17 (D-CSUBSET-CONSTEXPR): a FLOAT literal leaf — ONLY when the
        // caller opted in by populating `floatLiteralTokens`. Decodes via the
        // SAME config-aware `decodeFloat` the CST→HIR literal lowering uses
        // (suffix strip per numberStyle; a malformed/undecodable text fails
        // loud, never a silent zero).
        //
        // ⚠ THE OLD NOTE HERE SAID THE INTEGER-REQUIRED CONSUMERS LEAVE THIS SET
        // NULL SO THE ARM IS "STRUCTURALLY UNREACHABLE" FOR THEM. That stopped
        // being true with [[D-C-STATIC-ASSERT-REFUSES-A-LONG-DOUBLE-COMPARISON]]:
        // `constIntExpr` now opts in too, because a float SUB-expression whose
        // RESULT is an integer (`0.1L > 0.0L`, `(int)1.5`) is accepted by all four
        // reference toolchains in every ICE position — array dimension, enumerator,
        // bit-field width, static assertion (✔MEASURED separately on gcc 13.3.0,
        // clang 18.1.3, mingw-w64 gcc 13.2.0 and MSVC 19.51). ★ THE WALL MOVED
        // RATHER THAN VANISHED, and it moved to the place that states the actual
        // rule: `asInt64Bridge` refuses a float-armed RESULT, so `int a[1.5]` and
        // `_Static_assert(1.5+1.5, "")` still fail loud while `int a[0.1 > 0.0]`
        // compiles. A wall at the LEAF could only ever say "no floats anywhere in
        // here", which is not what any of the four references implement.
        if (ctx.floatLiteralTokens != nullptr
            && ctx.floatLiteralTokens->contains(tk.v)) {
            HirLiteralValue lv;
            lv.core  = TypeKind::F64;  // the fold-arithmetic core; consumers
                                       // read `.value` (see the I32 note above)
            // FC17.9(e) (D-CSUBSET-LONG-DOUBLE): stamp the TRUE suffix-typed
            // core via the SAME `typeFloatLiteral` ladder the typed side runs
            // (the integer Fork-2c discipline above). LOAD-BEARING: a `20.0L`
            // on a walled axis must carry F80/F128 so the hostBacked fold gate
            // in applyBinary/UnaryFloat refuses it — the flat F64 stamp would
            // silently fold at binary64. An axis-undeclared long-double
            // literal is simply not a foldable constant here (the semantic
            // literal typing already emitted S_LongDoubleFormatUndeclared).
            //
            // ★ THE LADDER RUNS BEFORE THE DECODE, and that ORDER is the fix for
            // D-CSUBSET-LONG-DOUBLE-LITERAL-DECODE-PRECISION: the core it
            // resolves IS the target's mantissa width, so the decoder can be
            // asked for the value AT that width instead of being handed a host
            // `double` after the fact. (It used to decode first and stamp the
            // core second, which is why a `0.1L` leaf could only ever carry 53
            // significant bits no matter what the axis said.)
            if (!ctx.floatLiteralTyping.empty()) {
                auto const r = typeFloatLiteral(tree.text(expr), ctx.numberStyle,
                                                ctx.floatLiteralTyping,
                                                ctx.dataModel, ctx.longDoubleFormat);
                if (r.status == FloatLadderStatus::AxisUndeclared) {
                    return fail(ConstEvalFailure::NotAConstantExpression, expr);
                }
                if (r.status == FloatLadderStatus::Typed) lv.core = r.kind;
            }
            // ★ ONE DISPATCH, THREE READERS. The wide/narrow split used to be
            // written out here AND in `cst_to_hir.cpp`'s literal leaf; it is now
            // the shared `decodeFloatLiteralAtKind` (number_decode.hpp), which
            // the semantic tier's range warning
            // (D-C-FLOAT-LITERAL-OVERFLOW-REFUSED-INSTEAD-OF-YIELDING-INFINITY)
            // is the third caller of. The two properties that split carried are
            // unchanged: F80/F128 decode at TARGET precision into the wide arm
            // (LD-3 — the `double` arm stays valid for those kinds, so this is a
            // value-precision change and not a representation the downstream has
            // to learn), and every other kind is NARROWED to the core's own
            // width. The narrowing changes ANSWERS, not just digits: ✔MEASURED
            // gcc 13.3.0 / clang 18.1.3 / mingw-w64 gcc 13.2.0 / MSVC 19.51 all
            // accept `_Static_assert(0.1f != 0.1, "")`, while an un-narrowed leaf
            // folds that comparison FALSE and would refuse the program.
            auto const fd = decodeFloatLiteralAtKind(tree.text(expr),
                                                     ctx.numberStyle, lv.core);
            if (!fd.ok) {
                return fail(ConstEvalFailure::NotAConstantExpression, expr);
            }
            if (fd.wide.has_value()) lv.value = *fd.wide;
            else                     lv.value = fd.narrow;
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
            // ★ AN AGGREGATE READ WHOLE IS NO SCALAR CONSTANT, and a SCALAR's initializer is
            // read through its braces (`scalarInitializerValue`; P69 L2, the review of
            // checkpoint 29, BLOCKER 2 and MINOR 5). An array named constant read as a value
            // is its address (C 6.3.2.1p3) — no integer constant — and a structure or a union
            // has no value an operator takes; the declared type the resolver classified says
            // which (`namedConstantRead`: nullopt for the three).
            if (!resolved->declaredType.has_value()) {
                return fail(ConstEvalFailure::NotAConstantExpression, expr);
            }
            CstScalarInitializer const scalar =
                scalarInitializerValue(tree, ctx.schema, resolved->initExpr);
            if (scalar.empty) {
                HirLiteralValue zero;
                zero.core  = TypeKind::I32;
                zero.value = std::int64_t{0};
                return convertToCastTarget(ok(std::move(zero)), *resolved->declaredType,
                                           options, expr);
            }
            if (!scalar.value.valid()) {
                return fail(ConstEvalFailure::NotAConstantExpression, expr);
            }
            visitedInitNodes.insert(resolved->initExpr.v);
            // The initializer folds in the SYMBOL's scope (resolved->initScopeOpaque), not
            // the original use-site scope — closes D7 cross-scope shadowing — as a frame of
            // the driver (`Kind::Ref`: it erases the cycle-set entry, blames this name for a
            // failure, and converts the value to the declared type).
            // ★ P69 (lane `lm`, [[D-CSUBSET-CONST-REF-DECLARED-NARROWING]]): A NAMED
            // CONSTANT'S VALUE IS ITS INITIALIZER CONVERTED TO ITS DECLARED TYPE (C
            // 6.7.9p11) — by the ONE conversion the cast uses, as a constant's member
            // is read below. This arm converted for a `_BitInt` declared type alone
            // (C4b: `const _BitInt(4) k = 20wb; k` is 4, not the initializer's 20) and
            // handed every other symbol on at its initializer's type: `const unsigned
            // char c = 300; c` folded to 300, `const unsigned u = 5; -u` to -5 (see
            // `CstResolvedSymbol::declaredType` for the measurement). A wrong value
            // here was a wrong static assertion, and — since a condition this folds
            // can mark what follows a statement as never reached — deleted code.
            request = CstFoldFrame{.kind = CstFoldFrame::Kind::Ref, .node = expr,
                                   .child = scalar.value,
                                   .childScope = resolved->initScopeOpaque,
                                   .initKey = resolved->initExpr.v,
                                   .target = resolved->declaredType};
            return suspended(expr);
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

    // P31 ([[D-FFI-OFFSETOF-MACRO]] + `__builtin_types_compatible_p`): the
    // compile-time-answer operators in a const-expr context — the canonical use,
    // since `_Static_assert(offsetof(struct S, b) == 4, "")` is what a real
    // header writes. Dispatched by rule-id ahead of the wrapper-peel for the SAME
    // reason sizeof is: their one meaningful Internal child is a castTypeRef the
    // peel would descend into and reject. The VALUE comes from the caller's
    // `resolveFoldedConstant` closure, which owns the type resolver and the
    // target's layout params this engine must not depend on. Absent closure or a
    // failed fold ⇒ NotAConstantExpression (fail loud) — never a zero.
    if (isFoldedConstantRule(cfg, rule)) {
        if (!env.resolveFoldedConstant) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        auto const v = env.resolveFoldedConstant(expr);
        if (!v.has_value()) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        // ★ THE VALUE HAS ITS RULE'S TYPE, AS THE TYPER TYPES THE NODE (P69 L2, lane
        // `lm`, the review of checkpoint 29, BLOCKER 1 and MAJOR 3): `__builtin_types_
        // compatible_p` is an `int` (c.lang.json `typesCompatible`), `__builtin_offsetof` a
        // `size_t`. Every such value was a 64-bit unsigned here, so `__builtin_types_
        // compatible_p(int, long) - 1` was 2^64 - 1 and `1 ? __builtin_types_compatible_p(int,
        // int) : 0` balanced to an unsigned 64-bit integer where the typer says `int` — which
        // stopped the compiler. ✔MEASURED 2026-10-20 on checkpoint 29's bytes (pe64 and ELF,
        // both pipelines): `if (__builtin_types_compatible_p(int, long) - 1 >= 0) return 1;
        // return 2;` died of an illegal instruction (the fold marked `return 2` never
        // reached) and `_Static_assert(... - 1 < 0, "")` failed, where clang 18.1.3 and gcc
        // 13.3.0 run 2 and hold it. The resolver hands the bits; the TIER's type of the node
        // states what they are. A tier that gives the node no type has no constant here —
        // never a guessed width.
        std::optional<CstCastTarget> const type =
            env.resolveExpressionType ? env.resolveExpressionType(expr, currentScopeOpaque)
                                      : std::nullopt;
        if (!type.has_value() || !type->isInteger || type->intSignednessUnknown) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        HirLiteralValue lit;
        lit.core  = TypeKind::U64;
        lit.value = *v;
        return convertToCastTarget(ok(std::move(lit)), *type, options, expr);
    }

    // P31 / P49: a COMPILE-TIME SELECTION in a const-expr context —
    // `__builtin_choose_expr` (winner picked by an integer constant condition)
    // and C11/C23 6.5.1.1 `_Generic` (winner picked by the controlling
    // expression's TYPE). ONE arm for both, because what this engine does with a
    // selection does not depend on how the winner was chosen — the same reason
    // the CST→HIR tier lowers both through one recorded-selection frame.
    //
    // The closure hands back the SELECTED arm and this engine then evaluates ONLY
    // that node, so the discarded arms are never visited at all. For
    // `__builtin_choose_expr` that is why `__builtin_choose_expr(1, 4, 1/0)`
    // folds to 4 instead of tripping the divide-by-zero wall, matching gcc; for
    // `_Generic` it is 6.5.1.1p3 itself — the unselected associations are
    // unevaluated, so `_Generic(x, int: 4, default: f())` folds to 4 while
    // `_Generic(x, int: f(), default: 4)` stays non-constant BECAUSE the ordinary
    // fold of the returned arm fails, not because of any second opinion here.
    // ⚠ THE FOLD OF THE ARM IS THE ONLY CONSTNESS TEST. A resolver that answered
    // "this selection is constant" instead of "this is the arm" would be a second
    // owner of that question, and the over-broad answer (a selection is always an
    // ICE) is exactly the shape that passes the easy cases and breaks the
    // standard's boundary.
    bool const isCompileTimeSelection =
        (cfg.chooseExprRule.valid() && rule.v == cfg.chooseExprRule.v)
        || (cfg.genericRule.valid() && rule.v == cfg.genericRule.v);
    if (isCompileTimeSelection) {
        if (!env.resolveSelectedArm) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        auto const arm = env.resolveSelectedArm(expr);
        if (!arm.has_value() || !arm->valid()) {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        request = CstFoldFrame{.kind = CstFoldFrame::Kind::PassThrough, .node = *arm};
        return suspended(expr);
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
        request = CstFoldFrame{.kind = CstFoldFrame::Kind::Convert, .node = expr,
                               .child = operandN, .target = *tgt};
        return suspended(expr);
    }

    // ★ P69 (lane `cs`, C23 6.6p6-p7): a CONSTANT SUBOBJECT — a compound literal constant
    // `(constexpr T){ v }`, or the `.` member of a structure or union named constant /
    // compound literal constant, even recursively. The resolver hands back the initializer
    // VALUE that initializes it (or says it is zero-initialized) and its declared type; this
    // arm folds that value in the value's own scope — the `resolveSymbolInit` path, cycle
    // guard included — and reads it AT the declared type through the cast arm's own
    // conversion. ✔MEASURED 2026-09-30 (probe-reference-cc runs 20260930-185149-8dd835aa,
    // -185219-ff240d50, -215342-bd6f89d0, -215414-bc6d1ef1, -215506-23709b5b): gcc 13.3.0
    // -std=c2x folds every such constant in an array bound, a `case` label, an enumerator and
    // a static assertion, a zero-initialized member and an unsigned member at its own type
    // included, and refuses an INACTIVE union member and an array member's element; clang
    // 18.1.3 -std=c2x folds a CONST (not constexpr) compound literal and a const structure's
    // member in all four as a GNU extension. A member access the resolver does not claim
    // falls through to the offsetof arm below; a compound literal it does not claim is not a
    // constant.
    bool const isCompoundLiteral = [&] {
        for (auto const& row : ctx.schema.semantics().compoundLiteralRules)
            if (row.rule.valid() && row.rule.v == rule.v) return true;
        return false;
    }();
    if (isCompoundLiteral
        || (cfg.postfixExprRule.valid() && rule.v == cfg.postfixExprRule.v)) {
        std::optional<CstConstantSubobject> const sub =
            env.resolveConstantSubobject
                ? env.resolveConstantSubobject(expr, currentScopeOpaque)
                : std::nullopt;
        if (sub.has_value()) {
            // A BIT-FIELD member is read at its width, at its declared type
            // (`readThroughBitField`) — both returns below, so the one reader stays the
            // one when it gains a step.
            auto const asRead = [&](ConstEvalResult const& atType) -> ConstEvalResult {
                if (!sub->bitField.has_value() || !atType.value.has_value()) return atType;
                return readThroughBitField(atType, sub->type, *sub->bitField, expr);
            };
            if (sub->zeroValue) {
                HirLiteralValue zero;
                zero.core  = TypeKind::I32;
                zero.value = std::int64_t{0};
                return asRead(convertToCastTarget(ok(std::move(zero)), sub->type, options, expr));
            }
            NodeId const init = sub->init.initExpr;
            if (!init.valid() || visitedInitNodes.contains(init.v)) {
                return fail(ConstEvalFailure::NotAConstantExpression, expr);
            }
            // A subobject read here is a SCALAR (its `type` a conversion): its initializer is
            // read through its braces, an empty level its zero (`scalarInitializerValue`).
            CstScalarInitializer const scalar = scalarInitializerValue(tree, ctx.schema, init);
            if (scalar.empty) {
                HirLiteralValue zero;
                zero.core  = TypeKind::I32;
                zero.value = std::int64_t{0};
                return asRead(convertToCastTarget(ok(std::move(zero)), sub->type, options, expr));
            }
            if (!scalar.value.valid()) {
                return fail(ConstEvalFailure::NotAConstantExpression, expr);
            }
            visitedInitNodes.insert(init.v);
            request = CstFoldFrame{.kind = CstFoldFrame::Kind::Subobject, .node = expr,
                                   .child = scalar.value,
                                   .childScope = sub->init.initScopeOpaque,
                                   .initKey = init.v, .target = sub->type,
                                   .bitField = sub->bitField};
            return suspended(expr);
        }
        if (isCompoundLiteral) return fail(ConstEvalFailure::NotAConstantExpression, expr);
    }

    // ★ P69 (lane `cs`, D-CSUBSET-GNUC-PREDEFINE-SELECTS-UNIMPLEMENTED-BUILTIN): a CALL of a
    // builtin whose verb has a constant form. ✔MEASURED 2026-10-01 (lane `cs`'s probes r5s
    // s01-s04 + s08, t06-t07): gcc 13.3.0 and clang 18.1.3, both modes, take
    // `__builtin_expect(1, 1)`, `__builtin_ffs(8)`, `__builtin_parity(7)` and
    // `__builtin_popcountl(7)` as integer constant expressions in `_Static_assert`, an
    // enumerator, an array bound and a `case` label, and clang `__builtin_object_size(g, 0)`
    // as well (gcc refuses that one; the union accepts it). The resolver names the callee
    // and its parameters; every operand folds and converts as the call converts it, and
    // `foldBuiltinVerb` — the HIR evaluator's arithmetic too — computes the value. Not a
    // builtin call (an ordinary call): the resolver answers nullopt and the call falls to
    // the member-access arm below, which refuses it as before.
    if (cfg.postfixExprRule.valid() && rule.v == cfg.postfixExprRule.v
        && env.resolveBuiltinCall) {
        if (std::optional<CstBuiltinCall> const bc = env.resolveBuiltinCall(expr)) {
            TypeKind const resultCore = builtinOperandCore(bc->resultType);
            if (bc->answer.has_value()) {
                // A compile-time answer the tier computed (its operands are unevaluated):
                // an integer, or a `_Bool` (the `_p` overflow predicates' result) — held in
                // the int64 arm as 0 / 1, the HirLiteralValue contract for a Bool core.
                if (!bc->resultType.isInteger && !bc->resultType.isBool) {
                    return fail(ConstEvalFailure::NotAConstantExpression, expr);
                }
                HirLiteralValue lit;
                lit.core = resultCore;
                if (bc->resultType.isBool)
                    lit.value = static_cast<std::int64_t>(*bc->answer != 0 ? 1 : 0);
                else if (bc->resultType.intSigned)
                    lit.value = static_cast<std::int64_t>(*bc->answer);
                else
                    lit.value = *bc->answer;
                return convertToCastTarget(ok(std::move(lit)), bc->resultType, options, expr);
            }
            // Its operands fold one by one, as frames of the driver (`Kind::Builtin`), each
            // converted as the call converts it; `foldBuiltinVerb` computes the value.
            request = CstFoldFrame{.kind = CstFoldFrame::Kind::Builtin, .node = expr, .call = *bc};
            return suspended(expr);
        }
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
        request = CstFoldFrame{.kind = CstFoldFrame::Kind::Member, .node = expr,
                               .child = baseN, .second = fieldTok};
        return suspended(expr);
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
        // LogicalAnd / LogicalOr: short-circuit — the right operand is not folded until the
        // left one's truth is known (a frame of the driver, `Kind::Logical`). Matches the HIR
        // walker's C99 semantics exactly.
        if (e->target == "LogicalAnd" || e->target == "LogicalOr") {
            request = CstFoldFrame{.kind = CstFoldFrame::Kind::Logical, .node = expr,
                                   .child = lhsN, .second = rhsN,
                                   .isAnd = (e->target == "LogicalAnd")};
            return suspended(expr);
        }
        // Plain arithmetic / bitwise / comparison — DEAD-VIA-DRIVER (the driver's own plan
        // takes the case first): the same frame.
        request = CstFoldFrame{.kind = CstFoldFrame::Kind::Binary, .node = expr, .entry = e,
                               .child = lhsN, .second = rhsN};
        return suspended(expr);
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
            request = CstFoldFrame{.kind = CstFoldFrame::Kind::AddressOf, .node = expr,
                                   .child = operandN};
            return suspended(expr);
        }
        // Deref is not const-foldable.
        if (e->target == "Deref") {
            return fail(ConstEvalFailure::NotAConstantExpression, expr);
        }
        // DEAD-VIA-DRIVER (the driver's own plan takes the case first): the same frame.
        request = CstFoldFrame{.kind = CstFoldFrame::Kind::Unary, .node = expr, .entry = e,
                               .child = operandN};
        return suspended(expr);
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
        // The condition, then the selected arm (a bit-field's value promoted), then — unless the
        // conditional is a pointer's — the unselected one, each a frame of the driver
        // (`Kind::Conditional`); the selected arm at the conditional's type
        // (`balanceConditionalArm`, C 6.5.15p5).
        request = CstFoldFrame{.kind = CstFoldFrame::Kind::Conditional, .node = expr,
                               .child = condN, .second = thenN, .third = elseN};
        return suspended(expr);
    }

    // FC17 F2 (D-CSUBSET-CONSTEXPR / the pre-existing `_Static_assert('a'==97)`
    // gap): a NARROW character constant (`'a'`, `'\n'`, `'\xFF'`) in const-expr
    // position — C 6.4.4.4 makes it an integer constant expression. SHAPE-keyed,
    // not rule-keyed: an all-token node carrying the NARROW `charStartToken`
    // followed by the `charBodyToken` — a shape the generic wrapper-peel below
    // cannot descend (all tokens, zero internals ⇒ it would fail
    // NotAConstantExpression).
    //
    // ★ KIND-KEYED, NOT COUNT-KEYED (D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN): this
    // used to require `kids.size() == 2` and index `kids[0]`/`kids[1]` directly.
    // The closing `'` is a token of its own now, so the node has THREE children
    // and the count test silently stopped matching — dropping every char constant
    // to the wrapper-peel, which fails NotAConstantExpression and regresses
    // `_Static_assert('a' == 97, "ok")`. Re-pinning the count at 3 would just
    // re-arm the same trap for the next tokenizer shape change, so the scan looks
    // for the OPENER and BODY kinds and is indifferent to what else the literal
    // carries.
    //
    // Gated on the body token being INTEGER-cored in this consumer's set (C's
    // `'x'` is int-typed via `literalTypes`; a language whose char constants are
    // not integers never folds them here). Decodes via the SHARED
    // `decodeCharLiteralBody` — the EXACT decode the CST→HIR narrow value path
    // (`lowerCharLiteral`) runs, so const-expr and value positions can never
    // disagree; empty / multi-char / malformed-escape bodies fail loud.
    //
    // ★ A WIDE/UTF OPENER (`L'`/`u'`/`U'`/`u8'`) FOLDS TOO, through the pair's
    // element core (P68 round 9,
    // D-C-PREFIXED-CHARACTER-CONSTANT-IS-NOT-A-CONSTANT-EXPRESSION). C 6.4.4.4 +
    // 6.6 make every character constant an integer constant expression, and
    // ✔MEASURED at 4d9a24c4 on pe64 DSS refused `_Static_assert(L'a' == 97, "")`,
    // `u'a'`/`U'a'` there, a file-scope `int a[L'a' == 97 ? 1 : -1]` and `enum { E =
    // L'a' }` while mingw-w64 13.2.0 compiled and ran them all. This arm used to
    // refuse them on purpose because the element core is the pair's (`wchar_t` is
    // `unsigned short` on Windows, `int` on x86_64 Linux) and the engine cannot see
    // the pair — so the core now comes from `resolveWideCharCore`, which each tier
    // answers from the fact it owns, and the value from the SHARED
    // `decodeWideCharCodepoint` the value tier runs, so an escape too wide for the
    // element is refused here exactly as it is there. No resolver, or no core for
    // this node, ⇒ not foldable, and the consumer's loud refusal stands.
    if (cfg.charBodyToken.valid()
        && ctx.integerLiteralTokens.contains(cfg.charBodyToken.v)) {
        NodeId        charBody{};
        SchemaTokenId opener{};
        bool          allTokens = !kids.empty();
        for (NodeId c : kids) {
            if (tree.kind(c) != NodeKind::Token) { allTokens = false; break; }
            SchemaTokenId const k = tree.tokenKind(c);
            if (!opener.valid()) {
                opener = k;   // the OPENER comes first: narrow `'`, or a prefixed one
                continue;
            }
            if (!charBody.valid() && k.v == cfg.charBodyToken.v) charBody = c;
        }
        bool const narrow = cfg.charStartToken.valid() && opener.valid()
                            && opener.v == cfg.charStartToken.v;
        if (allTokens && !narrow && opener.valid() && charBody.valid()
            && env.resolveWideCharCore) {
            if (auto const core = env.resolveWideCharCore(expr)) {
                auto const unit = decodeWideCharCodepoint(tree.text(charBody), *core);
                if (!unit.has_value()) {
                    return fail(ConstEvalFailure::NotAConstantExpression, expr);
                }
                // The constant's VALUE is its code unit read as the element type
                // (C 6.4.4.4p11): a signed element (`wchar_t` = `int`) holds a unit
                // with its top bit set as a NEGATIVE number — `L'\xffffffff'` is -1
                // on x86_64 Linux and 4294967295 on aarch64 Linux, as gcc 13.3.0 and
                // clang 18.1.3 answer (✔MEASURED). The literal-pool contract carries
                // a signed core in the int64 arm and an unsigned one in the uint64.
                // The element's width and sign from the ONE table the fold
                // arithmetic reads (`intKindInfo`); no plain-`char` core reaches
                // here, so no `char` answer is needed.
                auto const info = detail::intKindInfo(*core, std::nullopt);
                if (!info.has_value()) {
                    return fail(ConstEvalFailure::NotAConstantExpression, expr);
                }
                HirLiteralValue lv;
                lv.core = *core;
                std::uint64_t const raw = static_cast<std::uint64_t>(*unit);
                if (info->isSigned && info->bits < 64) {
                    std::uint64_t const sign = std::uint64_t{1} << (info->bits - 1);
                    lv.value = static_cast<std::int64_t>((raw ^ sign) - sign);
                } else {
                    lv.value = raw;
                }
                return ok(std::move(lv));
            }
        }
        if (allTokens && narrow && charBody.valid()) {
            auto const cp = decodeCharLiteralBody(tree.text(charBody));
            if (!cp.has_value()) {
                return fail(ConstEvalFailure::NotAConstantExpression, expr);
            }
            // [[D-CSUBSET-CHAR-HIGHBYTE-ICE-SIGNEDNESS]] /
            // [[D-CSUBSET-CONST-EVAL-CHAR-SIGNEDNESS]]: the decoder answers with
            // the CODE UNIT (0..255); C 6.4.4.4p10's VALUE is that unit read as a
            // plain `char` and converted to `int`, which is negative for a high
            // byte on a signed-`char` target. This arm used to take the unit
            // verbatim, so `int a[('\xff' < 0) ? 1 : 2]` sized 2 where gcc and
            // clang size 1 — rc 0, no diagnostic. The turn is made by the ONE
            // shared helper so the `#if` fold and value lowering cannot disagree.
            // A 0–127 body is the same integer either way and folds with no
            // target in hand; a high byte without one REFUSES rather than
            // guessing (the caller's loud "not a constant expression").
            if (narrowCharConstantSignednessMatters(*cp)
                && !options.charIsUnsigned.has_value()) {
                return fail(ConstEvalFailure::NotAConstantExpression, expr);
            }
            HirLiteralValue lv;
            lv.core  = TypeKind::I32;  // C 6.4.4.4: a char constant has type
                                       // `int`; consumers read `.value`
            lv.value = narrowCharConstantValue(
                *cp, options.charIsUnsigned.value_or(false));
            return ok(std::move(lv));
        }
    }

    // THE VALUE STEP THE STATEMENT LEAVES TO A DESCENT THAT ASKS A VALUE, by name
    // (`valueStepChild`), before the wrapper rule below — which takes its steps by the
    // statement (`wrapperChild`). DEAD-VIA-DRIVER: the driver takes both as frames. Every
    // other node that says something itself and reached this tail has no arm above (a label
    // address, a statement expression of anything else, a BRACE LIST — whose first element is
    // a scalar's value only to a reader that knows it reads a scalar,
    // `scalarInitializerValue`): no constant.
    if (NodeId const value = valueStepChild(tree, cfg, expr); value.valid()) {
        request = CstFoldFrame{.kind = CstFoldFrame::Kind::PassThrough, .node = value};
        return suspended(expr);
    }
    if (tree.kind(expr) == NodeKind::Internal && cfg.braceInitListRule.valid()
        && tree.rule(expr).v == cfg.braceInitListRule.v) {
        return fail(ConstEvalFailure::NotAConstantExpression, expr);
    }
    // Wrapper rule: grouping — an internal node with exactly one meaningful child
    // (an Internal) and any number of pure tokens (parens, separators), or a single
    // token-leaf wrapper (e.g. an `operand` rule whose sole content is an
    // integer-literal token or an identifier-Ref token: the Token-leaf branch above
    // dispatches it). The SAME decision the driver takes (`wrapperChild`).
    if (NodeId const child = wrapperChild(tree, cfg, expr, kids); child.valid()) {
        request = CstFoldFrame{.kind = CstFoldFrame::Kind::PassThrough, .node = child};
        return suspended(expr);
    }
    return fail(ConstEvalFailure::NotAConstantExpression, expr);
}

// ── Plan 24 Stage 6 — the iterative CST const-fold driver ──────────────────
// `enter` classifies a node: a plain binary, a unary, a value step or a wrapper is pushed as its
// frame (the plan helpers gate on the exact conditions `evalNode`'s arms would); every other node
// goes to `evalNode`, which folds it (a leaf, or an arm with no child to fold) or fills a frame
// for the child it needs, pushed here (the frame kinds: `CstFoldFrame`). The loop runs each
// frame's phase machine. The realloc-safe rule (the Stage-4 hir_to_mir `ValueFrame` idiom): copy
// a frame's fields to locals and advance `phase` BEFORE any `enter`, which may grow `work` and
// dangle `f`; copy `result` out before `pop_back`.
[[nodiscard]] ConstEvalResult
evalImpl(NodeId                              expr,
         CstEvalContext const&               ctx,
         CstEvalEnvironment const&           env,
         EvalOptions const&                  options,
         std::uint32_t                       initialScopeOpaque,
         std::unordered_set<std::uint32_t> & visitedInitNodes) {
    Tree const&              tree = ctx.tree;
    HirLoweringConfig const& cfg  = ctx.schema.hirLowering();

    std::vector<CstFoldFrame> work;
    // `enter` ALWAYS assigns `result` for a node it folds, and every pushed frame delivers into
    // `result` before it is read (then popped), so this sentinel never leaks.
    ConstEvalResult result = fail(ConstEvalFailure::NotAConstantExpression, expr);
    // P69 L2 (lane `lm`): how many open frames fold something that is NOT the expression asked
    // for as evaluated — a conditional's unselected arm (folded for its type alone) and another
    // object's initializer (a `Ref` / `Subobject`, its own expression). An admitted form folded
    // under one is not told to `env.noteConstantForm` (C 6.6p3 exempts an unevaluated
    // subexpression; the initializer is diagnosed by its own rule). Frames finish LIFO, so the
    // count is exact at every fold.
    std::uint32_t notTheAskedExpression = 0;

    auto const enter = [&](NodeId n, std::uint32_t scope) {
        if (n.valid() && tree.kind(n) == NodeKind::Internal) {
            auto kids = visibleChildren(tree, n);
            if (auto bp = plainBinaryPlan(tree, cfg, n, kids)) {
                work.push_back({.kind = CstFoldFrame::Kind::Binary, .node = n,
                                .entry = bp->entry, .scope = scope, .child = bp->lhsN,
                                .second = bp->rhsN});
                return;
            }
            if (auto up = unaryPlan(tree, cfg, n, kids)) {
                work.push_back({.kind = CstFoldFrame::Kind::Unary, .node = n,
                                .entry = up->entry, .scope = scope, .child = up->operandN});
                return;
            }
            // A value step (`valueStepChild`: a statement expression's sole expression) is a
            // frame too, so its nesting costs heap.
            if (NodeId const value = valueStepChild(tree, cfg, n); value.valid()) {
                work.push_back({.kind = CstFoldFrame::Kind::PassThrough, .node = value,
                                .scope = scope});
                return;
            }
            if (NodeId const child = wrapperChild(tree, cfg, n, kids); child.valid()) {
                work.push_back({.kind = CstFoldFrame::Kind::PassThrough, .node = child,
                                .scope = scope});
                return;
            }
        }
        CstFoldFrame request{.kind = CstFoldFrame::Kind::PassThrough, .node = NodeId{}};
        ConstEvalResult folded =
            evalNode(n, ctx, env, options, scope, visitedInitNodes, request);
        if (!request.node.valid()) {
            result = std::move(folded);
            return;
        }
        request.scope = scope;
        work.push_back(std::move(request));
    };

    enter(expr, initialScopeOpaque);
    while (!work.empty()) {
        CstFoldFrame& f = work.back();
        std::uint32_t const scope = f.scope;
        NodeId const        node  = f.node;
        switch (f.kind) {
        case CstFoldFrame::Kind::PassThrough:
            // `node` IS the child: its folded `result` is this frame's.
            work.pop_back();
            enter(node, scope);
            break;
        case CstFoldFrame::Kind::Unary:
            if (f.phase == 0) {
                f.phase = 1;
                NodeId const operandN = f.child;
                enter(operandN, scope);              // may invalidate `f`
            } else {
                NodeId const            operandN = f.child;
                HirOperatorEntry const* e        = f.entry;
                ConstEvalResult operand = std::move(result);
                work.pop_back();
                // The operand, a bit-field's value promoted (`asBitFieldOperand`).
                operand = asBitFieldOperand(operandN, std::move(operand), env, options, scope);
                result = combineUnaryCst(node, *e, options, std::move(operand));
            }
            break;
        case CstFoldFrame::Kind::Binary:
            // LHS first (phase 0→1), then RHS (phase 1→2) — left-to-right, platform-independent.
            if (f.phase == 0) {
                f.phase = 1;
                NodeId const lhsN = f.child;
                enter(lhsN, scope);                  // may invalidate `f`
            } else if (f.phase == 1) {
                f.c0 = std::move(result);
                f.phase = 2;
                NodeId const rhsN = f.second;
                enter(rhsN, scope);                  // may invalidate `f`
            } else {
                NodeId const            lhsN = f.child;
                NodeId const            rhsN = f.second;
                HirOperatorEntry const* e    = f.entry;
                ConstEvalResult lhs = std::move(f.c0);
                ConstEvalResult rhs = std::move(result);
                work.pop_back();
                // Each operand, a bit-field's value promoted (`asBitFieldOperand`).
                lhs = asBitFieldOperand(lhsN, std::move(lhs), env, options, scope);
                rhs = asBitFieldOperand(rhsN, std::move(rhs), env, options, scope);
                // ★ P69 L2 (lane `lm`): THE COMMA OPERATOR. C 6.6p3 keeps it out of a constant
                // expression; the language admits it where the position asked for admits the
                // `commaOperator` form (`EvalOptions::admittedForms` — the caller's position,
                // `IntegerConstantPosition`). The discarded LEFT operand must fold too (both
                // references refuse `(y, 1)` for a non-constant `y`, ✔MEASURED); the value is
                // the RIGHT operand's, a bit-field's promoted as above — clang's comma hands its
                // right operand's bit-field on. Not admitted: refused AT the comma.
                if (e->target == "Comma") {
                    if (!options.admittedForms.admits(ConstantForm::CommaOperator)) {
                        result = fail(ConstEvalFailure::NotAConstantExpression, node);
                    } else if (!lhs.value.has_value()) {
                        result = std::move(lhs);
                    } else {
                        if (rhs.value.has_value() && notTheAskedExpression == 0
                            && env.noteConstantForm) {
                            env.noteConstantForm(ConstantForm::CommaOperator, node);
                        }
                        result = std::move(rhs);
                    }
                    break;
                }
                result = combineBinaryCst(node, *e, options, std::move(lhs), std::move(rhs));
            }
            break;
        case CstFoldFrame::Kind::Conditional:
            if (f.phase == 0) {
                f.phase = 1;
                NodeId const condN = f.child;
                enter(condN, scope);                 // may invalidate `f`
            } else if (f.phase == 1) {
                ConstEvalResult cond = std::move(result);
                if (!cond.value.has_value()) {
                    work.pop_back();
                    result = std::move(cond);
                    break;
                }
                auto const condTrue = asBool(*cond.value, options.allowFloat);
                if (!condTrue.has_value()) {
                    work.pop_back();
                    result = fail(ConstEvalFailure::UnsupportedTypeKind, node);
                    break;
                }
                NodeId const selected = *condTrue ? f.second : f.third;
                NodeId const other    = *condTrue ? f.third : f.second;
                f.child  = selected;
                f.second = other;
                f.phase  = 2;
                enter(selected, scope);              // may invalidate `f`
            } else if (f.phase == 2) {
                ConstEvalResult arm =
                    asBitFieldOperand(f.child, std::move(result), env, options, scope);
                if (!arm.value.has_value()) {
                    work.pop_back();
                    result = std::move(arm);
                    break;
                }
                std::optional<CstCastTarget> const tierType =
                    env.resolveExpressionType ? env.resolveExpressionType(node, scope)
                                              : std::nullopt;
                if (tierType.has_value() && tierType->isPointer) {   // a pointer conditional
                    work.pop_back();
                    result = std::move(arm);
                    break;
                }
                f.c0     = std::move(arm);
                f.target = tierType;
                f.phase  = 3;
                NodeId const other = f.second;
                ++notTheAskedExpression;             // the unselected arm: its type only
                enter(other, scope);                 // may invalidate `f`
            } else {
                --notTheAskedExpression;
                NodeId const                       other    = f.second;
                ConstEvalResult                    arm      = std::move(f.c0);
                std::optional<CstCastTarget> const tierType = f.target;
                work.pop_back();
                ConstEvalResult const otherValue =
                    asBitFieldOperand(other, std::move(result), env, options, scope);
                result = balanceConditionalArm(node, other, otherValue, std::move(arm), tierType,
                                               ctx, env, options, scope);
            }
            break;
        case CstFoldFrame::Kind::Logical:
            if (f.phase == 0) {
                f.phase = 1;
                NodeId const lhsN = f.child;
                enter(lhsN, scope);                  // may invalidate `f`
            } else if (f.phase == 1) {
                ConstEvalResult a = std::move(result);
                if (!a.value.has_value()) {
                    work.pop_back();
                    result = std::move(a);
                    break;
                }
                auto const aTrue = asBool(*a.value, options.allowFloat);
                if (!aTrue.has_value()) {
                    work.pop_back();
                    result = fail(ConstEvalFailure::UnsupportedTypeKind, node);
                    break;
                }
                if (f.isAnd ? !*aTrue : *aTrue) {    // short-circuits: the RHS is never folded
                    work.pop_back();
                    result = ok(makeBoolLiteral(*aTrue ? 1 : 0));
                    break;
                }
                f.phase = 2;
                NodeId const rhsN = f.second;
                enter(rhsN, scope);                  // may invalidate `f`
            } else {
                ConstEvalResult b = std::move(result);
                work.pop_back();
                if (!b.value.has_value()) {
                    result = std::move(b);
                    break;
                }
                auto const bTrue = asBool(*b.value, options.allowFloat);
                if (!bTrue.has_value()) {
                    result = fail(ConstEvalFailure::UnsupportedTypeKind, node);
                    break;
                }
                result = ok(makeBoolLiteral(*bTrue ? 1 : 0));
            }
            break;
        case CstFoldFrame::Kind::Convert:
            if (f.phase == 0) {
                f.phase = 1;
                NodeId const operandN = f.child;
                enter(operandN, scope);              // may invalidate `f`
            } else {
                CstCastTarget const target = *f.target;
                ConstEvalResult inner = std::move(result);
                work.pop_back();
                if (inner.value.has_value()) {
                    result = convertToCastTarget(inner, target, options, node);
                } else {
                    result = std::move(inner);
                }
            }
            break;
        case CstFoldFrame::Kind::Ref:
        case CstFoldFrame::Kind::Subobject:
            if (f.phase == 0) {
                f.phase = 1;
                NodeId const        initN     = f.child;
                std::uint32_t const initScope = f.childScope;
                ++notTheAskedExpression;             // another object's initializer
                enter(initN, initScope);             // may invalidate `f`
            } else {
                --notTheAskedExpression;
                CstFoldFrame const done = std::move(f);
                work.pop_back();
                visitedInitNodes.erase(done.initKey);
                ConstEvalResult inner = std::move(result);
                if (!inner.value.has_value()) {
                    inner.blamedNode = HirNodeId{node.v};
                    result = std::move(inner);
                    break;
                }
                ConstEvalResult atType = convertToCastTarget(inner, *done.target, options, node);
                // A BIT-FIELD member is read at its width, at its declared type
                // (`readThroughBitField`).
                if (done.kind == CstFoldFrame::Kind::Subobject && done.bitField.has_value()
                    && atType.value.has_value()) {
                    atType = readThroughBitField(atType, *done.target, *done.bitField, node);
                }
                result = std::move(atType);
            }
            break;
        case CstFoldFrame::Kind::Member:
            if (f.phase == 0) {
                f.phase = 1;
                NodeId const baseN = f.child;
                enter(baseN, scope);                 // may invalidate `f`
            } else {
                NodeId const fieldTok = f.second;
                work.pop_back();
                result = memberAddressCst(node, fieldTok, std::move(result), env);
            }
            break;
        case CstFoldFrame::Kind::AddressOf:
            if (f.phase == 0) {
                f.phase = 1;
                NodeId const operandN = f.child;
                enter(operandN, scope);              // may invalidate `f`
            } else {
                ConstEvalResult inner = std::move(result);
                work.pop_back();
                // The address OF an lvalue at a known offset is that address; the address of a
                // plain value is no constant.
                if (inner.value.has_value() && asAddress(*inner.value) == nullptr) {
                    result = fail(ConstEvalFailure::NotAConstantExpression, node);
                } else {
                    result = std::move(inner);
                }
            }
            break;
        case CstFoldFrame::Kind::Builtin: {
            // `phase` counts the operands asked for; the one before it has just folded.
            CstBuiltinCall const& bc = *f.call;
            if (f.phase > 0) {
                std::size_t const i = f.phase - 1;
                ConstEvalResult inner = std::move(result);
                if (inner.value.has_value() && i < bc.argTypes.size()
                    && bc.argTypes[i].has_value()) {
                    inner = convertToCastTarget(inner, *bc.argTypes[i], options, bc.args[i]);
                    if (inner.value.has_value())
                        f.cores.push_back(builtinOperandCore(*bc.argTypes[i]));
                } else if (inner.value.has_value()) {
                    f.cores.push_back(inner.value->core);
                }
                if (!inner.value.has_value()) {
                    work.pop_back();
                    result = std::move(inner);
                    break;
                }
                f.values.push_back(std::move(*inner.value));
            }
            if (f.phase < bc.args.size()) {
                NodeId const argN = bc.args[f.phase];
                ++f.phase;
                enter(argN, scope);                  // may invalidate `f` (and `bc`)
                break;
            }
            CstCastTarget const resultType = bc.resultType;
            auto folded = foldBuiltinVerb(bc.verb, f.values, f.cores,
                                          builtinOperandCore(resultType));
            work.pop_back();
            if (!folded.has_value()) {
                result = fail(ConstEvalFailure::NotAConstantExpression, node);
            } else {
                result = convertToCastTarget(ok(std::move(*folded)), resultType, options, node);
            }
            break;
        }
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

std::optional<bool> asBoolBridge(HirLiteralValue const& v,
                                 bool allowFloat) noexcept {
    return detail::asBool(v, allowFloat);
}

// [[D-C-FLOAT-CAST-DOES-NOT-FOLD-IN-A-CONSTANT-EXPRESSION]]: the ONE classifier.
// Moved here from `semantic_analyzer.cpp`'s `resolveCastTarget` closure when the
// HIR-lowering tier needed the same answer for an index designator's
// `[(int)1.5]`. Copying the switch would have made the two tiers' cast surfaces
// drift silently apart — which is not hypothetical: the `isInteger` rows for
// I128/U128 were once absent from the ONE copy that existed, and the MEASURED
// symptom was every 128-bit constant expression refusing.
std::optional<CstCastTarget>
classifyCstCastTarget(TypeInterner const& in, TypeId ty,
                      std::optional<bool> charIsUnsigned) {
    if (!ty.valid()) return std::nullopt;
    CstCastTarget t;
    TypeKind const k = in.kind(ty);
    if (k == TypeKind::Ptr) {
        t.isPointer = true;
        auto const ops = in.operands(ty);
        if (!ops.empty()) t.pointeeType = ops[0];
        return t;
    }
    // A FLOAT target folds through C 6.3.1.4/6.3.1.5 at the target's OWN format.
    // The whole classifier used to fall to `default: nullopt` here, so
    // `_Static_assert((double)3 == 3.0, "")` — accepted by gcc 13.3.0, clang
    // 18.1.3, mingw-w64 gcc 13.2.0 and MSVC 19.51, probed separately — was refused.
    if (k == TypeKind::F16 || k == TypeKind::F32 || k == TypeKind::F64
        || k == TypeKind::F80 || k == TypeKind::F128) {
        t.floatKind = k;
        return t;
    }
    TypeKind ik = k;   // an enum casts as its underlying integer
    if (k == TypeKind::Enum) {
        auto const sc = in.scalars(ty);
        if (!sc.empty()) ik = static_cast<TypeKind>(sc[0]);
    }
    switch (ik) {
        // ⚠ `_Bool` IS NOT `isInteger`. C 6.3.1.2 converts BY COMPARISON to zero,
        // which a width-1 truncation gets wrong for every even value — see the
        // `isBool` note on the descriptor for the measured 41-vs-42 array bound.
        case TypeKind::Bool: t.isBool   = true;  break;
        // ⚠ `Char` IS NOT `I8`. `signed char` (I8) is signed by the standard;
        // plain `char` is whichever the TARGET declares (C 6.2.5p15), so it reads
        // the threaded answer and REFUSES when there is none. Sharing I8's row was
        // the defect: `(char)200` folded to -56 on the unsigned-`char` arm64 leg
        // too, silently, in every array-dimension and enumerator const-expr.
        case TypeKind::Char:
            t.isInteger=true; t.intBits=8;
            t.intSigned = charIsUnsigned.has_value() ? !*charIsUnsigned : false;
            t.intSignednessUnknown = !charIsUnsigned.has_value();
            break;
        case TypeKind::I8:   t.isInteger=true; t.intBits=8;  t.intSigned=true;  break;
        case TypeKind::U8:   t.isInteger=true; t.intBits=8;  t.intSigned=false; break;
        case TypeKind::I16:  t.isInteger=true; t.intBits=16; t.intSigned=true;  break;
        case TypeKind::U16:  t.isInteger=true; t.intBits=16; t.intSigned=false; break;
        case TypeKind::I32:  t.isInteger=true; t.intBits=32; t.intSigned=true;  break;
        case TypeKind::U32:  t.isInteger=true; t.intBits=32; t.intSigned=false; break;
        case TypeKind::I64:  t.isInteger=true; t.intBits=64; t.intSigned=true;  break;
        case TypeKind::U64:  t.isInteger=true; t.intBits=64; t.intSigned=false; break;
        // ★ D-CSUBSET-INT128-CONSTFOLD-WIDE (TF-C94): the two 128-bit STANDARD kinds.
        // WITHOUT these rows they fell to `default: nullopt` and the whole cast was
        // non-foldable — which MEASURED as *every* 128-bit integer constant
        // expression refusing, not just wide ones:
        // `_Static_assert((__uint128_t)5 == 5, "")` fired S_StaticAssertFailed
        // ("not an integer constant expression"), and so did `(__uint128_t)5`,
        // `((__uint128_t)5 + 1) == 6` and `(int)((__uint128_t)5) == 5`, while the
        // `_BitInt(128)` twin of the first was already clean. These rows are what
        // make the 128-bit arm in the Cast fold (which routes them through the
        // bignum, NOT through the int64 `narrowIntToBits`) reachable at all.
        //   ★ WHY `isInteger` AND NOT `isBitPrecise`: a `__int128` is a
        // STANDARD-rank integer, not a bit-precise one. Carrying it as
        // `isBitPrecise` would tag the folded result `TypeKind::BitInt` and a
        // `__int128` expression would silently change type mid-fold (the exact
        // hazard `bitIntOperandType` orders its checks against). The 128-bit Cast
        // arm keys on `intBits == 128` and mints an I128/U128 core.
        //   ★ NO TRUNCATION IS OPENED BY THIS — MEASURED, and this is the
        // load-bearing safety property. ⚠ THE RULE IS THE VALUE'S MAGNITUDE, NOT
        // ITS DECLARED WIDTH — it was width until P42 closed
        // [[D-CE-ASINT64-REJECTS-BY-WIDTH-NOT-MAGNITUDE]], and a rationale that has
        // gone false is worse than none. The folded value rides a 128-bit
        // `BitIntValue`, and `asInt64` (const_eval_arith.hpp) bridges it to the
        // 64-bit ICE slots IFF `INT64_MIN <= value <= INT64_MAX`; anything outside
        // that range nullopts, so `asInt64Bridge` — the array-dimension /
        // static-assert / enumerator bridge — fails loud rather than narrowing, and
        // the generic `isInteger` narrowing arm is never reached for an
        // out-of-range operand for the same reason. `_BitInt(128)` obeys the
        // identical rule (one predicate, both 128-bit families):
        // `int a[(_BitInt(128))5];` ACCEPTS at 5, while
        // `int a[((__int128)1 << 100) + 3];` MEASURED S_NonConstantArrayLength —
        // and THAT is the cell that proves it, because 2^100+3 has low 64 bits of
        // exactly 3, so a truncating fold would have built a silent `int a[3]`
        // instead of refusing. Pinned by
        // `SemanticAnalyzerC.Int128WideConstantNeverTruncatesIntoA64BitSlot`.
        case TypeKind::I128: t.isInteger=true; t.intBits=128; t.intSigned=true;  break;
        case TypeKind::U128: t.isInteger=true; t.intBits=128; t.intSigned=false; break;
        // C23 6.3.1.3 (D-CSUBSET-BITINT-CONSTFOLD-LARGE, C4b): a cast TO
        // `_BitInt(N)` folds via the wrap-aware bignum at width N (mod-2^N), for
        // ANY N (narrow AND wide) — `(_BitInt(4))15 + 1 == 0` /
        // `(_BitInt(40))2000000 * ...`. Carried as the bit-precise descriptor (NOT
        // `isInteger`, whose `intBits` maxes at 64 and whose narrow fold cannot
        // express a wide `_BitInt`).
        case TypeKind::BitInt:
            t.isBitPrecise = true;
            t.bitWidth = static_cast<std::uint32_t>(in.bitIntWidth(ty));
            t.bitSigned = in.bitIntIsSigned(ty);
            break;
        default: return std::nullopt;   // aggregate / function / void — non-foldable
    }
    return t;
}

std::optional<CstResolvedSymbol>
namedConstantRead(TypeInterner const& in, NodeId initExpr, std::uint32_t initScopeOpaque,
                  TypeId declaredType, std::optional<bool> charIsUnsigned) {
    if (!declaredType.valid()) return std::nullopt;
    CstResolvedSymbol out{initExpr, initScopeOpaque, std::nullopt};
    if (auto const conversion = classifyCstCastTarget(in, declaredType, charIsUnsigned);
        conversion.has_value()) {
        out.declaredType = conversion;
        return out;
    }
    switch (in.kind(declaredType)) {
        case TypeKind::Struct: case TypeKind::Union: case TypeKind::Tuple:
        case TypeKind::Array:  case TypeKind::Slice:
            return out;   // read member by member, each at its own type; whole, no constant
        default:
            return std::nullopt;   // a type with no constant value at its own type
    }
}

// The header says what this is. Each step is to a strict descendant (an element's value, or
// a wrapper's one child), so the tree's node count bounds the walk; past it the node graph
// is cyclic — never the program's fault — and the process stops. No recursion: a scalar in
// as many braces as the parser admits costs one loop.
CstScalarInitializer
scalarInitializerValue(Tree const& tree, GrammarSchema const& schema, NodeId init) {
    HirLoweringConfig const& cfg = schema.hirLowering();
    NodeId cur = init;
    for (substrate::WalkSteps steps{tree.nodeCount(), "the scalar initializer's brace steps"};;
         steps.step()) {
        if (!cur.valid() || tree.kind(cur) != NodeKind::Internal) return {cur, false};
        auto const kids = visibleChildren(tree, cur);
        if (cfg.braceInitListRule.valid() && tree.rule(cur).v == cfg.braceInitListRule.v) {
            NodeId element{};
            for (NodeId c : kids) {
                if (tree.kind(c) != NodeKind::Internal) continue;
                element = c;   // the first element; any after it is excess
                break;
            }
            if (!element.valid()) return {NodeId{}, true};   // `{}`: the scalar's zero
            NodeId value{};
            std::size_t parts = 0;
            for (NodeId c : visibleChildren(tree, element)) {
                if (tree.kind(c) != NodeKind::Internal) continue;
                ++parts;
                value = c;
            }
            // A designated element has two parts, the designation and the value: no value.
            if (parts != 1) return {};
            cur = value;
            continue;
        }
        // A transparent wrapper between two levels (the evaluator's own step): peeled, so a
        // brace list under it is a level too; anything else is the value.
        NodeId const child = wrapperChild(tree, cfg, cur, kids);
        if (!child.valid()) return {cur, false};
        cur = child;
    }
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
