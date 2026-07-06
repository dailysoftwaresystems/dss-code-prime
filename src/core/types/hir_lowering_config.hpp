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
// HR10: one role-child slot of an extension node. The engine LOCATES a child of
// the mapped CST node (by sub-rule name, or by the "expr" classifier) and LOWERS
// it one way. Children named by no slot (keywords, punctuation) are dropped. This
// is the generic, language-agnostic vocabulary — the engine reads it, never SQL
// rule/kind names. `list` lowers EACH item of a matched list rule (a grouping
// node's children); `optional` lets an absent match produce no child.
// How the engine lowers a located child (or each item of a `list` slot). A
// closed set of engine dispatch verbs — NOT an `hir`-layer enum, so it lives in
// this `core` header and the loader validates the JSON string against it at load
// time (an unknown verb is a config error reported then, never at lowering time).
enum class ChildLower : std::uint8_t {
    Expr,      // lowerExpr (the language's general expression rule)
    FlatExpr,  // lowerFlatExpr (a flat `operand (op operand)*` sequence)
    Ext,       // lowerExtensionNode (the located rule is itself extension-mapped)
    Ref,       // a name reference (core Ref, or refExtensionKind leaf) to the resolved name
    VarDecl,   // lowerVarLike (core VarDecl, reusing the semantics declaration)
};

struct DSS_EXPORT ChildSlotSpec {
    RuleId      matchRule{};      std::string matchRuleName;  // sub-rule to find (xor classifier)
    std::string classifier;       // "expr" — the first expression child (xor matchRule)
    bool        optional = false;
    bool        list     = false; // matched node is a list: lower each of its item children
    ChildLower  lower    = ChildLower::Expr;   // how to lower the located node / each list item
    std::string role;             // human label (diagnostics + .dsshir readability); engine ignores
};

struct DSS_EXPORT HirRuleMapping {
    RuleId      rule{};
    std::string hirKind;     // core HirKind name OR a registered extension-kind name
    std::string ruleName;    // source-text rule name, retained for diagnostics
    // HR10: when `hirKind` names an extension kind, the role-children of the
    // Extension node, gathered generically from the CST node's children. Empty
    // for core-kind mappings (which have bespoke lowering).
    std::vector<ChildSlotSpec> childGathering;
};

// HR10: a HIR extension kind to register before lowering (language/domain-
// qualified, e.g. "TSQL::Select"). The engine registers each into the builder's
// HirKindRegistry so a rule mapped to this name lowers to a HirKind::Extension
// node carrying the minted HirKindId; .dsshir round-trips via the registry preamble.
struct DSS_EXPORT ExtensionKindEntry {
    std::string name;   // language/domain-qualified, e.g. "TSQL::Select"
    // The owning language as the registry records it — independent of `name`'s
    // `::` prefix (e.g. name "TSQL::Select" but lang "TsqlSubset"), so it is NOT
    // derivable from the prefix.
    std::string lang;
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
    //   `caseStmtRule` (optional) is the C 6.8.1 labeled-statement form of a case
    // — `caseLabel statement` — which lets a goto-label precede a case
    // (`foo: case 1: stmt`, D-CSUBSET-LABEL-BEFORE-CASE). When valid, lowerSwitch
    // peels a (label-wrapped) caseStmt into an arm; a caseStmt reached as a plain
    // statement (NOT a direct switch-body item) is the fail-loud outside-switch
    // case. Invalid (unset) for languages without the form (toy/tsql) -> no-op.
    RuleId        caseLabelRule{};     std::string caseLabelRuleName;
    RuleId        caseStmtRule{};      std::string caseStmtRuleName;
    SchemaTokenId caseDefaultToken{};  std::string caseDefaultTokenName;

    // c115 SEH (both optional — unset for languages without `__try`): the two
    // handler-arm sub-rules of the try statement. The engine identifies which
    // arm parsed by RULE identity: the except arm carries [filterExpr, block]
    // children; the finally arm is the trigger-gated fail-loud
    // (D-CSUBSET-SEH-FINALLY — no shipped consumer).
    RuleId        sehExceptArmRule{};  std::string sehExceptArmRuleName;
    RuleId        sehFinallyArmRule{}; std::string sehFinallyArmRuleName;

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
    // HR10: a SECOND string opener (e.g. SQL's `N'…'` unicode strings) sharing
    // the same coalesced `stringBodyToken`. Invalid ⇒ language has one string form.
    SchemaTokenId unicodeStringStartToken{}; std::string unicodeStringStartTokenName;
    // HR10: string bodies use doubled-delimiter escaping (`''`→`'`, SQL) rather
    // than C-family backslash escapes. Selects the decoder in lowerStringLiteral.
    // `stringDelimiter` is the doubled byte (the string's close delimiter), which
    // the loader derives from the opener's StringStyle so the engine needn't.
    bool stringDoubledDelimiter = false;
    char stringDelimiter = '\0';

    // HR10 — extension kinds + flat-expression + NULL literal (SQL et al.):
    // The extension kinds to register before lowering (so a rule mapped to one
    // builds a HirKind::Extension node).
    std::vector<ExtensionKindEntry> extensionKinds;
    // A FLAT binary-expression rule `operand (binaryOpRule operand)*` (SQL's
    // `expression`), as opposed to the Pratt wrappers. When the engine meets this
    // rule it left-folds it into nested core BinaryOp nodes. `flatBinaryOpRule`
    // wraps each operator token; `binaryOps` maps the token → HIR op (shared).
    RuleId      flatExprRule{};       std::string flatExprRuleName;
    RuleId      flatBinaryOpRule{};   std::string flatBinaryOpRuleName;
    // A typeless literal token (SQL `NULL`) → a leaf Extension node of this kind.
    SchemaTokenId nullToken{};        std::string nullTokenName;
    std::string   nullExtensionKind;  // e.g. "TSQL::Null"

