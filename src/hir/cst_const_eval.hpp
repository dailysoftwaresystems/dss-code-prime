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

// FC6: SizeOf-folding resolver for a const-expr context (an array dimension
// `int a[sizeof(T)]`). Given the `sizeofRule` CST node, return its byte size,
// or `nullopt` when un-sizeable / the target declared no layout params / the
// operand's type is not yet resolved at this pass. The closure (supplied by the
// semantic engine) owns the type resolver + the target's layout params — kept
// OUT of this CST engine, which stays HIR-/interner-free. Absent closure ⇒
// SizeOf is non-constant. The engine dispatches it by rule-id BEFORE the
// wrapper-peel (the sizeof node's single meaningful child is its type-ref, which
// the peel would otherwise descend into and reject — or, for the value form,
// fold to the operand's VALUE instead of its size).
using CstSizeofResolver =
    std::function<std::optional<std::uint64_t>(NodeId)>;

// C11/C23 6.5.3.4: AlignOf-folding resolver for a const-expr context (an array
// dimension `int a[_Alignof(T)]`, a `_Static_assert(_Alignof(T)==N,...)`). Given
// the `alignofRule` CST node, return its type's ALIGNMENT, or `nullopt` when
// un-alignable / the target declared no layout params. An ADDITIVE mirror of
// CstSizeofResolver — same shape, reads alignment instead of size. The closure
// (supplied by the semantic engine) owns the type resolver + the target's layout
// params, kept OUT of this CST engine. Absent closure ⇒ AlignOf is non-constant.
// The engine dispatches it by rule-id BEFORE the wrapper-peel (the alignof node's
// single meaningful child is its castTypeRef, which the peel would otherwise
// descend into and reject).
using CstAlignofResolver =
    std::function<std::optional<std::uint64_t>(NodeId)>;

// Item 1 (shipped-header constants / enum array-dim): DIRECT-VALUE resolver for
// a named INTEGER CONSTANT whose value is carried inline on its symbol rather
// than in a defining init-expression CST — an enum enumerator or a shipped-
// descriptor-injected constant. Given the identifier-token node + current scope,
// returns the constant's literal value DIRECTLY (no init-CST to recurse into),
// or nullopt when the name is not such a constant. The engine tries this BEFORE
// `resolveSymbolInit`, so `int a[CHAR_BIT]` / `int b[ENUM_VAL]` fold even though
// neither symbol has an init-expression the init-resolver could walk. The
// closure (supplied by each consumer) shares the ONE `constantLiteralForSymbol`
// builder with the HIR Ref fold, so value- and const-expr-position agree.
using CstSymbolValueResolver =
    std::function<std::optional<HirLiteralValue>(NodeId, std::uint32_t)>;

// c43 (D-CSUBSET-ADDRESS-CONSTANT-FOLD / Option A): resolved cast-target shape.
// The CST engine is interner-free, so the closure (semantic side) introspects the
// resolved target type and hands back exactly what the fold needs: whether it is a
// pointer (an int 0 → a null address; ptr→ptr identity) and, for an integer
// target, its width/signedness (so the engine narrows an int cast correctly —
// e.g. the `(size_t)` that terminates the offsetof idiom — without a TypeInterner).
struct CstCastTarget {
    bool   isPointer = false;
    bool   isInteger = false;
    int    intBits   = 0;       // integer target width (8/16/32/64)
    bool   intSigned = false;
    TypeId pointeeType{};       // a pointer target's pointee (retype / future stride)
};

// c43: cast-target resolver — given a cast CST node, return its TARGET descriptor
// (via resolveTypeNode + a width/pointer classification), or nullopt. Absent ⇒
// casts stay non-foldable (the prior behaviour). Recognizes `(T*)0`, `(char*)x`,
// and `(size_t)int` (the offsetof spine).
using CstCastTargetResolver = std::function<std::optional<CstCastTarget>(NodeId)>;

// c43: field-offset resolver — given a struct/union CONTAINER TypeId + a field-name
// token, return the field's {byteOffset, fieldType}, or nullopt (unknown field /
// bit-field / no layout params / not a composite). Owns computeLayout + the
// composite member-scope lookup; lets the engine fold `&((T*)0)->M` member offsets.
struct CstFieldResolution { std::uint64_t offset = 0; TypeId fieldType{}; };
using CstFieldOffsetResolver =
    std::function<std::optional<CstFieldResolution>(TypeId, NodeId)>;

struct CstEvalEnvironment {
    CstSymbolInitResolver  resolveSymbolInit{};
    CstSymbolValueResolver resolveSymbolValue{};  // Item 1 — direct inline constant value
    CstSizeofResolver      resolveSizeof{};   // FC6 — sizeof in a const-expr context
    CstAlignofResolver     resolveAlignof{};  // 6.5.3.4 — _Alignof in a const-expr context
    CstCastTargetResolver  resolveCastTarget{};   // c43 — (T*)0 / (char*)x / (size_t)int
    CstFieldOffsetResolver resolveFieldOffset{};  // c43 — &((T*)0)->M offsets
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
// FC4 c1: DECLARATOR-mode rows carry one init PER init-declarator
// (`const int K = 8, L = 9;`), so the caller passes the symbol's NAME
// node (`SymbolRecord::declNode`) and the helper returns the init of
// the init-declarator that declares THAT name (via the shared
// declarator walk over the schema's `declarators` roles). `nameNode`
// is ignored for legacy positional rows.
//
// Forward-declared `DeclarationRule` is included via core/types; the
// helper definition resolves it from the loaded schema.
struct DeclarationRule;
[[nodiscard]] DSS_EXPORT std::optional<NodeId>
findInitExprInDecl(Tree const& tree, DeclarationRule const& decl,
                   NodeId declRuleNode, NodeId nameNode = {});

} // namespace dss
