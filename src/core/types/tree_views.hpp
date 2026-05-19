#pragma once

#include "core/types/rule_id.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"
#include "core/types/well_known_names.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <vector>

// Typed views — ergonomic accessors over `(Tree const&, NodeId)`. A view
// is a tiny POD-like wrapper (Tree pointer + NodeId, occasionally one
// extra byte) that gives named accessors for the role-positions of a
// rule's children, or for a token's text/kind.
//
// Pattern:
//   - Public `View(Tree const&, NodeId)` constructor — UNCHECKED.
//   - Static factory `View::from(tree, id) -> std::optional<View>` that
//     verifies the rule / token-kind before constructing. Returns
//     std::nullopt on mismatch. Cheap (one RuleId / SchemaTokenId compare).
//   - Accessors trust the schema-aware TreeBuilder produced a valid shape.
//     For a hand-built tree that violates the assumed structure,
//     accessors may abort via `Tree::children()`'s bounds check or
//     return InvalidNode (documented per-accessor).
//
// Lifetime contract: a view stores `Tree const*`. The caller MUST keep
// the bound Tree alive — same rule as `TreeCursor` / `NodeAttribute`.
//
// EmptySpace handling: rule-level structural accessors (e.g. lhs/op/rhs)
// walk only VISIBLE children (NodeFlags::EmptySpace is skipped), so
// whitespace between siblings doesn't break role indexing.