    // HR10: when set, a name reference (the `ref` lower verb, and the bare-name
    // operand of a flat expression) lowers to a leaf Extension node of THIS kind
    // rather than a core `Ref`. SQL relational names — table names and column
    // references — are NOT typed value reads (tables have no value-type; columns
    // bind relationally, and this phase deliberately does not resolve them), so a
    // core `Ref` (which the verifier requires to be typed) is the wrong vehicle.
    // The name stays recoverable via source provenance, exactly as for an
    // unresolved core Ref. Empty ⇒ refs lower to core `Ref` (typed languages).
    std::string   refExtensionKind;   // e.g. "TSQL::Name"

    // D5.3 brace-init + designated-initializer + compound-literal
    // vocabulary. Generic across record-bearing languages; absent ⇒
    // language has no brace-init expression form. The engine's
    // `lowerBraceInit` consumes designators (field-name + index forms,
    // including dot-chained), normalizes to positional with zero-fills
    // against the context type's interner-known field list, and emits
    // a core `HirKind::ConstructAggregate`. Element types are coerced
    // to their slot type via the shared `coerce` helper (the same path
    // ordinary VarDecl init / coerce uses). Compound literal `(T){...}`
    // lowers via the same `lowerBraceInit` with the type taken from
    // the parenthesized prefix — declared via a separate operand-alt
    // rule (`compoundLiteralRule`) so its lookup is positional and the
    // engine never branches on language.
    RuleId      braceInitListRule{};   std::string braceInitListRuleName;
    RuleId      initElementRule{};     std::string initElementRuleName;
    RuleId      designatedFieldRule{}; std::string designatedFieldRuleName;
    RuleId      designatedIndexRule{}; std::string designatedIndexRuleName;
    RuleId      compoundLiteralRule{}; std::string compoundLiteralRuleName;

    // FC2: explicit cast `(T)expr` — the operand-alt rule whose subtree
    // lowers to a core `HirKind::Cast` (explicit flags, NOT Synthetic).
    // The target type is read from the semantic phase's per-node stamp
    // below the type-ref child (the same stamped-type probe the compound
    // literal uses); the operand child lowers as an ordinary expression.
    // Invalid ⇒ the language has no explicit-cast surface.
    RuleId      castRule{};            std::string castRuleName;

    // FC6: `sizeof ( type-name )` | `sizeof unary-expression` → core
    // `HirKind::SizeOf`. The rule is the speculative `sizeofExpr` alt; the
    // operand (a castTypeRef for the type form, a castOperand expr for the value
    // form) carries the semantic-stamped type being sized — the SizeOf node folds
    // to that type's byte size via the `type_layout` engine. Invalid ⇒ the
    // language has no `sizeof` surface.
    RuleId      sizeofRule{};          std::string sizeofRuleName;

    // FC12a-core: the three variadic-intrinsic operand-alt rules. Each routes its
    // CST subtree to its dedicated lowering (`HirKind::VaStart`/`VaArg`/`VaEnd`) —
    // the type child of `va_arg` is recovered from the semantic stamp, NEVER lowered
    // as an expression (the SizeOf precedent). Invalid ⇒ the language has no
    // variadic-intrinsic surface (these stay unset and the dispatch skips them).
    RuleId      vaStartRule{};         std::string vaStartRuleName;
    RuleId      vaArgRule{};           std::string vaArgRuleName;
    RuleId      vaEndRule{};           std::string vaEndRuleName;

    // D-CSUBSET-COMPUTED-GOTO: the GNU `&&label` label-address operand rule
    // (`labelAddressExpr`). A dedicated operand alt (the SizeOf precedent) — its
    // `Identifier` child is a RAW label name, recovered by the CST→HIR lowering
    // (`HirKind::LabelAddressOf`, result `void*`) and resolved to the label's
    // per-function ordinal, NEVER lowered as an expression. Invalid ⇒ the language
    // has no computed-goto surface (unset; the dispatch skips it).
    RuleId      labelAddressRule{};    std::string labelAddressRuleName;

    // Per-language MIR-globals const-evaluation policy. The shared
    // const-eval engine (plan 12.5) supports a float-folding gate via
    // its `allowFloat` knob; today every v1 schema is IEEE 754 so the
    // engine ran with `allowFloat=true` unconditionally inside
    // MIR-globals. Plan 12.5 §0.2 D3 closed: the policy is now per-
    // schema (JSON-declared, NOT C++-coded — config-driven), so a
    // future schema with non-IEEE float semantics (decimal float,
    // fixed-point, saturating arithmetic) declares `allowFloat: false`
    // and the engine refuses to fold its float arithmetic at module-
    // load time. The MIR-globals caller reads this struct off the
    // owning schema and threads it into `EvalOptions`.
    struct GlobalsConstEval {
        bool allowFloat = true;
    };
    GlobalsConstEval globalsConstEval{};

    // True when the block carries no facets — the engine then does nothing.
    [[nodiscard]] bool empty() const noexcept {
        return ruleMappings.empty() && binaryOps.empty() && unaryOps.empty()
            && postfixOps.empty() && extensionKinds.empty();
    }
};

} // namespace dss
