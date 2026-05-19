#include "core/types/scope_kind.hpp"

namespace dss {

std::string_view scopeName(ScopeKind kind) noexcept {
    switch (kind) {
        case ScopeKind::None:    return "None";
        case ScopeKind::Root:    return "Root";
        case ScopeKind::Block:   return "Block";
        case ScopeKind::Paren:   return "Paren";
        case ScopeKind::Bracket: return "Bracket";
        case ScopeKind::Generic: return "Generic";
        case ScopeKind::String:  return "String";
        case ScopeKind::Comment: return "Comment";
    }
    // Language-specific scope ids (>= 1024) and any future built-ins not
    // listed above fall through here. Return a stable opaque label so the
    // formatter has something printable; GrammarSchema-aware tooling can
    // resolve the real name itself.
    return "Custom";
}

} // namespace dss