namespace dss {

namespace detail::views {

[[noreturn]] inline void viewFatal(char const* what) {
    std::fputs("dss::tree_views fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

[[nodiscard]] inline bool isVisible_(NodeFlags f) noexcept {
    return !isEmptySpace(f);
}

[[nodiscard]] inline std::size_t visibleChildCount(Tree const& t, NodeId parent) noexcept {
    if (!parent.valid()) return 0;
    std::size_t n = 0;
    for (NodeId c : t.children(parent)) {
        if (isVisible_(t.flags(c))) ++n;
    }
    return n;
}

// Returns the `index`-th visible child of `parent`, or InvalidNode if
// `parent` has fewer than (index+1) visible children. Zero-allocation;
// stops walking at the requested index.
[[nodiscard]] inline NodeId nthVisibleChild(Tree const& t, NodeId parent, std::size_t index) noexcept {
    if (!parent.valid()) return InvalidNode;
    std::size_t seen = 0;
    for (NodeId c : t.children(parent)) {
        if (!isVisible_(t.flags(c))) continue;
        if (seen == index) return c;
        ++seen;
    }
    return InvalidNode;
}

[[nodiscard]] inline std::vector<NodeId> visibleChildren(Tree const& t, NodeId parent) {
    std::vector<NodeId> out;
    if (!parent.valid()) return out;
    auto kids = t.children(parent);
    out.reserve(kids.size());
    for (NodeId c : kids) {
        if (isVisible_(t.flags(c))) out.push_back(c);
    }
    return out;
}

[[nodiscard]] inline RuleId ruleIdFor(Tree const& t, std::string_view name) noexcept {
    return t.rules().find(name);
}

// Token-kind name → SchemaTokenId via the tree's schema. Returns the
// invalid sentinel when the tree has no schema OR the name isn't
// registered — same shape as ruleIdFor's "invalid id on miss" contract,
// so token-level `View::from` returns std::nullopt cleanly on hand-built
// schema-less trees instead of aborting.
[[nodiscard]] inline SchemaTokenId tokenKindFor(Tree const& t, std::string_view name) noexcept {
    if (!t.hasSchema()) return InvalidSchemaToken;
    return t.schema().schemaTokens().find(name);
}

} // namespace detail::views

// ── IdentifierView ───────────────────────────────────────────────────────

class IdentifierView {
public:
    IdentifierView(Tree const& t, NodeId id) noexcept : tree_(&t), id_(id) {}

    [[nodiscard]] static std::optional<IdentifierView> from(Tree const& t, NodeId id) {
        if (!id.valid()) return std::nullopt;
        if (t.kind(id) != NodeKind::Token) return std::nullopt;
        const auto want = detail::views::tokenKindFor(t, tokens::kIdentifier);
        if (!want.valid()) return std::nullopt;
        if (t.tokenKind(id) != want) return std::nullopt;
        return IdentifierView{t, id};
    }

    [[nodiscard]] NodeId            node() const noexcept { return id_; }
    [[nodiscard]] std::string_view  name() const noexcept { return tree_->text(id_); }
    [[nodiscard]] SourceSpan        span() const noexcept { return tree_->span(id_); }

private:
    Tree const* tree_;
    NodeId      id_;
};

// ── LiteralView ──────────────────────────────────────────────────────────

class LiteralView {
public:
    enum class Kind : std::uint8_t {
        Int, Float, String, Char, Bool, Null,
    };

    LiteralView(Tree const& t, NodeId id) noexcept
        : tree_(&t), id_(id), kind_(resolveKind_(t, id)) {}

    [[nodiscard]] static std::optional<LiteralView> from(Tree const& t, NodeId id) {
        if (!id.valid()) return std::nullopt;
        if (t.kind(id) != NodeKind::Token) return std::nullopt;
        const auto resolved = tryResolveKind_(t, id);
        if (!resolved.has_value()) return std::nullopt;
        return LiteralView{t, id, *resolved};
    }

    [[nodiscard]] NodeId           node() const noexcept { return id_; }
    [[nodiscard]] Kind             kind() const noexcept { return kind_; }
    [[nodiscard]] std::string_view text() const noexcept { return tree_->text(id_); }
    [[nodiscard]] SourceSpan       span() const noexcept { return tree_->span(id_); }

private:
    LiteralView(Tree const& t, NodeId id, Kind k) noexcept
        : tree_(&t), id_(id), kind_(k) {}

    static std::optional<Kind> tryResolveKind_(Tree const& t, NodeId id) noexcept {
        const auto tk = t.tokenKind(id);
        if (!tk.valid()) return std::nullopt;
        if (tk == detail::views::tokenKindFor(t, tokens::kIntLiteral))    return Kind::Int;
        if (tk == detail::views::tokenKindFor(t, tokens::kFloatLiteral))  return Kind::Float;
        if (tk == detail::views::tokenKindFor(t, tokens::kStringLiteral)) return Kind::String;
        if (tk == detail::views::tokenKindFor(t, tokens::kCharLiteral))   return Kind::Char;
        if (tk == detail::views::tokenKindFor(t, tokens::kBoolLiteral))   return Kind::Bool;
        if (tk == detail::views::tokenKindFor(t, tokens::kNullLiteral))   return Kind::Null;
        return std::nullopt;
    }

    // Aborts if the node is not one of the six literal token kinds (the
    // unchecked-ctor contract). Note: with the no-schema fallback in
    // tokenKindFor, a schema-less tree resolves all literal kinds to
    // InvalidSchemaToken — every comparison fails — and this also aborts.
    // Callers wanting a safe path use `from()`.
    static Kind resolveKind_(Tree const& t, NodeId id) {
        if (auto k = tryResolveKind_(t, id)) return *k;
        detail::views::viewFatal("LiteralView constructed over a non-literal token");
    }

    Tree const* tree_;
    NodeId      id_;
    Kind        kind_;
};

// ── BinaryExprView ───────────────────────────────────────────────────────
//
// Assumed shape: rule == "binaryExpr"; visible children == [lhs, op, rhs].

class BinaryExprView {
public:
    BinaryExprView(Tree const& t, NodeId id) noexcept : tree_(&t), id_(id) {}

    [[nodiscard]] static std::optional<BinaryExprView> from(Tree const& t, NodeId id) {
        if (!id.valid() || t.kind(id) != NodeKind::Internal) return std::nullopt;
        const auto want = detail::views::ruleIdFor(t, rules::kBinaryExpr);
        if (!want.valid() || t.rule(id) != want) return std::nullopt;
        return BinaryExprView{t, id};
    }

    [[nodiscard]] NodeId           node()    const noexcept { return id_; }
    [[nodiscard]] SourceSpan       span()    const noexcept { return tree_->span(id_); }
    [[nodiscard]] NodeId           lhs()     const noexcept { return detail::views::nthVisibleChild(*tree_, id_, 0); }
    [[nodiscard]] NodeId           opNode()  const noexcept { return detail::views::nthVisibleChild(*tree_, id_, 1); }
    [[nodiscard]] NodeId           rhs()     const noexcept { return detail::views::nthVisibleChild(*tree_, id_, 2); }
    [[nodiscard]] std::string_view op()      const noexcept {
        const auto n = opNode();
        return n.valid() ? tree_->text(n) : std::string_view{};
    }

private:
    Tree const* tree_;
    NodeId      id_;
};

// ── BlockView ────────────────────────────────────────────────────────────
//
// Assumed shape: rule == "block"; visible children == zero or more
// statement nodes. The view does not assume any particular brace/delim
// structure — those are normally EmptySpace or schema-driven punctuation
// that the grammar config wraps OUTSIDE the block rule.

class BlockView {
public:
    BlockView(Tree const& t, NodeId id) noexcept : tree_(&t), id_(id) {}

    [[nodiscard]] static std::optional<BlockView> from(Tree const& t, NodeId id) {
        if (!id.valid() || t.kind(id) != NodeKind::Internal) return std::nullopt;
        const auto want = detail::views::ruleIdFor(t, rules::kBlock);
        if (!want.valid() || t.rule(id) != want) return std::nullopt;
        return BlockView{t, id};
    }

    [[nodiscard]] NodeId      node()           const noexcept { return id_; }
    [[nodiscard]] SourceSpan  span()           const noexcept { return tree_->span(id_); }
    [[nodiscard]] std::size_t statementCount() const noexcept {
        return detail::views::visibleChildCount(*tree_, id_);
    }
    [[nodiscard]] NodeId      statementAt(std::size_t i) const noexcept {
        return detail::views::nthVisibleChild(*tree_, id_, i);
    }
    // Source-order vector of visible-child NodeIds. Allocates; perf-critical
    // callers should walk children directly via `Tree::cursor()` rather than
    // pair `statementCount` + `statementAt` (that pairing is O(N²)).
    [[nodiscard]] std::vector<NodeId> statements() const {
        return detail::views::visibleChildren(*tree_, id_);
    }

private:
    Tree const* tree_;
    NodeId      id_;
};

// ── FunctionDeclView ─────────────────────────────────────────────────────
//
// Assumed shape: rule == "functionDecl"; visible children ==
// [name (Identifier token), paramList, body]. Languages with leading
// keywords or modifiers should either reshape their grammar config or
// write a dedicated view.

class FunctionDeclView {
public:
    FunctionDeclView(Tree const& t, NodeId id) noexcept : tree_(&t), id_(id) {}

    [[nodiscard]] static std::optional<FunctionDeclView> from(Tree const& t, NodeId id) {
        if (!id.valid() || t.kind(id) != NodeKind::Internal) return std::nullopt;
        const auto want = detail::views::ruleIdFor(t, rules::kFunctionDecl);
        if (!want.valid() || t.rule(id) != want) return std::nullopt;
        return FunctionDeclView{t, id};
    }

    [[nodiscard]] NodeId         node()      const noexcept { return id_; }
    [[nodiscard]] SourceSpan     span()      const noexcept { return tree_->span(id_); }

    // Returns an UNCHECKED IdentifierView. If the underlying name node
    // isn't actually an Identifier token, the chained view's `name()`
    // text accessor still works (returns whatever lexeme is there);
    // callers that want validation should call IdentifierView::from
    // on `nameNode()` instead.
    [[nodiscard]] IdentifierView name()      const noexcept {
        return IdentifierView{*tree_, nameNode()};
    }
    [[nodiscard]] NodeId         nameNode()  const noexcept { return detail::views::nthVisibleChild(*tree_, id_, 0); }
    [[nodiscard]] NodeId         paramList() const noexcept { return detail::views::nthVisibleChild(*tree_, id_, 1); }
    [[nodiscard]] NodeId         body()      const noexcept { return detail::views::nthVisibleChild(*tree_, id_, 2); }

private:
    Tree const* tree_;
    NodeId      id_;
};

// ── VarDeclView (toy-aligned) ────────────────────────────────────────────
//
// Assumed shape (toy.lang.json):
//   rule == "varDecl";
//   visible children == [VarKeyword, Identifier, AssignmentOperator,
//                        expression, EndCommand].

class VarDeclView {
public:
    VarDeclView(Tree const& t, NodeId id) noexcept : tree_(&t), id_(id) {}

    [[nodiscard]] static std::optional<VarDeclView> from(Tree const& t, NodeId id) {
        if (!id.valid() || t.kind(id) != NodeKind::Internal) return std::nullopt;
        const auto want = detail::views::ruleIdFor(t, rules::kVarDecl);
        if (!want.valid() || t.rule(id) != want) return std::nullopt;
        return VarDeclView{t, id};
    }

    [[nodiscard]] NodeId         node()      const noexcept { return id_; }
    [[nodiscard]] SourceSpan     span()      const noexcept { return tree_->span(id_); }
    [[nodiscard]] IdentifierView name()      const noexcept { return IdentifierView{*tree_, nameNode()}; }
    [[nodiscard]] NodeId         nameNode()  const noexcept { return detail::views::nthVisibleChild(*tree_, id_, 1); }
    [[nodiscard]] NodeId         value()     const noexcept { return detail::views::nthVisibleChild(*tree_, id_, 3); }

private:
    Tree const* tree_;
    NodeId      id_;
};

// ── ExprStmtView (toy-aligned) ───────────────────────────────────────────
//
// Assumed shape: rule == "exprStmt"; visible children == [expression, EndCommand].

class ExprStmtView {
public:
    ExprStmtView(Tree const& t, NodeId id) noexcept : tree_(&t), id_(id) {}

    [[nodiscard]] static std::optional<ExprStmtView> from(Tree const& t, NodeId id) {
        if (!id.valid() || t.kind(id) != NodeKind::Internal) return std::nullopt;
        const auto want = detail::views::ruleIdFor(t, rules::kExprStmt);
        if (!want.valid() || t.rule(id) != want) return std::nullopt;
        return ExprStmtView{t, id};
    }

    [[nodiscard]] NodeId     node()        const noexcept { return id_; }
    [[nodiscard]] SourceSpan span()        const noexcept { return tree_->span(id_); }
    [[nodiscard]] NodeId     expression()  const noexcept { return detail::views::nthVisibleChild(*tree_, id_, 0); }

private:
    Tree const* tree_;
    NodeId      id_;
};

} // namespace dss
