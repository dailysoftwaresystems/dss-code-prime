#include "hir/lowering/cst_to_hir.hpp"

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/char_decode.hpp"
#include "core/types/hir_lowering_config.hpp"
#include "core/types/number_decode.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"             // isEmptySpace
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "hir/hir_op.hpp"
#include "hir/hir_verifier.hpp"

#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <memory>
#include <unordered_map>
#include <vector>

// The single language-agnostic CST→HIR engine (plan 09 HR8). Reads the schema's
// `hirLowering` + `semantics` config and lowers each tree's CST to HIR via the
// HirBuilder, inferring each expression node's result type as it goes (the
// semantic phase types literals/refs/calls but not operator result nodes — per
// plan §2.4 lowering populates typeId per node). Never branches on schema.name().

namespace dss {

namespace {

// Core operator name → HirOpKind (reverse of opName()); std::nullopt if not a
// core op. Used to resolve the config's `target` strings.
[[nodiscard]] std::optional<HirOpKind> coreOpFromName(std::string_view s) {
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(HirOpKind::Count_); ++i) {
        auto const op = static_cast<HirOpKind>(i);
        if (opName(op) == s) return op;
    }
    return std::nullopt;
}

[[nodiscard]] bool isComparison(HirOpKind op) noexcept {
    switch (op) {
        case HirOpKind::Eq: case HirOpKind::Ne: case HirOpKind::Lt:
        case HirOpKind::Le: case HirOpKind::Gt: case HirOpKind::Ge: return true;
        default: return false;
    }
}

// decodeInteger lives in core/types/number_decode.hpp — shared with the
// semantic phase so a literal's text is interpreted identically everywhere.

[[nodiscard]] double decodeFloat(std::string_view text, NumberStyle const* ns, bool& ok) {
    std::string s;
    s.reserve(text.size());
    char const sep = (ns && ns->digitSeparator) ? *ns->digitSeparator : '\0';
    for (char c : text) {
        if (sep != '\0' && c == sep) continue;
        if (c == 'f' || c == 'F') continue;  // float suffix
        s += c;
    }
    errno = 0;
    char* end = nullptr;
    double const d = std::strtod(s.c_str(), &end);
    ok = (end != s.c_str()) && errno != ERANGE;   // parsed something, in range
    return d;
}

[[nodiscard]] bool isSignedCore(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::U8: case TypeKind::U16: case TypeKind::U32:
        case TypeKind::U64: case TypeKind::U128: return false;
        default: return true;
    }
}
[[nodiscard]] bool isFloatCore(TypeKind k) noexcept {
    return k == TypeKind::F16 || k == TypeKind::F32 || k == TypeKind::F64 || k == TypeKind::F128;
}

// HR11: one Lowerer is a single-LANGUAGE lowering context bound to one schema's
// `cfg`/`sem`/`numberStyle` + its schema-specific rule/token index maps. A
// multi-language CU runs one Lowerer per distinct schema, all sharing the one
// `builder` / `literals` / `spans` (and thus one HIR module + arena + kind
// registry + literal pool + source map), so the whole CU lowers to ONE module.
// A homogeneous CU is the one-Lowerer case. The single-language body below never
// changed for HR11 — only the shared output moved out of the struct.
struct Lowerer {
    SemanticModel&           model;
    HirLoweringConfig const& cfg;
    SemanticConfig const&    sem;
    NumberStyle const*       numberStyle;
    TypeInterner&            interner;
    DiagnosticReporter&      reporter;
    HirBuilder&              builder;    // shared across all per-schema Lowerers
    HirLiteralPool&          literals;   // shared
    Tree const*              t_ = nullptr;

    // pendingSpans (shared): applied to the result's HirSourceMap after finish().
    std::vector<std::pair<HirNodeId, HirSourceLoc>>& spans;

    // O(1) lookups.
    std::unordered_map<std::uint32_t, std::size_t> ruleMap_;     // RuleId.v → ruleMappings idx
    std::unordered_map<std::uint32_t, std::size_t> declMap_;     // RuleId.v → sem.declarations idx
    std::unordered_map<std::uint32_t, std::size_t> binOp_, unOp_, postOp_;  // SchemaTokenId.v → idx
    std::unordered_map<std::uint32_t, TypeId>      litType_;     // SchemaTokenId.v → core TypeId
    std::unordered_map<std::uint32_t, TypeKind>    litCore_;     // SchemaTokenId.v → TypeKind
    std::unordered_map<std::uint32_t, bool>        deferred_;    // RuleId.v of explicitly-deferred rules

    // The result of lowering an expression: the HIR node + its resolved type.
    struct E { HirNodeId id; TypeId type; };

    Lowerer(SemanticModel& m, HirLoweringConfig const& c, SemanticConfig const& s,
            NumberStyle const* ns, DiagnosticReporter& r, HirBuilder& b,
            HirLiteralPool& lits, std::vector<std::pair<HirNodeId, HirSourceLoc>>& sp)
        : model(m), cfg(c), sem(s), numberStyle(ns), interner(m.lattice().interner()),
          reporter(r), builder(b), literals(lits), spans(sp) {
        for (std::size_t i = 0; i < cfg.ruleMappings.size(); ++i)
            ruleMap_.emplace(cfg.ruleMappings[i].rule.v, i);
        for (std::size_t i = 0; i < sem.declarations.size(); ++i)
            declMap_.emplace(sem.declarations[i].rule.v, i);
        for (std::size_t i = 0; i < cfg.binaryOps.size(); ++i)
            binOp_.emplace(cfg.binaryOps[i].token.v, i);
        for (std::size_t i = 0; i < cfg.unaryOps.size(); ++i)
            unOp_.emplace(cfg.unaryOps[i].token.v, i);
        for (std::size_t i = 0; i < cfg.postfixOps.size(); ++i)
            postOp_.emplace(cfg.postfixOps[i].token.v, i);
        for (auto const& lt : sem.literalTypes) {
            litType_.emplace(lt.literal.v, interner.primitive(lt.core));
            litCore_.emplace(lt.literal.v, lt.core);
        }
        for (RuleId r : cfg.deferredRules) deferred_.emplace(r.v, true);
        // HR10: register every declared extension kind up front, so a rule mapped
        // to one (or a NULL literal) lowers to a HirKind::Extension carrying its id.
        for (auto const& e : cfg.extensionKinds)
            extKindByName_.emplace(e.name, builder.registry().registerExtension(e.name, e.lang));
        for (std::size_t i = 0; i < sem.callRules.size(); ++i)
            callMap_.emplace(sem.callRules[i].rule.v, i);
        for (auto const& rr : sem.references) refRule_.emplace(rr.rule.v, true);
    }

    std::unordered_map<std::string, HirKindId> extKindByName_;  // HR10 extension kinds
    std::unordered_map<std::uint32_t, std::size_t> callMap_;    // RuleId.v → sem.callRules idx
    std::unordered_map<std::uint32_t, bool>        refRule_;     // RuleId.v of reference rules

    // The registered HirKindId for an extension-kind name. Every name reaching
    // here is declared in cfg.extensionKinds: ruleMapping/nested-ext callers are
    // gated by `extKindByName_.count`, and the loader validates `nullExtensionKind`
    // / `refExtensionKind` against `extensionKinds`. A miss is therefore an
    // internal invariant violation, not user-config drift — report it loud (never
    // silently mint a phantom kind) and still register so the node stays
    // well-formed for the rest of the pass.
    [[nodiscard]] HirKindId extKind(std::string const& name) {
        auto it = extKindByName_.find(name);
        if (it != extKindByName_.end()) return it->second;
        unsupported(NodeId{}, std::format("internal: extension kind '{}' was not "
                                          "declared in hirLowering.extensionKinds", name));
        HirKindId id = builder.registry().registerExtension(name, std::string{model.unit().schema().name()});
        extKindByName_.emplace(name, id);
        return id;
    }

    // True iff the subtree rooted at `node` contains a node whose rule is
    // explicitly deferred (e.g. an array declarator). Bounded.
    [[nodiscard]] bool subtreeHasDeferred(NodeId node) const {
        if (deferred_.empty()) return false;
        std::vector<NodeId> stack{node};
        for (int guard = 0; !stack.empty() && guard < 8192; ++guard) {
            NodeId c = stack.back();
            stack.pop_back();
            if (tree().kind(c) == NodeKind::Internal && deferred_.count(tree().rule(c).v) != 0)
                return true;
            for (NodeId g : visible(c)) stack.push_back(g);
        }
        return false;
    }

    [[nodiscard]] Tree const& tree() const { return *t_; }

    // ── small helpers ─────────────────────────────────────────────────────────
    [[nodiscard]] std::vector<NodeId> visible(NodeId parent) const {
        std::vector<NodeId> out;
        for (NodeId c : tree().children(parent))
            if (!isEmptySpace(tree().flags(c))) out.push_back(c);
        return out;
    }
    [[nodiscard]] bool isToken(NodeId n) const { return tree().kind(n) == NodeKind::Token; }
    [[nodiscard]] bool isExprNode(NodeId n) const {
        if (tree().kind(n) != NodeKind::Internal) return false;
        std::uint32_t const r = tree().rule(n).v;
        return r == cfg.binaryExprRule.v || r == cfg.unaryExprRule.v
            || r == cfg.postfixExprRule.v || r == cfg.operandRule.v
            || (cfg.ternaryExprRule.valid() && r == cfg.ternaryExprRule.v);
    }
    [[nodiscard]] TypeId boolType() { return interner.primitive(TypeKind::Bool); }
    [[nodiscard]] TypeId typeAtOr(NodeId n, TypeId fallback) const {
        TypeId t = model.typeAt(n);
        return t.valid() ? t : fallback;
    }

    HirNodeId track(HirNodeId id, NodeId cst) {
        if (cst.valid())
            spans.push_back({id, HirSourceLoc{tree().source().id(), tree().span(cst)}});
        return id;
    }

