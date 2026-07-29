#pragma once

#include "analysis/semantic/semantic_model.hpp"
#include "core/export.hpp"
#include "core/types/strong_ids.hpp"

#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// CU-scoped scope tree: enter / leave / lookup-by-parent-chain / bind.
// Slot 0 is the InvalidScope sentinel; slot 1 is the CU root scope —
// minted at construction. Each tree gets its OWN root scope as a child
// of the CU root (the analyzer pushes one per tree), so two unrelated
// files' top-level decls do NOT share a namespace; cross-file
// visibility comes only from explicit crossRefs injection. Push/pop is
// stack-disciplined and tracked externally (the analyzer threads a
// `current` ScopeId through its CST walks).

namespace dss {

class DSS_EXPORT ScopeTree {
public:
    ScopeTree();

    // Mint a new child scope of `parent`, anchored at `anchor` (the rule
    // node whose subtree opens it). Returns the new ScopeId. Aborts on
    // an invalid parent.
    ScopeId pushScope(ScopeId parent, NodeId anchor, TreeId tree);

    // The root scope (always ScopeId{1}). Convenience.
    [[nodiscard]] ScopeId root() const noexcept { return ScopeId{1}; }

    // Bind `name` to `symbol` in `scope`, in the C 6.2.3 namespace `ns`
    // (Ordinary by default; Tag for a struct/union/enum tag). Returns the
    // previously-bound SymbolId if a binding for `name` already existed in
    // this scope's `ns` namespace (the caller emits S_RedeclaredSymbol with a
    // RelatedLocation to the prior decl); returns InvalidSymbol on success. A
    // tag and an ordinary symbol of the same name do NOT collide — they live
    // in separate maps.
    SymbolId bind(ScopeId scope, std::string name, SymbolId symbol,
                  SymbolNamespace ns = SymbolNamespace::Ordinary);

    // Inject a binding (no redeclaration check) into namespace `ns`. Used by
    // cross-tree import injection where the same symbol can be visible in
    // multiple root scopes; collisions are pre-filtered by the analyzer.
    void injectBinding(ScopeId scope, std::string name, SymbolId symbol,
                       SymbolNamespace ns = SymbolNamespace::Ordinary);

    // Walk parent chain looking for `name` in namespace `ns`. Returns
    // InvalidSymbol on miss. The walk is left-to-right (innermost-first) so a
    // shadowing inner binding wins.
    [[nodiscard]] SymbolId
    lookup(ScopeId scope, std::string_view name,
           SymbolNamespace ns = SymbolNamespace::Ordinary) const noexcept;

    // TF-C89 (D-CSUBSET-SHIPPED-TYPEDEF-POSITION-BLIND-SUPPRESSION):
    // the PREDICATE-FILTERED sibling of `lookup`. Same
    // innermost-first parent-chain walk, but a hop whose binding does not
    // satisfy `accept` is SKIPPED and the walk CONTINUES outward instead of
    // stopping there. Returns InvalidSymbol when no hop's binding is accepted.
    //
    // This exists because C 6.2.1p7 scopes an identifier "just after the
    // completion of its declarator": in the region BEFORE an inner
    // declaration of a name, the ENCLOSING declaration of that name is the one
    // in scope. This scope tree binds a whole file's declarations up front and
    // therefore cannot tell "before" from "after" positionally — but a caller's
    // own usability test (e.g. "is this a TYPE symbol with a resolved type?")
    // is a faithful proxy, so the outer binding is reachable exactly when the
    // inner one is not yet usable. Plain `lookup` (nearest binding wins,
    // period) stays the default; nothing here is language-, target-, or
    // format-specific — `accept` is entirely the caller's predicate.
    template <typename Accept>
    [[nodiscard]] SymbolId
    lookupIf(ScopeId scope, std::string_view name, Accept&& accept,
             SymbolNamespace ns = SymbolNamespace::Ordinary) const {
        while (scope.valid() && scope.v < scopes_.size()) {
            ScopeRecord const& rec = scopes_[scope.v];
            SymbolId const found = bindingIn(rec, name, ns);
            if (found.valid() && accept(found)) return found;
            scope = rec.parent;
        }
        return InvalidSymbol;
    }

    // Snapshot of every (name, namespace, SymbolId) binding directly in
    // `scope` (no parent walk), across BOTH namespaces. Used by the
    // cross-tree import-injection step to copy a defining tree's root-scope
    // symbols into a referencing tree's root scope — carrying the namespace
    // so a tag re-injects as a tag (and a tag + a same-named typedef do not
    // false-conflict). Returns empty on an invalid/out-of-range scope.
    [[nodiscard]]
    std::vector<std::tuple<std::string_view, SymbolNamespace, SymbolId>>
    bindingsOf(ScopeId scope) const;

    // Move the scope records out for embedding in SemanticModel. The
    // tree is single-use; `release()` consumes the storage and leaves
    // the tree in a moved-from state (caller must not invoke further
    // mutators).
    [[nodiscard]] std::vector<ScopeRecord> release() && noexcept { return std::move(scopes_); }

    // Read-only access for tests / tooling.
    [[nodiscard]] std::vector<ScopeRecord> const& scopes() const noexcept { return scopes_; }

private:
    // The ONE per-scope binding probe both `lookup` and `lookupIf` walk with —
    // namespace selection lives here so the two walks can never drift on which
    // C 6.2.3 namespace map they read.
    [[nodiscard]] static SymbolId
    bindingIn(ScopeRecord const& rec, std::string_view name,
              SymbolNamespace ns) noexcept;

    std::vector<ScopeRecord> scopes_;
};

} // namespace dss
