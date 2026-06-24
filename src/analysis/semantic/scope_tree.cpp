#include "analysis/semantic/scope_tree.hpp"

#include <cstdio>
#include <cstdlib>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace dss {

namespace {

[[noreturn]] void stFatal(char const* what) {
    std::fputs("dss::ScopeTree fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

} // namespace

ScopeTree::ScopeTree() {
    // Slot 0 is the InvalidScope sentinel; slot 1 is the CU root scope.
    // Reserve them both at construction so ScopeId{1} == root() is always
    // valid even before the analyzer pushes any nested scopes.
    scopes_.resize(2);
    // root scope: parent invalid, anchor invalid, tree invalid (CU-wide root).
}

ScopeId ScopeTree::pushScope(ScopeId parent, NodeId anchor, TreeId tree) {
    if (!parent.valid() || parent.v >= scopes_.size()) {
        stFatal("pushScope: parent ScopeId out of range");
    }
    const auto id = ScopeId{static_cast<std::uint32_t>(scopes_.size())};
    ScopeRecord rec;
    rec.parent = parent;
    rec.anchor = anchor;
    rec.tree   = tree;
    scopes_.push_back(std::move(rec));
    scopes_[parent.v].children.push_back(id);
    return id;
}

namespace {
// Select the namespace's binding map within a scope record (C 6.2.3): Tag →
// `tagBindings`, Ordinary → `bindings`. The const overload mirrors it for the
// read-only lookup walk.
std::unordered_map<std::string, SymbolId>&
mapFor(ScopeRecord& rec, SymbolNamespace ns) {
    return ns == SymbolNamespace::Tag ? rec.tagBindings : rec.bindings;
}
std::unordered_map<std::string, SymbolId> const&
mapFor(ScopeRecord const& rec, SymbolNamespace ns) {
    return ns == SymbolNamespace::Tag ? rec.tagBindings : rec.bindings;
}
} // namespace

SymbolId ScopeTree::bind(ScopeId scope, std::string name, SymbolId symbol,
                         SymbolNamespace ns) {
    if (!scope.valid() || scope.v >= scopes_.size()) {
        stFatal("bind: ScopeId out of range");
    }
    auto& bindings = mapFor(scopes_[scope.v], ns);
    auto it = bindings.find(name);
    if (it != bindings.end()) {
        return it->second;   // redecl — caller emits S_RedeclaredSymbol
    }
    bindings.emplace(std::move(name), symbol);
    return InvalidSymbol;
}

void ScopeTree::injectBinding(ScopeId scope, std::string name, SymbolId symbol,
                              SymbolNamespace ns) {
    if (!scope.valid() || scope.v >= scopes_.size()) {
        stFatal("injectBinding: ScopeId out of range");
    }
    // Last-writer-wins for imported symbols; the analyzer pre-filters
    // collisions so this branch is exercised only on a deliberate
    // re-injection (a target tree appearing in two imports of the same
    // source tree).
    mapFor(scopes_[scope.v], ns)[std::move(name)] = symbol;
}

SymbolId ScopeTree::lookup(ScopeId scope, std::string_view name,
                           SymbolNamespace ns) const noexcept {
    while (scope.valid() && scope.v < scopes_.size()) {
        auto const& rec = scopes_[scope.v];
        // unordered_map heterogeneous lookup is C++20 with custom hash/equal;
        // a plain std::string round-trip keeps the lookup self-contained.
        std::string key{name};
        auto const& bindings = mapFor(rec, ns);
        auto it = bindings.find(key);
        if (it != bindings.end()) return it->second;
        scope = rec.parent;
    }
    return InvalidSymbol;
}

std::vector<std::tuple<std::string_view, SymbolNamespace, SymbolId>>
ScopeTree::bindingsOf(ScopeId scope) const {
    std::vector<std::tuple<std::string_view, SymbolNamespace, SymbolId>> out;
    if (!scope.valid() || scope.v >= scopes_.size()) return out;
    auto const& rec = scopes_[scope.v];
    out.reserve(rec.bindings.size() + rec.tagBindings.size());
    // The string_view aliases the key stored in the scope record; valid
    // for as long as this ScopeTree lives, which outlives the injection
    // step that consumes it. Emit BOTH namespaces so the cross-tree
    // injection re-keys each binding on (name, namespace).
    for (auto const& [name, sym] : rec.bindings) {
        out.emplace_back(std::string_view{name}, SymbolNamespace::Ordinary, sym);
    }
    for (auto const& [name, sym] : rec.tagBindings) {
        out.emplace_back(std::string_view{name}, SymbolNamespace::Tag, sym);
    }
    return out;
}

} // namespace dss