    void unsupported(NodeId node, std::string detail) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::H_UnsupportedLoweringForKind;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = tree().source().id();
        d.span     = node.valid() ? tree().span(node) : SourceSpan::empty(0);
        d.actual   = std::move(detail);
        reporter.report(std::move(d));
    }
    HirNodeId errorNode(NodeId cst, TypeId type = InvalidType) {
        return track(builder.addLeaf(HirKind::Error, type, 0, HirFlags::HasError), cst);
    }
    // Report + an Error sentinel (every Error node is paired with a diagnostic).
    HirNodeId reportedError(NodeId cst, std::string detail) {
        unsupported(cst, std::move(detail));
        return errorNode(cst);
    }
    E exprError(NodeId cst, std::string detail) { return {reportedError(cst, std::move(detail)), InvalidType}; }
    // An optional child, or a reported Error sentinel when absent (only builds the
    // Error — and only reports — when the optional is empty; avoids the eager-eval
    // trap of `opt.value_or(errorNode(...))`).
    HirNodeId orError(std::optional<HirNodeId> v, NodeId cst, std::string detail) {
        return v ? *v : reportedError(cst, std::move(detail));
    }

    [[nodiscard]] HirRuleMapping const* mappingFor(NodeId n) const {
        if (tree().kind(n) != NodeKind::Internal) return nullptr;
        auto it = ruleMap_.find(tree().rule(n).v);
        return it == ruleMap_.end() ? nullptr : &cfg.ruleMappings[it->second];
    }

    // Alt rules (`statement`, `topLevel`, `expression`, `switchBodyItem`, …)
    // materialize as wrapper nodes with a single meaningful child. Peel them
    // until reaching a node the engine recognizes — an expression form, a
    // mapped statement/decl rule, or the case-label rule — so callers can
    // classify a child by ROLE without knowing the grammar's wrapper shapes.
    [[nodiscard]] NodeId peelToCore(NodeId n) const {
        NodeId cur = n;
        for (int guard = 0; guard < 128 && cur.valid(); ++guard) {
            if (tree().kind(cur) != NodeKind::Internal) return cur;  // token
            std::uint32_t const rv = tree().rule(cur).v;
            if (isExprNode(cur)) return cur;
            if (ruleMap_.count(rv) != 0) return cur;
            if (cfg.caseLabelRule.valid() && rv == cfg.caseLabelRule.v) return cur;
            NodeId only = soleMeaningfulChild(cur);
            if (!only.valid()) return cur;
            cur = only;
        }
        return cur;
    }

    enum class Role { Expr, Stmt, Other };
    [[nodiscard]] Role classify(NodeId n) const {
        NodeId core = peelToCore(n);
        if (core.valid() && tree().kind(core) == NodeKind::Internal) {
            if (isExprNode(core)) return Role::Expr;
            if (ruleMap_.count(tree().rule(core).v) != 0) return Role::Stmt;
        }
        return Role::Other;
    }

    // ── expressions ───────────────────────────────────────────────────────────
    E lowerExpr(NodeId node) {
        if (tree().kind(node) == NodeKind::Internal) {
            std::uint32_t const r = tree().rule(node).v;
            if (cfg.flatExprRule.valid() && r == cfg.flatExprRule.v)
                return lowerFlatExpr(node);   // HR10: SQL-style flat expression
            if (r == cfg.operandRule.v)     return lowerOperand(node);
            if (r == cfg.binaryExprRule.v)  return lowerBinary(node);
            if (r == cfg.unaryExprRule.v)   return lowerUnary(node);
            if (r == cfg.postfixExprRule.v) return lowerPostfix(node);
            if (cfg.ternaryExprRule.valid() && r == cfg.ternaryExprRule.v)
                return lowerTernary(node);
            // Unknown wrapper (e.g. an `expression` node): descend through a
            // single meaningful child.
            NodeId only = soleMeaningfulChild(node);
            if (only.valid()) return lowerExpr(only);
        }
        unsupported(node, "expression form has no hirLowering mapping");
        return {errorNode(node), InvalidType};
    }

    // The single non-token child, or invalid if there isn't exactly one.
    [[nodiscard]] NodeId soleMeaningfulChild(NodeId node) const {
        NodeId found{};
        for (NodeId c : visible(node)) {
            if (isToken(c)) continue;
            if (found.valid()) return {};
            found = c;
        }
        return found;
    }

    // Detect and lower a value-bearing LEAF literal directly under `node`: the
    // char / string / unicode-string forms materialize as a `[startToken,
    // COALESCED body]` subtree, and NULL as a single child token — none of which
    // is an expression node, so both operand lowerers probe for them before the
    // generic internal-child descent. std::nullopt ⇒ not such a leaf literal.
    [[nodiscard]] std::optional<E> tryLowerLeafLiteral(NodeId node) {
        if (NodeId chl = bodyLiteralNodeOf(node, cfg.charStartToken); chl.valid())
            return lowerCharLiteral(chl);
        if (NodeId sl = bodyLiteralNodeOf(node, cfg.stringStartToken); sl.valid())
            return lowerStringLiteral(sl);
        if (NodeId us = bodyLiteralNodeOf(node, cfg.unicodeStringStartToken); us.valid())
            return lowerStringLiteral(us);   // SQL `N'…'` — same body token + decoder
        if (cfg.nullToken.valid())            // SQL NULL → a typeless extension leaf
            if (NodeId nt = childTokenOfKind(node, cfg.nullToken); nt.valid())
                return lowerNullLiteral(node);
        return std::nullopt;
    }

    E lowerOperand(NodeId node) {
        // operand = Identifier | <literal token> | <char/string literal> | ( expression )
        if (auto lit = tryLowerLeafLiteral(node)) return *lit;
        for (NodeId c : visible(node)) {
            if (tree().kind(c) == NodeKind::Internal) return lowerExpr(c);  // paren-wrapped
        }
        for (NodeId c : visible(node)) {
            if (!isToken(c)) continue;
            SchemaTokenId const tk = tree().tokenKind(c);
            if (sem.identifierToken.valid() && tk.v == sem.identifierToken.v) {
                TypeId const type = typeAtOr(node, InvalidType);
                SymbolId const sym = model.symbolAt(c);
                return {track(builder.makeRef(type, sym.v), node), type};
            }
            auto lit = litType_.find(tk.v);
            if (lit != litType_.end()) return lowerLiteral(node, c, tk, lit->second);
        }
        unsupported(node, "operand has no Identifier / literal / parenthesized child");
        return {errorNode(node), InvalidType};
    }

    // The child rule node of `operand` whose first visible child is `startTok`
    // (a `charLiteralExpr` / `stringLiteralExpr` subtree), or invalid.
    [[nodiscard]] NodeId bodyLiteralNodeOf(NodeId operand, SchemaTokenId startTok) {
        if (!startTok.valid()) return {};
        for (NodeId c : visible(operand)) {
            if (tree().kind(c) != NodeKind::Internal) continue;
            for (NodeId g : visible(c)) {
                if (isToken(g) && tree().tokenKind(g).v == startTok.v) return c;
                break;  // only the FIRST visible child decides
            }
        }
        return {};
    }

    // The first visible child token of `node` whose kind is `k`, or invalid.
    [[nodiscard]] NodeId childTokenOfKind(NodeId node, SchemaTokenId k) {
        for (NodeId c : visible(node))
            if (isToken(c) && tree().tokenKind(c).v == k.v) return c;
        return {};
    }

    // `'a'` / `'\n'` → a Char-typed literal carrying the decoded codepoint.
    E lowerCharLiteral(NodeId node) {
        TypeId const type = interner.primitive(TypeKind::Char);
        NodeId const bodyTok = childTokenOfKind(node, cfg.charBodyToken);
        std::string_view const body = bodyTok.valid() ? tree().text(bodyTok) : std::string_view{};
        auto cp = decodeCharLiteralBody(body);
        if (!cp) {
            unsupported(node, std::format("char literal '{}' is empty, multi-character, "
                                          "or has an unsupported escape", body));
            return {errorNode(node, type), type};
        }
        HirLiteralValue v;
        v.core  = TypeKind::Char;
        v.value = static_cast<std::uint64_t>(*cp);
        return {track(builder.makeLiteral(type, literals.add(std::move(v))), node), type};
    }

    // `"hello"` → an Array<Char, N+1> literal carrying the decoded bytes (NUL
    // implied by the +1 length).
    E lowerStringLiteral(NodeId node) {
        NodeId const bodyTok = childTokenOfKind(node, cfg.stringBodyToken);
        std::string_view const body = bodyTok.valid() ? tree().text(bodyTok) : std::string_view{};
        std::string bytes;
        if (cfg.stringDoubledDelimiter) {
            // SQL `'…''…'`: doubled-delimiter escaping (never fails — pairs only).
            bytes = decodeDoubledDelimiterBody(body, cfg.stringDelimiter);
        } else if (auto decoded = decodeStringLiteralBody(body)) {  // C-family backslash escapes
            bytes = std::move(*decoded);
        } else {
            unsupported(node, std::format("string literal \"{}\" has an unsupported escape", body));
            return {errorNode(node), InvalidType};
        }
        TypeId const type = interner.array(interner.primitive(TypeKind::Char),
                                           static_cast<std::int64_t>(bytes.size() + 1));
        HirLiteralValue v;
        v.core  = TypeKind::Char;
        v.value = std::move(bytes);
        return {track(builder.makeLiteral(type, literals.add(std::move(v))), node), type};
    }

    // SQL `NULL` (typeless) → a leaf Extension node of the configured kind.
    E lowerNullLiteral(NodeId node) {
        HirKindId const kid = extKind(cfg.nullExtensionKind);
        return {track(builder.addLeaf(HirKind::Extension, InvalidType, kid.v), node), InvalidType};
    }

    // ── HR10: flat expression + SQL operand lowering ───────────────────────────
    [[nodiscard]] bool isNameToken(NodeId n) const {
        if (!isToken(n)) return false;
        SchemaTokenId const tk = tree().tokenKind(n);
        return (sem.identifierToken.valid() && tk.v == sem.identifierToken.v)
            || (sem.bracketIdentifierToken && sem.bracketIdentifierToken->valid()
                && tk.v == sem.bracketIdentifierToken->v);
    }

    // A flat `operand (binaryOpRule operand)*` sequence (SQL's `expression`),
    // left-folded into nested core BinaryOp nodes. Distinct from the Pratt path.
    E lowerFlatExpr(NodeId node) {
        std::vector<NodeId> operands;
        std::vector<NodeId> opToks;
        for (NodeId c : visible(node)) {
            if (tree().kind(c) != NodeKind::Internal) continue;
            if (cfg.flatBinaryOpRule.valid() && tree().rule(c).v == cfg.flatBinaryOpRule.v) {
                for (NodeId t : visible(c)) if (isToken(t)) { opToks.push_back(t); break; }
            } else {
                operands.push_back(c);
            }
        }
        if (operands.empty()) return exprError(node, "flat expression has no operand");
        // A well-formed `operand (op operand)*` has exactly one fewer operator
        // than operands. A mismatch is a malformed parse — fail loud rather than
        // silently fold a truncated expression.
        if (opToks.size() + 1 != operands.size())
            return exprError(node, std::format("malformed flat expression: {} operands "
                                               "but {} operators", operands.size(), opToks.size()));
        E acc = lowerFlatOperand(operands[0]);
        for (std::size_t i = 0; i + 1 < operands.size() && i < opToks.size(); ++i) {
            auto it = binOp_.find(tree().tokenKind(opToks[i]).v);
            if (it == binOp_.end()) {
                unsupported(opToks[i], std::format("binary operator '{}' has no hirLowering mapping",
                                                   tree().text(opToks[i])));
                return {errorNode(node), InvalidType};
            }
            acc = combineBinary(opToks[i], cfg.binaryOps[it->second], acc, lowerFlatOperand(operands[i + 1]));
        }
        return acc;
    }

    // One operand of a flat SQL expression: literal / string / NULL / unary-minus
    // / call / name-reference / parenthesized sub-expression.
    E lowerFlatOperand(NodeId node) {
        if (auto lit = tryLowerLeafLiteral(node)) return *lit;
        // Unary prefix (SQL `-operand`): a unary-op token + an inner operand.
        {
            NodeId negTok{}, inner{};
            for (NodeId c : visible(node)) {
                if (isToken(c)) { if (unOp_.count(tree().tokenKind(c).v)) negTok = c; }
                else if (!inner.valid()) inner = c;
            }
            if (negTok.valid() && inner.valid()) {
                auto it = unOp_.find(tree().tokenKind(negTok).v);
                auto op = coreOpFromName(cfg.unaryOps[it->second].target);
                E e = lowerFlatOperand(inner);
                if (!op || arityOf(*op) != HirOpArity::Unary)
                    return exprError(node, "unary operand has no core unary op");
                return {track(builder.addParent(HirKind::UnaryOp, std::array{e.id}, e.type,
                                                encodeOp(*op)), node), e.type};
            }
        }
        for (NodeId c : visible(node)) {
            if (tree().kind(c) != NodeKind::Internal) continue;
            if (cfg.flatExprRule.valid() && tree().rule(c).v == cfg.flatExprRule.v)
                return lowerFlatExpr(c);                       // parenthesized expression
            // SQL call `f(args)`: the operand wraps a call rule (e.g. nameOrCall →
            // callExpr). `peelToCore` would descend PAST the call into its sole
            // non-token child (the argument list), so detect the call rule
            // explicitly in the subtree before the name-reference fallback.
            if (NodeId call = findCallNode(c); call.valid()) return lowerSqlCall(call);
            // A name reference: `c` peels directly to a name token, or wraps one
            // (qualifiedName → nameAtom → Identifier — `peelToCore` stops at the
            // nameAtom wrapper, so probe the subtree for the name token).
            if (firstNameToken(c).node.valid()) return nameRefExpr(c);
            return lowerExpr(c);                                // defensive fallback
        }
        for (NodeId c : visible(node)) {                        // direct literal token (IntLiteral)
            if (!isToken(c)) continue;
            SchemaTokenId const tk = tree().tokenKind(c);
            auto lit = litType_.find(tk.v);
            if (lit != litType_.end()) return lowerLiteral(node, c, tk, lit->second);
        }
        return exprError(node, "SQL operand has no recognizable form");
    }

    // SQL `f(args)` → a core Call (callee Ref + lowered argument expressions),
    // reusing the semantics `callRules` callee/args child positions.
    E lowerSqlCall(NodeId callNode) {
        auto it = callMap_.find(tree().rule(callNode).v);
        if (it == callMap_.end()) return exprError(callNode, "call rule has no semantics entry");
        CallRule const& cr = sem.callRules[it->second];
        auto vis = visible(callNode);
        HirNodeId callee{};
        TypeId calleeTy = InvalidType;
        if (cr.calleeChild < vis.size()) {
            NodeId calleeNode = vis[cr.calleeChild];
            NodeId tok = isToken(calleeNode) ? calleeNode : peelToCore(calleeNode);
            SymbolId const sym = isNameToken(tok) ? model.symbolAt(tok) : SymbolId{};
            calleeTy = typeAtOr(calleeNode, InvalidType);
            // The callee is a name. In a relational-name language (refExtensionKind
            // set), a function name is symbolic like any other name — not a typed
            // value read — so it lowers to the name Extension; the Call node itself
            // carries the result type. Otherwise a typed core Ref (C-family).
            callee = makeNameNode(calleeNode, sym, calleeTy);
        } else {
            return exprError(callNode, "call has no callee child");
        }
        std::vector<HirNodeId> args;
        if (cr.argsChild < vis.size()) {
            for (NodeId a : visible(vis[cr.argsChild])) {
                if (isToken(a)) continue;                       // skip commas
                args.push_back(lowerFlatExpr(a).id);
            }
        }
        TypeId const result = (calleeTy.valid() && interner.kind(calleeTy) == TypeKind::FnSig)
                            ? interner.fnResult(calleeTy) : typeAtOr(callNode, InvalidType);
        return {track(builder.makeCall(callee, args, result), callNode), result};
    }

    // ── HR10: generic extension-node lowering ──────────────────────────────────
    // Build a HirKind::Extension node of `m.hirKind`, gathering role children per
    // `m.childGathering`. Entirely config-driven — no language vocabulary here.
    HirNodeId lowerExtensionNode(NodeId cstNode, HirRuleMapping const& m) {
        HirKindId const kid = extKind(m.hirKind);
        std::vector<HirNodeId> children;
        for (ChildSlotSpec const& slot : m.childGathering) {
            if (slot.list) {
                // Lower EACH `matchRule` item of a comma-separated list. Some
                // grammars flatten `item (, item)*` (all items direct children);
                // others nest the tail in a repeat-group node. `collectListItems`
                // handles both by gathering matches in document order, stopping
                // descent at each match (list items don't nest), so it never
                // double-counts. A required (non-optional) list that matched no
                // item is a malformed tree — fail loud rather than emit an empty
                // grouping.
                std::vector<NodeId> items;
                collectListItems(cstNode, slot.matchRule, items);
                if (items.empty() && !slot.optional) {
                    children.push_back(reportedError(cstNode,
                        std::format("extension list slot '{}' matched no items", slot.role)));
                    continue;
                }
                for (NodeId item : items) children.push_back(lowerSlot(item, slot));
                continue;
            }
            NodeId child = slot.classifier == "expr"
                ? findExprChild(cstNode)
                : (slot.matchRule.valid() ? findChildByRule(cstNode, slot.matchRule) : NodeId{});
            if (!child.valid()) {
                if (!slot.optional)
                    children.push_back(reportedError(cstNode,
                        std::format("extension slot '{}' not found", slot.role)));
                continue;
            }
            children.push_back(lowerSlot(child, slot));
        }
        return track(builder.addParent(HirKind::Extension, children, typeAtOr(cstNode, InvalidType),
                                       kid.v), cstNode);
    }

    // Collect, in document order, every descendant of `parent` whose rule is
    // `matchRule`, descending through intervening wrapper nodes (e.g. a grammar's
    // repeat-group) but STOPPING at each match — so a same-rule list item nested
    // inside another (were a grammar to allow it) is not flattened into the outer
    // list. When `matchRule` is unset, gathers the direct internal children.
    void collectListItems(NodeId parent, RuleId matchRule, std::vector<NodeId>& out) {
        for (NodeId c : visible(parent)) {
            if (tree().kind(c) != NodeKind::Internal) continue;
            if (!matchRule.valid()) { out.push_back(c); continue; }
            if (tree().rule(c).v == matchRule.v) { out.push_back(c); continue; }  // matched: don't descend
            collectListItems(c, matchRule, out);   // descend through repeat-group wrappers
        }
    }

    // The outermost call-rule node within `subtree` (a node whose rule is in the
    // semantics `callRules`), or invalid if none. Descent stops at the first
    // match, so a call's own argument subtrees (which may contain nested calls)
    // are not mistaken for the operand's call.
    [[nodiscard]] NodeId findCallNode(NodeId subtree) const {
        std::vector<NodeId> stack{subtree};
        for (int guard = 0; guard < 4096 && !stack.empty(); ++guard) {
            NodeId n = stack.back(); stack.pop_back();
            if (tree().kind(n) == NodeKind::Internal && callMap_.count(tree().rule(n).v))
                return n;                                   // matched: don't descend
            for (NodeId k : visible(n)) stack.push_back(k);
        }
        return {};
    }

    // Lower one located child per the slot's `lower` verb. The loader validated
    // the verb against the closed ChildLower set, so the switch is exhaustive.
    HirNodeId lowerSlot(NodeId child, ChildSlotSpec const& slot) {
        switch (slot.lower) {
            case ChildLower::Expr:     return lowerExpr(child).id;
            case ChildLower::FlatExpr: return lowerFlatExpr(flatExprChildOf(child)).id;
            case ChildLower::Ext:      return lowerNestedExtension(child);
            case ChildLower::VarDecl:  return lowerVarLike(child, /*asGlobal=*/false);
            case ChildLower::Ref:      return nameRefExpr(child).id;
        }
        return reportedError(child, "unhandled childGathering lower verb");
    }

    // The name token inside `subtree` (e.g. a qualifiedName → nameAtom →
    // Identifier) and its resolved symbol. Prefers a symbol-bearing token (the
    // resolved last identifier — what `nameMatch: lastIdentifier` binds); when no
    // name resolves, falls back to the rightmost name token's span (the LIFO walk
    // pushes children in order and pops the deepest/rightmost first), which is the
    // qualified name's last segment — the right span for an unresolved column.
    struct NameHit { NodeId node{}; SymbolId sym{}; };
    [[nodiscard]] NameHit firstNameToken(NodeId subtree) const {
        NameHit hit;
        std::vector<NodeId> stack{subtree};
        for (int guard = 0; guard < 4096 && !stack.empty(); ++guard) {
            NodeId n = stack.back(); stack.pop_back();
            if (isNameToken(n)) {
                if (!hit.node.valid()) hit.node = n;            // rightmost name (for the span)
                if (model.symbolAt(n).valid()) { hit.sym = model.symbolAt(n); hit.node = n; break; }
            }
            for (NodeId k : visible(n)) stack.push_back(k);
        }
        return hit;
    }

    // A name reference as an expression (id + type): resolves the last identifier
    // the semantic phase bound inside `subtree` (e.g. a qualifiedName's last name)
    // and builds the configured node. When `cfg.refExtensionKind` is set the
    // language's names are relational, not typed value reads (SQL table/column
    // names), so this emits a leaf Extension node (no type requirement); otherwise
    // a core typed `Ref`. An unresolved name (sym 0) is fine — its text is
    // recoverable from source provenance (correct for SQL columns, which bind
    // relationally, not lexically). See `makeNameNode`.
    E nameRefExpr(NodeId subtree) {
        NameHit h = firstNameToken(subtree);
        if (!h.node.valid())   // a `ref`-lowered slot that holds no name token: fail loud
            return exprError(subtree, "name reference subtree has no name token");
        TypeId const t = typeAtOr(h.node, InvalidType);
        HirNodeId const node = makeNameNode(h.node, h.sym, t);
        return {node, cfg.refExtensionKind.empty() ? t : InvalidType};
    }

    // Emit a name reference: a leaf Extension of `cfg.refExtensionKind` when the
    // language declares one (SQL relational names — untyped), else a core typed
    // `Ref`. Single seam so every "name reference" site agrees.
    HirNodeId makeNameNode(NodeId at, SymbolId sym, TypeId type) {
        if (!cfg.refExtensionKind.empty())
            return track(builder.addLeaf(HirKind::Extension, InvalidType,
                                         extKind(cfg.refExtensionKind).v), at);
        return track(builder.makeRef(type, sym.v), at);
    }

    // `ext` slot: the child rule must itself be extension-mapped.
    HirNodeId lowerNestedExtension(NodeId child) {
        NodeId core = peelToCore(child);
        HirRuleMapping const* cm = mappingFor(core);
        if (cm && extKindByName_.count(cm->hirKind)) return lowerExtensionNode(core, *cm);
        return reportedError(child, "ext slot's child is not an extension-mapped rule");
    }

    // For a `flatExpr` slot whose match is a clause wrapper (e.g. whereClause =
    // [WHERE, expression]): descend to the flat-expression child. If the matched
    // node IS the flat-expression already, use it directly.
    [[nodiscard]] NodeId flatExprChildOf(NodeId node) {
        if (cfg.flatExprRule.valid() && tree().kind(node) == NodeKind::Internal
            && tree().rule(node).v == cfg.flatExprRule.v)
            return node;
        if (NodeId e = findChildByRule(node, cfg.flatExprRule); e.valid()) return e;
        return node;
    }

    [[nodiscard]] NodeId findChildByRule(NodeId parent, RuleId rule) {
        if (!rule.valid()) return {};
        for (NodeId c : visible(parent)) {
            if (tree().kind(c) == NodeKind::Internal && tree().rule(c).v == rule.v) return c;
        }
        return {};
    }
    [[nodiscard]] NodeId findExprChild(NodeId parent) {
        for (NodeId c : visible(parent))
            if (tree().kind(c) == NodeKind::Internal && isExprNode(peelToCore(c))) return c;
        return {};
    }

    E lowerLiteral(NodeId operandNode, NodeId tokenNode, SchemaTokenId tk, TypeId type) {
        TypeKind const core = litCore_.at(tk.v);
        std::string_view const text = tree().text(tokenNode);
        HirLiteralValue val;          // value defaults to monostate (= undecodable)
        val.core = core;
        bool ok = true;
        if (isFloatCore(core)) {
            double const d = decodeFloat(text, numberStyle, ok);
            if (ok) val.value = d;
        } else if (auto iv = decodeInteger(text, numberStyle)) {
            if (isSignedCore(core)) val.value = static_cast<std::int64_t>(*iv);
            else                    val.value = *iv;
        } else {
            ok = false;             // integer overflow
        }
        if (!ok)
            unsupported(tokenNode, std::format("literal '{}' is out of range / undecodable", text));
        std::uint32_t const idx = literals.add(val);
        return {track(builder.makeLiteral(type, idx), operandNode), type};
    }

    // Combine two lowered operands under an ALREADY-RESOLVED binary-operator entry
    // `e`, anchoring provenance + diagnostics at `anchor`. Shared by the Pratt
    // (`lowerBinary`) and flat (`lowerFlatExpr`) paths so the logical-op special
    // cases and operator-result typing (comparison → Bool, else the left operand's
    // type) stay identical. Does NOT handle `Assign` (an expression-position
    // store) — that is Pratt-only and resolved by the caller before this point.
    E combineBinary(NodeId anchor, HirOperatorEntry const& e, E lhs, E rhs) {
        if (e.target == "LogicalAnd")
            return {track(builder.makeLogicalAnd(lhs.id, rhs.id, boolType()), anchor), boolType()};
        if (e.target == "LogicalOr")
            return {track(builder.makeLogicalOr(lhs.id, rhs.id, boolType()), anchor), boolType()};
        auto op = coreOpFromName(e.target);
        if (!op || arityOf(*op) != HirOpArity::Binary) {
            unsupported(anchor, std::format("binary target '{}' is not a core binary operator", e.target));
            return {errorNode(anchor), InvalidType};
        }
        TypeId const result = isComparison(*op) ? boolType()
                            : (lhs.type.valid() ? lhs.type : rhs.type);
        return {track(builder.addParent(HirKind::BinaryOp, std::array{lhs.id, rhs.id},
                                        result, encodeOp(*op)), anchor), result};
    }

    E lowerBinary(NodeId node) {
        // [lhs, OP-token, rhs]
        NodeId lhsN{}, rhsN{}, opTok{};
        for (NodeId c : visible(node)) {
            if (isToken(c)) { if (!opTok.valid()) opTok = c; continue; }
            if (!lhsN.valid()) lhsN = c; else if (!rhsN.valid()) rhsN = c;
        }
        if (!opTok.valid() || !lhsN.valid() || !rhsN.valid()) {
            unsupported(node, "malformed binary expression");
            return {errorNode(node), InvalidType};
        }
        auto it = binOp_.find(tree().tokenKind(opTok).v);
        if (it == binOp_.end()) {
            unsupported(node, std::format("binary operator '{}' has no hirLowering mapping",
                                          tree().text(opTok)));
            return {errorNode(node), InvalidType};
        }
        HirOperatorEntry const& e = cfg.binaryOps[it->second];
        // Assignment is a STATEMENT in HIR, but C lets it be used as a value
        // (`while ((c = f()) != EOF)`). Lower it as a SeqExpr that performs the
        // store then yields the stored value — the sound, position-independent
        // form (hoisting the store out would be wrong inside a loop condition).
        // Covers compound assignment too (`(x += 1)` reads, applies the op, writes).
        if (e.target == "Assign") {
            // lhsN / rhsN were already extracted by the scan above.
            auto lv = lhsN.valid() ? classifyLvalue(lhsN) : std::nullopt;
            if (!lv || !rhsN.valid())
                return exprError(node, "assignment sub-expression needs an lvalue and a value");
            HirNodeId stored;
            if (e.compoundBase.empty()) {
                stored = lowerExpr(rhsN).id;                        // plain `=`
            } else {
                auto op = coreOpFromName(e.compoundBase);           // `OP=`
                if (!op || arityOf(*op) != HirOpArity::Binary)
                    return exprError(node, std::format("compound base op '{}' is not binary",
                                                       e.compoundBase));
                stored = builder.addParent(HirKind::BinaryOp,
                                           std::array{lvRead(*lv), lowerExpr(rhsN).id},
                                           lv->type, encodeOp(*op));
            }
            std::vector<HirNodeId> stmts = lv->prep;
            stmts.push_back(lvWrite(*lv, stored));
            HirNodeId yield = lvRead(*lv);   // the new value (re-read of the lvalue)
            return {track(builder.makeSeqExpr(stmts, yield, lv->type, HirFlags::Synthetic), node),
                    lv->type};
        }
        return combineBinary(node, e, lowerExpr(lhsN), lowerExpr(rhsN));
    }

    E lowerUnary(NodeId node) {
        // [OP-token, operand]
        NodeId opTok{}, operandN{};
        for (NodeId c : visible(node)) {
            if (isToken(c)) { if (!opTok.valid()) opTok = c; continue; }
            if (!operandN.valid()) operandN = c;
        }
        if (!opTok.valid() || !operandN.valid()) {
            unsupported(node, "malformed unary expression");
            return {errorNode(node), InvalidType};
        }
        auto it = unOp_.find(tree().tokenKind(opTok).v);
        if (it == unOp_.end()) {
            unsupported(node, std::format("unary operator '{}' has no hirLowering mapping",
                                          tree().text(opTok)));
            return {errorNode(node), InvalidType};
        }
        E operand = lowerExpr(operandN);
        HirOperatorEntry const& e = cfg.unaryOps[it->second];
        if (e.target == "AddressOf") {
            TypeId const result = operand.type.valid() ? interner.pointer(operand.type) : InvalidType;
            return {track(builder.makeAddressOf(operand.id, result), node), result};
        }
        if (e.target == "Deref") {
            TypeId result = InvalidType;
            if (operand.type.valid() && interner.kind(operand.type) == TypeKind::Ptr)
                result = interner.operands(operand.type)[0];
            return {track(builder.makeDeref(operand.id, result), node), result};
        }
        auto op = coreOpFromName(e.target);
        if (!op || arityOf(*op) != HirOpArity::Unary) {
            unsupported(node, std::format("unary target '{}' is not a core unary operator", e.target));
            return {errorNode(node), InvalidType};
        }
        TypeId const result = (*op == HirOpKind::Not) ? boolType() : operand.type;
        return {track(builder.addParent(HirKind::UnaryOp, std::array{operand.id},
                                        result, encodeOp(*op)), node), result};
    }

    // `cond ? then : else` → a Ternary node. The wrapper holds
    // [cond, `?`, then, `:`, else]; the three operands are the visible non-token
    // children. Result type is the then-branch's (C requires then/else to be
    // compatible; the semantic phase already checked, and prefers a node type
    // when it set one).
    E lowerTernary(NodeId node) {
        std::vector<NodeId> operands;
        for (NodeId c : visible(node)) if (!isToken(c)) operands.push_back(c);
        if (operands.size() != 3) {
            unsupported(node, "malformed ternary expression (expected cond, then, else)");
            return {errorNode(node), InvalidType};
        }
        E cond = lowerExpr(operands[0]);
        E thenE = lowerExpr(operands[1]);
        E elseE = lowerExpr(operands[2]);
        TypeId const result = typeAtOr(node, thenE.type.valid() ? thenE.type : elseE.type);
        return {track(builder.makeTernary(cond.id, thenE.id, elseE.id, result), node), result};
    }

    E lowerPostfix(NodeId node) {
        // [base, OP-token, body...]   (Call: body=argList; Index: body=expression)
        NodeId baseN{}, opTok{};
        std::vector<NodeId> rest;
        for (NodeId c : visible(node)) {
            if (!baseN.valid() && !isToken(c)) { baseN = c; continue; }
            if (isToken(c)) { if (!opTok.valid()) opTok = c; continue; }
            rest.push_back(c);
        }
        if (!baseN.valid() || !opTok.valid()) {
            unsupported(node, "malformed postfix expression");
            return {errorNode(node), InvalidType};
        }
        auto it = postOp_.find(tree().tokenKind(opTok).v);
        if (it == postOp_.end()) {
            unsupported(node, std::format("postfix operator '{}' has no hirLowering mapping "
                                          "(++/-- deferred to HR9)", tree().text(opTok)));
            return {errorNode(node), InvalidType};
        }
        HirOperatorEntry const& e = cfg.postfixOps[it->second];
        // Value-yielding `x++` / `x--` (postfix yields the OLD value): save it in
        // a temp, mutate the lvalue, yield the temp — all in a SeqExpr. Handled
        // before lowering `base` (classifyLvalue lowers the lvalue itself).
        if (e.target == "PostInc" || e.target == "PostDec") {
            auto lv = classifyLvalue(baseN);
            if (!lv) return exprError(node, "++/-- needs an lvalue operand");
            SymbolId const tmp = freshSymbol();
            std::vector<HirNodeId> stmts = lv->prep;
            stmts.push_back(builder.makeVarDecl(lv->type, tmp.v, lvRead(*lv), HirFlags::Synthetic));
            HirNodeId one = synthOne(lv->type);
            HirNodeId inc = builder.addParent(HirKind::BinaryOp, std::array{lvRead(*lv), one}, lv->type,
                                              encodeOp(e.target == "PostInc" ? HirOpKind::Add
                                                                             : HirOpKind::Sub));
            stmts.push_back(lvWrite(*lv, inc));
            HirNodeId yield = builder.makeRef(lv->type, tmp.v);
            return {track(builder.makeSeqExpr(stmts, yield, lv->type, HirFlags::Synthetic), node),
                    lv->type};
        }
        E base = lowerExpr(baseN);
        if (e.target == "Call") {
            std::vector<HirNodeId> args;
            for (NodeId argN : argExpressions(rest)) args.push_back(lowerExpr(argN).id);
            // Prefer the semantic phase's resolved call type (it types call nodes);
            // fall back to the callee FnSig's result.
            TypeId inferred = InvalidType;
            if (base.type.valid() && interner.kind(base.type) == TypeKind::FnSig)
                inferred = interner.fnResult(base.type);
            TypeId const result = typeAtOr(node, inferred);
            return {track(builder.makeCall(base.id, args, result), node), result};
        }
        if (e.target == "Index") {
            HirNodeId idx = rest.empty() ? reportedError(node, "index has no subscript expression")
                                         : lowerExpr(rest.front()).id;
            TypeId inferred = InvalidType;
            if (base.type.valid()) {
                TypeKind const bk = interner.kind(base.type);
                if ((bk == TypeKind::Array || bk == TypeKind::Ptr || bk == TypeKind::Slice)
                    && !interner.operands(base.type).empty())
                    inferred = interner.operands(base.type)[0];
            }
            TypeId const result = typeAtOr(node, inferred);
            return {track(builder.makeIndex(base.id, idx, result), node), result};
        }
        // D5.1: `obj.field` and `ptr->field`. The semantic phase (Pass 2)
        // already resolved the field's SymbolId (via the `memberAccesses`
        // facet) and propagated its type to both the field-name leaf and
        // the postfixExpr node. We read fieldIndex off the field's
        // SymbolRecord (Pass 1 stamped it as the field's declaration-order
        // ordinal in its struct scope). The arrow form is desugared at HIR
        // level: `p->x` = `MemberAccess(Deref(p), idx)` — one HIR kind
        // handles both forms, downstream MIR sees uniform GEP-after-load
        // patterns.
        if (e.target == "MemberAccess" || e.target == "MemberAccessThruPtr") {
            // Locate the field-name token inside the follower subtree
            // (c-subset's `memberFollower = {sequence: [Identifier]}`). Robust
            // against a future schema that wraps the name (e.g. bracketed
            // identifiers): scan for a real token first, fall back to the
            // first visible child if the follower is all Internal.
            NodeId followerN = rest.empty() ? NodeId{} : rest.front();
            NodeId fieldNameN{};
            if (followerN.valid()) {
                for (NodeId c : visible(followerN)) {
                    if (isToken(c)) { fieldNameN = c; break; }
                    if (!fieldNameN.valid()) fieldNameN = c;
                }
            }
            if (!fieldNameN.valid()) {
                return exprError(node, "member access has no field-name leaf");
            }
            SymbolId const fieldSym = model.symbolAt(fieldNameN);
            if (!fieldSym.valid()) {
                return exprError(node, "member access field did not resolve "
                                       "to a symbol (semantic phase miss)");
            }
            auto const* frec = model.recordFor(fieldSym);
            if (frec == nullptr) {
                return exprError(node, "member access field SymbolId has no record");
            }
            // Defensive: the resolved symbol must be a field of a composite
            // type. Pass 2's member-access path always binds to a field
            // (struct-scope lookup), but a future Pass-2 recovery path that
            // falls back to enclosing-scope lookup could mis-bind to a
            // non-field symbol whose `fieldIndex` is just declaration-order
            // noise. Catch it here rather than emitting a structurally-valid
            // but semantically-wrong MemberAccess with a bogus index.
            if (!frec->scope.valid()
                || frec->kind != DeclarationKind::Variable) {
                return exprError(node, "member access resolved to a non-field "
                                       "symbol (semantic-phase mis-binding)");
            }
            std::uint32_t const fieldIndex = frec->fieldIndex;
            // Field type: prefer the semantic-phase-propagated type on the
            // field-name node; fall back to the symbol record's type.
            TypeId fieldType = model.typeAt(fieldNameN);
            if (!fieldType.valid()) fieldType = frec->type;
            HirNodeId object = base.id;
            if (e.target == "MemberAccessThruPtr") {
                // Arrow form: dereference the LHS pointer first. The Deref's
                // result type is the pointee type (Struct) — read from the
                // interner via the base's Ptr operand.
                TypeId pointeeType = InvalidType;
                if (base.type.valid()
                    && interner.kind(base.type) == TypeKind::Ptr
                    && !interner.operands(base.type).empty()) {
                    pointeeType = interner.operands(base.type)[0];
                }
                // Pass 2 also emitted S_NotAPointer if base.type wasn't a
                // pointer, but we still need a type here for the Deref node
                // to be HIR-verifier-valid (it requires a valid type). If
                // pointee is invalid, leave it InvalidType — the verifier's
                // requiresValidType rule will surface H_TypeUnresolved.
                object = track(builder.makeDeref(base.id, pointeeType,
                                                 HirFlags::Synthetic), node);
            }
            return {track(builder.makeMemberAccess(object, fieldIndex,
                                                   fieldType), node),
                    fieldType};
        }
        return exprError(node, std::format("postfix target '{}' has no lowering", e.target));
    }

    // The argument expressions inside an argList subtree (skip Comma tokens).
    [[nodiscard]] std::vector<NodeId> argExpressions(std::span<NodeId const> postfixRest) {
        std::vector<NodeId> out;
        for (NodeId r : postfixRest) {
            if (isToken(r)) continue;          // ')' etc.
            // r is the argList node; gather its expression children.
            for (NodeId c : visible(r)) {
                if (isToken(c)) continue;       // commas
                out.push_back(c);
            }
        }
        return out;
    }

    // ── statements ────────────────────────────────────────────────────────────
    HirNodeId lowerStmt(NodeId node) {
        HirRuleMapping const* m = mappingFor(node);
        if (m == nullptr) {
            // Transparent wrapper (e.g. `varDecl = [varDeclHead, ';']`): descend.
            NodeId only = soleMeaningfulChild(node);
            if (only.valid()) return lowerStmt(only);
            unsupported(node, "statement has no hirLowering mapping");
            return errorNode(node);
        }
        std::string const& k = m->hirKind;
        if (k == "VarDecl")     return lowerVarDecl(node);
        if (k == "TypeDecl")    return lowerTypeDecl(node);
        if (k == "Block")       return lowerBlock(node);
        if (k == "ExprStmt")    return lowerExprStmt(node);
        if (k == "ReturnStmt")  return lowerReturn(node);
        if (k == "BreakStmt")    return track(builder.makeBreak(0), node);
        if (k == "ContinueStmt") return track(builder.makeContinue(0), node);
        if (k == "IfStmt")      return lowerIf(node);
        if (k == "WhileStmt")   return lowerWhile(node, /*doWhile=*/false);
        if (k == "DoWhileStmt") return lowerWhile(node, /*doWhile=*/true);
        if (k == "ForStmt")     return lowerFor(node);
        if (k == "SwitchStmt")  return lowerSwitch(node);
        // HR10: a rule mapped to a registered extension kind → an Extension node.
        if (extKindByName_.count(k)) return lowerExtensionNode(node, *m);
        unsupported(node, std::format("statement maps to unsupported HIR kind '{}'", k));
        return errorNode(node);
    }

    // A statement-context expression: an assignment becomes an AssignStmt (HIR
    // has no assignment expression); anything else wraps in ExprStmt.
    HirNodeId lowerStmtExpr(NodeId exprNode) {
        // Peel the `expression` wrapper so a top-level assignment is recognized
        // (C's assignment is an expression; HIR's AssignStmt is a statement, so
        // an assignment in statement position lowers to AssignStmt, not ExprStmt).
        return lowerStmtExprCore(peelToCore(exprNode), /*wrapBare=*/true);
    }

    HirNodeId lowerAssign(NodeId binNode) {
        NodeId lhsN{}, rhsN{};
        for (NodeId c : visible(binNode)) {
            if (isToken(c)) continue;
            if (!lhsN.valid()) lhsN = c; else if (!rhsN.valid()) rhsN = c;
        }
        if (!lhsN.valid() || !rhsN.valid())  // malformed (parse-recovery) node — never abort
            return reportedError(binNode, "malformed assignment expression");
        E lhs = lowerExpr(lhsN), rhs = lowerExpr(rhsN);
        return track(builder.makeAssignStmt(lhs.id, rhs.id), binNode);
    }

    // A simple, side-effect-free lvalue (a plain variable reference): its CST
    // peels to an `operand` whose content is an Identifier. Such an lvalue can be
    // READ MORE THAN ONCE with no observable effect, which is what lets
    // compound-assignment and ++/-- lower by duplicating it. Returns (symbol,
    // type); nullopt for a complex lvalue (index / deref / call — those would
    // need once-only evaluation HIR can't express, and c-subset can't form them).
    [[nodiscard]] std::optional<std::pair<SymbolId, TypeId>> simpleLvalue(NodeId exprCst) {
        NodeId core = peelToCore(exprCst);
        if (tree().kind(core) != NodeKind::Internal || tree().rule(core).v != cfg.operandRule.v)
            return std::nullopt;
        for (NodeId c : visible(core)) {
            if (isToken(c) && sem.identifierToken.valid()
                && tree().tokenKind(c).v == sem.identifierToken.v)
                return std::pair{model.symbolAt(c), typeAtOr(core, InvalidType)};
        }
        return std::nullopt;
    }
    // A synthetic `1` literal of `type` (for ++/--). Synthetic ⇒ no source span.
    [[nodiscard]] HirNodeId synthOne(TypeId type) {
        TypeKind const core = type.valid() ? interner.kind(type) : TypeKind::I32;
        HirLiteralValue v;
        v.core = core;
        if (isFloatCore(core))       v.value = 1.0;
        else if (isSignedCore(core)) v.value = std::int64_t{1};
        else                         v.value = std::uint64_t{1};
        return builder.makeLiteral(type, literals.add(v), HirFlags::Synthetic);
    }

    // A fresh SymbolId for a lowering-synthesized temporary, minted above the
    // semantic symbol table so it can never collide with a source symbol. These
    // temps are self-contained (declared + referenced within one SeqExpr/Block),
    // so they need no name table — the `.dsshir` writer falls back to a `%sN`
    // handle and MIR maps them by their VarDecl node, not by an external table.
    [[nodiscard]] SymbolId freshSymbol() {
        if (nextSyntheticSym_ == 0)
            nextSyntheticSym_ = static_cast<std::uint32_t>(model.symbols().size());
        return SymbolId{nextSyntheticSym_++};
    }
    std::uint32_t nextSyntheticSym_ = 0;

    // A resolved lvalue: how to READ its current value and WRITE a new one, plus
    // the prep statements that must run FIRST. A SIMPLE variable lvalue needs no
    // prep (reading a `Ref` is side-effect-free, so it can be read repeatedly). A
    // COMPLEX lvalue (an index / deref whose address sub-expressions may have
    // side effects) binds its ADDRESS into a temp pointer once in `prep`, then
    // reads/writes through `*ptr` — so `a[f()] += 1` evaluates `f()` exactly
    // once. This is what makes compound-assign / ++ / assignment-as-value correct
    // for every lvalue, not just simple variables.
    struct Lvalue {
        bool                   simple = true;
        TypeId                 type{};       // the lvalue's value type
        SymbolId               sym{};        // simple: the variable; via-ptr: the temp pointer
        TypeId                 ptrType{};    // via-ptr only: interner.pointer(type)
        std::vector<HirNodeId> prep;         // via-ptr only: [ var ptr = &<lvalue> ]
    };

    [[nodiscard]] HirNodeId lvRead(Lvalue const& lv) {
        if (lv.simple) return builder.makeRef(lv.type, lv.sym.v);
        return builder.makeDeref(builder.makeRef(lv.ptrType, lv.sym.v), lv.type, HirFlags::Synthetic);
    }
    [[nodiscard]] HirNodeId lvWrite(Lvalue const& lv, HirNodeId value) {
        HirNodeId target = lv.simple
            ? builder.makeRef(lv.type, lv.sym.v)
            : builder.makeDeref(builder.makeRef(lv.ptrType, lv.sym.v), lv.type, HirFlags::Synthetic);
        return builder.makeAssignStmt(target, value);
    }

    // Classify an lvalue CST. A plain variable → simple (no prep). Anything else
    // (index / deref) → via a temp pointer bound in `prep`. nullopt when the
    // lvalue can't be lowered (no resolved type / not an addressable form).
    [[nodiscard]] std::optional<Lvalue> classifyLvalue(NodeId exprCst) {
        if (auto s = simpleLvalue(exprCst)) {
            Lvalue lv; lv.simple = true; lv.sym = s->first; lv.type = s->second;
            if (!lv.type.valid()) return std::nullopt;
            return lv;
        }
        E target = lowerExpr(peelToCore(exprCst));
        if (!target.type.valid()) return std::nullopt;
        Lvalue lv;
        lv.simple  = false;
        lv.type    = target.type;
        lv.ptrType = interner.pointer(target.type);
        lv.sym     = freshSymbol();
        HirNodeId addr = builder.makeAddressOf(target.id, lv.ptrType, HirFlags::Synthetic);
        lv.prep.push_back(builder.makeVarDecl(lv.ptrType, lv.sym.v, addr, HirFlags::Synthetic));
        return lv;
    }

    // Wrap [prep..., assign] as a single statement: the bare assign when there's
    // no prep (simple lvalue), else a Block (complex lvalue's temp-pointer bind +
    // the store). Used by statement-position compound-assign / ++.
    [[nodiscard]] HirNodeId asStmt(Lvalue const& lv, HirNodeId assign, NodeId cst) {
        if (lv.prep.empty()) return track(assign, cst);
        std::vector<HirNodeId> stmts = lv.prep;
        stmts.push_back(assign);
        return track(builder.makeBlock(stmts), cst);
    }

    // `lhs OP= rhs` → `lhs = lhs OP rhs` (statement). Safe only for a simple
    // lvalue (duplicating the read has no effect); complex lvalues fail loud.
    HirNodeId lowerCompoundAssign(NodeId binNode, std::string const& baseOpName) {
        NodeId lhsN{}, rhsN{};
        for (NodeId c : visible(binNode)) {
            if (isToken(c)) continue;
            if (!lhsN.valid()) lhsN = c; else if (!rhsN.valid()) rhsN = c;
        }
        auto op = coreOpFromName(baseOpName);
        auto lv = lhsN.valid() ? classifyLvalue(lhsN) : std::nullopt;
        if (!lv || !rhsN.valid() || !op || arityOf(*op) != HirOpArity::Binary)
            return reportedError(binNode, "compound assignment needs an lvalue and a binary base op");
        HirNodeId rhs = lowerExpr(rhsN).id;
        // lhs OP= rhs → lhs = (lhs OP rhs); the lvalue is read once + written once
        // (a complex lvalue's address is bound in lv.prep, evaluated once).
        HirNodeId value = track(builder.addParent(HirKind::BinaryOp, std::array{lvRead(*lv), rhs},
                                                  lv->type, encodeOp(*op)), binNode);
        return asStmt(*lv, lvWrite(*lv, value), binNode);
    }

    // `x++` / `x--` in STATEMENT position → `x = x +/- 1` (the produced value is
    // discarded). Value-yielding ++/-- (e.g. `y = x++`) lowers via a SeqExpr in
    // lowerPostfix.
    HirNodeId lowerIncDecStmt(NodeId postfixNode, bool isInc) {
        NodeId baseN{};
        for (NodeId c : visible(postfixNode)) { if (!isToken(c)) { baseN = c; break; } }
        auto lv = baseN.valid() ? classifyLvalue(baseN) : std::nullopt;
        if (!lv) return reportedError(postfixNode, "++/-- needs an lvalue operand");
        HirNodeId one = synthOne(lv->type);
        HirNodeId value = track(builder.addParent(HirKind::BinaryOp, std::array{lvRead(*lv), one},
                                                  lv->type,
                                                  encodeOp(isInc ? HirOpKind::Add : HirOpKind::Sub)),
                                postfixNode);
        return asStmt(*lv, lvWrite(*lv, value), postfixNode);
    }

    // The statement-position dispatch shared by exprStmt and for-init/update:
    // assignment / compound-assignment / inc-dec become statements; anything else
    // is the bare lowered expression (wrapped in ExprStmt when `wrapBare`).
    HirNodeId lowerStmtExprCore(NodeId core, bool wrapBare) {
        if (tree().kind(core) == NodeKind::Internal) {
            std::uint32_t const rv = tree().rule(core).v;
            if (rv == cfg.binaryExprRule.v) {
                for (NodeId c : visible(core)) {
                    if (!isToken(c)) continue;
                    auto it = binOp_.find(tree().tokenKind(c).v);
                    if (it != binOp_.end() && cfg.binaryOps[it->second].target == "Assign") {
                        std::string const& base = cfg.binaryOps[it->second].compoundBase;
                        return base.empty() ? lowerAssign(core) : lowerCompoundAssign(core, base);
                    }
                    break;  // first token is the operator
                }
            } else if (rv == cfg.postfixExprRule.v) {
                for (NodeId c : visible(core)) {
                    if (!isToken(c)) continue;
                    auto it = postOp_.find(tree().tokenKind(c).v);
                    if (it != postOp_.end()) {
                        std::string const& t = cfg.postfixOps[it->second].target;
                        if (t == "PostInc") return lowerIncDecStmt(core, /*isInc=*/true);
                        if (t == "PostDec") return lowerIncDecStmt(core, /*isInc=*/false);
                    }
                    break;
                }
            }
        }
        HirNodeId e = lowerExpr(core).id;
        return wrapBare ? track(builder.makeExprStmt(e), core) : e;
    }

    HirNodeId lowerExprStmt(NodeId node) {
        for (NodeId c : visible(node)) if (!isToken(c)) return lowerStmtExpr(c);
        unsupported(node, "expression statement has no expression");
        return errorNode(node);
    }

    HirNodeId lowerBlock(NodeId node) {
        std::vector<HirNodeId> stmts;
        for (NodeId c : visible(node)) {
            if (isToken(c)) continue;  // { }
            stmts.push_back(lowerStmt(c));
        }
        return track(builder.makeBlock(stmts), node);
    }

    HirNodeId lowerReturn(NodeId node) {
        for (NodeId c : visible(node)) {
            if (isToken(c)) continue;
            return track(builder.makeReturn(lowerExpr(c).id), node);
        }
        return track(builder.makeReturn(std::nullopt), node);
    }

    HirNodeId lowerIf(NodeId node) {
        std::optional<HirNodeId> cond;
        std::vector<HirNodeId> bodies;
        for (NodeId c : visible(node)) {
            if (isToken(c)) continue;
            Role const role = classify(c);
            if (role == Role::Expr && !cond) cond = lowerExpr(c).id;
            else if (role == Role::Stmt)     bodies.push_back(lowerStmt(c));
        }
        HirNodeId condId = orError(cond, node, "if statement has no condition");
        HirNodeId then = bodies.empty() ? reportedError(node, "if statement has no then-branch")
                                        : bodies[0];
        std::optional<HirNodeId> els;
        if (bodies.size() >= 2) els = bodies[1];
        return track(builder.makeIfStmt(condId, then, els), node);
    }

    HirNodeId lowerWhile(NodeId node, bool doWhile) {
        std::optional<HirNodeId> cond, body;
        for (NodeId c : visible(node)) {
            if (isToken(c)) continue;
            Role const role = classify(c);
            if (role == Role::Expr && !cond)      cond = lowerExpr(c).id;
            else if (role == Role::Stmt && !body) body = lowerStmt(c);
        }
        HirNodeId condId = orError(cond, node, "loop has no condition");
        HirNodeId bodyId = orError(body, node, "loop has no body");
        return doWhile ? track(builder.makeDoWhileStmt(bodyId, condId), node)
                       : track(builder.makeWhileStmt(condId, bodyId), node);
    }

    HirNodeId lowerFor(NodeId node) {
        // for ( init? ; cond? ; update? ) body  — segment the header by the
        // `;` separator; the body is the last meaningful child (after `)`).
        std::vector<std::pair<int, NodeId>> clauses;
        int seg = 0;
        for (NodeId c : visible(node)) {
            if (isToken(c)) {
                if (cfg.forClauseSeparator.valid()
                    && tree().tokenKind(c).v == cfg.forClauseSeparator.v) ++seg;
                continue;
            }
            clauses.push_back({seg, c});
        }
        if (clauses.empty()) { unsupported(node, "for has no body"); return errorNode(node); }
        NodeId bodyN = clauses.back().second;
        clauses.pop_back();
        std::optional<HirNodeId> init, cond, update;
        for (auto const& [s, c] : clauses) {
            if (s == 0)      init   = lowerForClause(c);
            else if (s == 1) cond   = lowerExpr(c).id;
            else if (s == 2) update = lowerForClause(c);
        }
        HirNodeId body = lowerStmt(bodyN);
        return track(builder.makeForStmt(init, cond, update, body), node);
    }

    // A for init/update clause: a varDeclHead → VarDecl; an assignment → AssignStmt;
    // otherwise the bare expression.
    HirNodeId lowerForClause(NodeId c) {
        NodeId core = peelToCore(c);
        HirRuleMapping const* m = mappingFor(core);
        if (m != nullptr && m->hirKind == "VarDecl") return lowerVarDecl(core);
        return lowerStmtExprCore(core, /*wrapBare=*/false);
    }

    HirNodeId lowerSwitch(NodeId node) {
        std::optional<HirNodeId> disc;
        std::vector<NodeId> items;
        for (NodeId c : visible(node)) {
            if (isToken(c)) continue;
            if (!disc && classify(c) == Role::Expr) { disc = lowerExpr(c).id; continue; }
            items.push_back(c);   // switchBodyItem wrappers (caseLabel | statement)
        }
        HirNodeId discId = orError(disc, node, "switch has no discriminant");

        std::vector<HirNodeId> arms;
        std::optional<NodeId>  curValue;       // the case match expression, if any
        bool                   curIsDefault = false;
        bool                   haveArm = false;
        std::vector<HirNodeId> curBody;
        auto flush = [&]() {
            if (!haveArm) return;
            std::optional<HirNodeId> value;
            if (!curIsDefault && curValue) value = lowerExpr(*curValue).id;
            arms.push_back(track(builder.makeCaseArm(value, curBody), node));
            curBody.clear();
        };
        for (NodeId raw : items) {
            NodeId core = peelToCore(raw);
            bool const isLabel = cfg.caseLabelRule.valid()
                && tree().kind(core) == NodeKind::Internal
                && tree().rule(core).v == cfg.caseLabelRule.v;
            if (isLabel) {
                flush();
                haveArm = true;
                curIsDefault = false;
                curValue = std::nullopt;
                for (NodeId lc : visible(core)) {
                    if (isToken(lc)) {
                        if (cfg.caseDefaultToken.valid()
                            && tree().tokenKind(lc).v == cfg.caseDefaultToken.v)
                            curIsDefault = true;
                    } else {
                        curValue = lc;   // the case match expression
                    }
                }
            } else if (haveArm) {
                curBody.push_back(lowerStmt(core));
            }
        }
        flush();
        return track(builder.makeSwitchStmt(discId, arms), node);
    }

    // ── declarations ──────────────────────────────────────────────────────────
    // A `var`-style declaration. The SAME rule lowers to a local `VarDecl`
    // inside a block and to a `Global` at module scope (`asGlobal`); a language
    // whose top-level and local variables share one rule — toy's `varDecl` — is
    // disambiguated by lowering context, not by a second rule.
    HirNodeId lowerVarLike(NodeId node, bool asGlobal) {
        if (subtreeHasDeferred(node))
            return reportedError(node, "array declarator is deferred to HR9 "
                                       "(the lattice has no Array type yet)");
        auto it = declMap_.find(tree().rule(node).v);
        DeclarationRule const* decl = (it != declMap_.end()) ? &sem.declarations[it->second] : nullptr;
        auto vis = visible(node);
        SymbolId sym{};
        TypeId type = InvalidType;
        if (decl && decl->nameChild && *decl->nameChild < vis.size()) {
            NodeId nameNode = vis[*decl->nameChild];
            // The symbol may sit on a name token nested under a wrapper (tsql's
            // columnDecl name is a `nameAtom`, not a bare Identifier); probe for it.
            sym = model.symbolAt(nameNode);
            if (!sym.valid()) sym = firstNameToken(nameNode).sym;
            if (auto const* rec = model.recordFor(sym)) type = rec->type;
        }
        std::optional<HirNodeId> init;
        for (NodeId c : vis) {
            // Skip an array-declarator suffix: its `[N]` length expression is
            // part of the TYPE (already folded into an Array<elem,N> by the
            // semantic phase), not the variable's initializer.
            if (decl && decl->arraySuffix && tree().kind(c) == NodeKind::Internal
                && tree().rule(c).v == decl->arraySuffix->rule.v)
                continue;
            if (classify(c) == Role::Expr) { init = lowerExpr(c).id; break; }
        }
        return asGlobal ? track(builder.makeGlobal(type, sym.v, init), node)
                        : track(builder.makeVarDecl(type, sym.v, init), node);
    }

    HirNodeId lowerVarDecl(NodeId node) { return lowerVarLike(node, /*asGlobal=*/false); }

    HirNodeId lowerTypeDecl(NodeId node) {
        auto it = declMap_.find(tree().rule(node).v);
        DeclarationRule const* decl = (it != declMap_.end()) ? &sem.declarations[it->second] : nullptr;
        auto vis = visible(node);
        SymbolId sym{};
        TypeId type = InvalidType;
        if (decl && decl->nameChild && *decl->nameChild < vis.size()) {
            sym = model.symbolAt(vis[*decl->nameChild]);
            if (auto const* rec = model.recordFor(sym)) type = rec->type;
        }
        return track(builder.makeTypeDecl(type, sym.v), node);
    }

    // topLevelDecl → Function (when the kindByChild discriminator resolves to
    // funcDefTail) or Global.
    HirNodeId lowerTopLevel(NodeId node) {
        auto it = declMap_.find(tree().rule(node).v);
        if (it == declMap_.end()) { unsupported(node, "top-level decl has no semantics rule"); return errorNode(node); }
        DeclarationRule const& decl = sem.declarations[it->second];
        auto vis = visible(node);
        SymbolId sym{};
        TypeId type = InvalidType;
        if (decl.nameChild && *decl.nameChild < vis.size()) {
            sym = model.symbolAt(vis[*decl.nameChild]);
            if (auto const* rec = model.recordFor(sym)) type = rec->type;
        }
        // Function iff the kindByChild discriminator matches funcDefTail.
        NodeId discNode{};
        if (decl.kindByChild) {
            discNode = descend(node, decl.kindByChild->childPath);
            if (discNode.valid() && tree().kind(discNode) == NodeKind::Internal
                && tree().rule(discNode).v == decl.kindByChild->whenRule.v) {
                return lowerFunction(node, sym, type, *decl.kindByChild, discNode);
            }
        }
        // Global.
        if (subtreeHasDeferred(node))
            return reportedError(node, "array declarator is deferred to HR9 "
                                       "(the lattice has no Array type yet)");
        std::optional<HirNodeId> init;
        RuleId const skip = decl.arraySuffix ? decl.arraySuffix->rule : RuleId{};
        for (NodeId c : descendantsForInit(node, skip)) if (isExprNode(c)) { init = lowerExpr(c).id; break; }
        return track(builder.makeGlobal(type, sym.v, init), node);
    }

    // A function declared by a DEDICATED rule (e.g. toy's `funcDef`), as opposed
    // to c-subset's dual-purpose `topLevelDecl`+kindByChild. Reads the params/body
    // subtrees from the semantic DeclarationRule's `paramsChild`/`bodyChild`
    // visible-child indices.
    HirNodeId lowerFunctionDecl(NodeId node) {
        auto it = declMap_.find(tree().rule(node).v);
        if (it == declMap_.end()) return reportedError(node, "function decl has no semantics rule");
        DeclarationRule const& decl = sem.declarations[it->second];
        auto vis = visible(node);
        SymbolId sym{};
        TypeId sig = InvalidType;
        if (decl.nameChild && *decl.nameChild < vis.size()) {
            sym = model.symbolAt(vis[*decl.nameChild]);
            if (auto const* rec = model.recordFor(sym)) sig = rec->type;
        }
        std::vector<HirNodeId> params;
        if (decl.paramsChild && *decl.paramsChild < vis.size())
            collectParams(vis[*decl.paramsChild], params);
        HirNodeId body = (decl.bodyChild && *decl.bodyChild < vis.size())
                       ? lowerStmt(vis[*decl.bodyChild])
                       : track(builder.makeBlock({}), node);
        return track(builder.makeFunction(sig, sym.v, params, body), node);
    }

    // extern function / global (no body). The FnSig/var type comes from the
    // symbol the semantic phase minted (FFI linkage metadata is plan 11).
    HirNodeId lowerExternDecl(NodeId node) {
        auto it = declMap_.find(tree().rule(node).v);
        if (it == declMap_.end()) return reportedError(node, "extern decl has no semantics rule");
        DeclarationRule const& decl = sem.declarations[it->second];
        auto vis = visible(node);
        SymbolId sym{};
        TypeId type = InvalidType;
        if (decl.nameChild && *decl.nameChild < vis.size()) {
            sym = model.symbolAt(vis[*decl.nameChild]);
            if (auto const* rec = model.recordFor(sym)) type = rec->type;
        }
        if (decl.kindByChild) {
            NodeId disc = descend(node, decl.kindByChild->childPath);
            if (disc.valid() && tree().kind(disc) == NodeKind::Internal
                && tree().rule(disc).v == decl.kindByChild->whenRule.v) {
                std::vector<HirNodeId> params;
                NodeId paramsNode = descend(disc, decl.kindByChild->paramsPath);
                if (paramsNode.valid()) collectParams(paramsNode, params);
                return track(builder.makeExternFunction(type, sym.v, params), node);
            }
        }
        return track(builder.makeExternGlobal(type, sym.v), node);
    }

    HirNodeId lowerFunction(NodeId node, SymbolId sym, TypeId sig,
                            KindDiscriminator const& disc, NodeId discNode) {
        std::vector<HirNodeId> params;
        NodeId paramsNode = descend(discNode, disc.paramsPath);
        if (paramsNode.valid()) collectParams(paramsNode, params);
        NodeId bodyNode = descend(discNode, disc.bodyPath);
        HirNodeId body = bodyNode.valid() ? lowerStmt(bodyNode) : track(builder.makeBlock({}), node);
        return track(builder.makeFunction(sig, sym.v, params, body), node);
    }

    // Gather param VarDecls under a funcParams subtree (nodes mapped to VarDecl).
    void collectParams(NodeId n, std::vector<HirNodeId>& out) {
        HirRuleMapping const* m = mappingFor(n);
        if (m != nullptr && m->hirKind == "VarDecl") { out.push_back(lowerVarDecl(n)); return; }
        for (NodeId c : visible(n)) if (!isToken(c)) collectParams(c, out);
    }

    // Direct children to scan for a global's initializer expression. A subtree
    // rooted at `skipRule` (the array-declarator suffix) is pruned so a global
    // array's `[N]` length is never mistaken for the initializer.
    [[nodiscard]] std::vector<NodeId> descendantsForInit(NodeId node, RuleId skipRule = {}) {
        std::vector<NodeId> out;
        std::vector<NodeId> stack = visible(node);
        while (!stack.empty()) {
            NodeId c = stack.back(); stack.pop_back();
            if (skipRule.valid() && tree().kind(c) == NodeKind::Internal
                && tree().rule(c).v == skipRule.v)
                continue;
            if (isExprNode(c)) { out.push_back(c); continue; }
            if (tree().kind(c) == NodeKind::Internal)
                for (NodeId g : visible(c)) stack.push_back(g);
        }
        return out;
    }

    [[nodiscard]] NodeId descend(NodeId start, std::vector<std::uint32_t> const& path) {
        NodeId cur = start;
        for (auto idx : path) {
            if (!cur.valid()) return {};
            auto vis = visible(cur);
            if (idx >= vis.size()) return {};
            cur = vis[idx];
        }
        return cur;
    }

    HirNodeId lowerDecl(NodeId node) {
        // Peel the `topLevel` alt wrapper (and any nested wrappers) to the real
        // declaration node.
        NodeId core = peelToCore(node);
        HirRuleMapping const* m = mappingFor(core);
        if (m == nullptr) {
            unsupported(core, std::format("top-level construct '{}' is not lowered "
                                          "(no hirLowering mapping)",
                                          tree().kind(core) == NodeKind::Internal
                                              ? std::string{tree().rules().name(tree().rule(core))}
                                              : std::string{"<token>"}));
            return errorNode(core);
        }
        // "Skip": a top-level construct that contributes NO HIR node (e.g. an
        // `#include` directive — its declarations arrive via the CU import
        // resolver's cross-refs, not as HIR nodes from the directive itself).
        // Config-driven (no hardcoded rule name); lowerModule drops invalid ids.
        if (m->hirKind == "Skip")       return HirNodeId{};
        if (m->hirKind == "Decl")       return lowerTopLevel(core);
        if (m->hirKind == "Function")   return lowerFunctionDecl(core);
        if (m->hirKind == "TypeDecl")   return lowerTypeDecl(core);
        if (m->hirKind == "ExternDecl") return lowerExternDecl(core);
        // A `var`-style declaration at module scope is a Global (the same rule
        // is a local VarDecl inside a block — see lowerVarLike).
        if (m->hirKind == "VarDecl")    return lowerVarLike(core, /*asGlobal=*/true);
        // A bare statement-level decl appearing at top level (unusual): route it.
        return lowerStmt(core);
    }

    // ── driver ─────────────────────────────────────────────────────────────────
    // Lower one tree's top-level declarations, appending to the shared module
    // decls (in tree order). The caller selects the Lowerer whose schema matches
    // this tree (HR11), so `lowerDecl` always reads this tree's own language config.
    void lowerTree(Tree const& t, std::vector<HirNodeId>& decls) {
        t_ = &t;
        if (!t.root().valid()) return;
        for (NodeId top : visible(t.root())) {
            if (isToken(top)) continue;
            HirNodeId const d = lowerDecl(top);
            if (d.valid()) decls.push_back(d);   // skip "Skip"-mapped nodes (e.g. #include)
        }
    }
};

} // namespace

