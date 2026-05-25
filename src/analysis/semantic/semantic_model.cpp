#include "analysis/semantic/semantic_model.hpp"

#include <cstdio>
#include <cstdlib>

namespace dss {

namespace {

[[noreturn]] void smFatal(char const* what) {
    std::fputs("dss::SemanticModel fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

} // namespace

ScopeRecord const& SemanticModel::scopeRecord(ScopeId id) const {
    if (!id.valid() || id.v >= scopes_.size()) {
        smFatal("scopeRecord: ScopeId out of range");
    }
    return scopes_[id.v];
}

SymbolRecord const* SemanticModel::recordFor(SymbolId id) const noexcept {
    if (!id.valid() || id.v >= symbols_.size()) return nullptr;
    return &symbols_[id.v];
}

SymbolId SemanticModel::symbolAt(NodeId id) const {
    auto const* p = nodeToSymbol_.tryGet(id);
    return p ? *p : InvalidSymbol;
}

TypeId SemanticModel::typeAt(NodeId id) const {
    auto const* p = nodeToType_.tryGet(id);
    return p ? *p : InvalidType;
}

} // namespace dss
