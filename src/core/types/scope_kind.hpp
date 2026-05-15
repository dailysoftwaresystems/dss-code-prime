#pragma once

#include "core/export.hpp"

#include <cstdint>
#include <string_view>

namespace dss {

// Stable, language-agnostic scope categories. Multiple may be active
// simultaneously on the builder's scope stack — the schema decides which
// tokens open/close which scopes.
//
// Values 0..255      reserved for built-in shared scopes (this enum).
// Values 256..1023   reserved for future shared additions.
// Values 1024..      reserved for *language-specific* scopes named by the
//                    GrammarSchema (kept as opaque ids; only the schema
//                    knows their names).
enum class ScopeKind : std::uint16_t {
    None    = 0,
    Root,
    Block,
    Paren,
    Bracket,
    Generic,
    String,
    Comment,
};

// Human-readable name of a built-in scope. Returns "Custom(<id>)" for
// language-specific scopes (id >= 1024) — the schema is the only entity
// that knows their proper names; the formatter falls back gracefully.
[[nodiscard]] DSS_EXPORT std::string_view scopeName(ScopeKind kind) noexcept;

} // namespace dss