std::unique_ptr<CstToHirResult> lowerToHir(SemanticModel& model, DiagnosticReporter& reporter) {
    std::size_t const errBefore = reporter.errorCount();

    // The shared output every per-schema Lowerer writes into: one builder (→ one
    // module, arena, kind registry, literal pool) + one literal pool + one span
    // list. The module is labelled with the CU's composite source language.
    auto const trees = model.unit().trees();
    HirBuilder builder{model.unit().compositeSourceLanguage()};
    HirLiteralPool literals;
    std::vector<std::pair<HirNodeId, HirSourceLoc>> spans;

    // One Lowerer per distinct schema in the CU (keyed by SchemaId), each bound
    // to its language's config + the shared output. `Tree::schema()` is the
    // authoritative per-file language.
    std::unordered_map<std::uint32_t, std::unique_ptr<Lowerer>> lowerers;
    for (Tree const& t : trees) {
        GrammarSchema const& sch = t.schema();
        if (lowerers.contains(sch.schemaId().v)) continue;
        lowerers.emplace(sch.schemaId().v, std::make_unique<Lowerer>(
            model, sch.hirLowering(), sch.semantics(), sch.numberStyle(),
            reporter, builder, literals, spans));
    }

    // Lower every tree IN ORDER, dispatching to its schema's Lowerer, into the
    // one shared decls list (so module decls follow tree-add order).
    std::vector<HirNodeId> decls;
    for (Tree const& t : trees) {
        lowerers.at(t.schema().schemaId().v)->lowerTree(t, decls);
    }

    HirNodeId const root = builder.makeModule(decls);
    Hir hir = std::move(builder).finish(root);
    lowerers.clear();   // drop the Lowerers (their builder ref is now moved-from)

    auto result = std::make_unique<CstToHirResult>(std::move(hir), std::move(literals));
    for (auto& [id, loc] : spans) result->sourceMap.set(id, loc);

    // verify-on-load.
    HirVerifier verifier{result->hir, &result->sourceMap, &model.lattice().interner()};
    (void)verifier.verify(reporter);

    result->ok = reporter.errorCount() == errBefore;
    return result;
}

} // namespace dss
