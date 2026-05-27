#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Per-language CST→HIR lowering config (schema v4 `hirLowering` block; plan 09
// HR8). The POD vocabulary a language declares so the single language-agnostic
// `CstToHirLowering` engine (src/hir/lowering/) can lower its CST to HIR. The
// engine reads ONLY this struct (+ the existing `semantics` block) — it NEVER
// branches on `schema.name()`. A new language lowers by adding a `hirLowering`
// block; absent ⇒ that language has no lowering (the engine produces nothing).
//
// LAYERING: this lives in `core` (like `semantic_config.hpp`) so `GrammarSchema`
// can own it. It therefore CANNOT name the `hir`-layer `HirKind`/`HirOpKind`
// enums directly — it stores their NAMES as strings. The schema loader validates
// JSON shape + rule/token references here; the lowering engine (which sees the
// `hir` layer) resolves the name strings to enums and reports an unknown name as
// `H_UnsupportedLoweringForKind` at engine construction.
//
// DELIBERATE REUSE of the `semantics` block (no duplication): declaration shapes
// — which child is the name / type / params / body, and the function-vs-variable
// `kindByChild` discriminator — already live in `SemanticConfig::declarations`,
// and literal token→type already lives in `SemanticConfig::literalTypes`. The
// engine consults those directly. `hirLowering` carries only what `semantics`
// does not: statement→HirKind mappings, the Pratt expression rule ids, the
// operator-token dispatch tables, and the structural specials below.

namespace dss {

// A CST rule that maps one-to-one to a core HIR statement/declaration kind.
// `hirKind` is the HirKind enum-member NAME (e.g. "Block", "IfStmt", "Function").
// The engine recognizes the node's role children (condition / body / value /
// init / ...) by CLASSIFYING each visible child as an expression node, a
// statement node, or a token — not by fixed indices — so optional keywords and
// punctuation need no configuration. (The for-loop's optional `;`-separated
// clauses are the one exception; see `forClauseSeparator`.)
struct DSS_EXPORT HirRuleMapping {
    RuleId      rule{};
    std::string hirKind;     // HirKind member name, resolved by the engine
    std::string ruleName;    // source-text rule name, retained for diagnostics
};

// One operator-token → HIR target. Used for the three Pratt wrapper rules
// (binaryExpr/unaryExpr/postfixExpr), whose operator token discriminates which
// HIR node to build. `target` is interpreted by the engine:
//   - a core HirOpKind name ("Add", "Sub", "Eq", "Lt", "Neg", "Not", "BitNot", …)
//     → makeBinaryOp / makeUnaryOp with that op;
//   - a special tag the engine handles directly: "LogicalAnd", "LogicalOr"
//     (short-circuit, distinct HIR kinds), "Assign" (→ AssignStmt),
//     "AddressOf", "Deref", "Call", "Index".
// `compoundBase` (optional, e.g. "Add" for `+=`) documents a compound-assign's
// base op; HR8 does not yet lower compound-assign (it needs once-only lvalue
// evaluation HIR can't express) and emits H_UnsupportedLoweringForKind, but the
// field carries the intent forward to HR9.
struct DSS_EXPORT HirOperatorEntry {
    SchemaTokenId token{};
    std::string   target;
    std::string   compoundBase;   // empty unless this is a compound-assign
    std::string   tokenName;      // source-text token name, for diagnostics
};

// The full `hirLowering` block. Every facet optional; absent ⇒ not lowered.
struct DSS_EXPORT HirLoweringConfig {
    // Statement + declaration rules that map to a HIR kind. Declaration rules
    // (topLevelDecl/varDecl/varDeclHead/param/typedefDecl) name their HIR kind
    // here ("Function"/"Global" are decided by the engine via the semantics
    // `kindByChild` discriminator when hirKind == "Decl"); statement rules name
    // it directly ("Block"/"IfStmt"/…).
    std::vector<HirRuleMapping> ruleMappings;

    // The three Pratt wrapper rule ids + the atom rule id. The engine recognizes
    // a CST node as an expression iff its rule is one of these (so it can both
    // dispatch and classify children). Resolved from the language's `expr`-shape
    // wrapperRules / atom.
    RuleId      binaryExprRule{};   std::string binaryExprRuleName;
    RuleId      unaryExprRule{};    std::string unaryExprRuleName;
    RuleId      postfixExprRule{};  std::string postfixExprRuleName;
    RuleId      ternaryExprRule{};  std::string ternaryExprRuleName;  // optional (mixfix `?:`)
    RuleId      operandRule{};      std::string operandRuleName;

    // Operator-token dispatch for each Pratt wrapper.
    std::vector<HirOperatorEntry> binaryOps;
    std::vector<HirOperatorEntry> unaryOps;
    std::vector<HirOperatorEntry> postfixOps;

    // The `;`-style token that separates a C-for's init/cond/update clauses
    // inside its parens. The engine segments the for-header by this token
    // (positional indexing fails because each clause is independently optional).
    SchemaTokenId forClauseSeparator{};  std::string forClauseSeparatorName;

    // Rules that are explicitly NOT lowered yet (deferred to a later plan, e.g.
    // c-subset's `arrayDeclSuffix` — arrays need an Array type the lattice/
    // semantic phase don't resolve). When a declaration's subtree contains one,
    // the engine fails loud (H_UnsupportedLoweringForKind) rather than silently
    // miscompiling (e.g. lowering `int a[10]` to a scalar with `10` as the init).
    std::vector<RuleId> deferredRules;

    // Flat C-style switch grouping: `caseLabelRule` is the case/default label
    // rule; statements following a label (until the next label) form that arm's
    // body. `caseDefaultToken` is the token (e.g. DefaultKeyword) that marks the
    // default arm (no match value). A valued label's match expression is found
    // by the engine as the label's expression child.
    RuleId        caseLabelRule{};     std::string caseLabelRuleName;
    SchemaTokenId caseDefaultToken{};  std::string caseDefaultTokenName;

    // Char / string literal lowering. A value-bearing body literal materializes
    // in an operand as a small subtree [startToken, bodyToken] where bodyToken
    // is the COALESCED body (one in-grammar token; see DefaultTokenSpec.coalesce).
    // The engine decodes the body token's text (C-family escapes) into the
    // literal pool: a char → a Char codepoint, a string → its bytes typed
    // Array<Char, N+1>. Both blocks optional; invalid ⇒ the language has no such
    // literal.
    SchemaTokenId charStartToken{};    std::string charStartTokenName;
    SchemaTokenId charBodyToken{};     std::string charBodyTokenName;
    SchemaTokenId stringStartToken{};  std::string stringStartTokenName;
    SchemaTokenId stringBodyToken{};   std::string stringBodyTokenName;

    // True when the block carries no facets — the engine then does nothing.
    [[nodiscard]] bool empty() const noexcept {
        return ruleMappings.empty() && binaryOps.empty() && unaryOps.empty()
            && postfixOps.empty();
    }
};

} // namespace dss
