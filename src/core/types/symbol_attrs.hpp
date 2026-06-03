#pragma once

// Canonical SymbolBinding / SymbolVisibility vocabulary — lifted from
// link/object_format_schema.hpp (which originally declared these) so
// MIR-tier producers can use them without a layer inversion. The
// link-tier header still includes this one + re-exports the names,
// keeping every existing consumer source-compatible.
//
// **Why MIR needs this** (D-OPT1-SYMBOL-BINDING-VISIBILITY-THREAD,
// step 13.6 OPT1 gate): plan 22 §2.9 requires DCE to NOT delete an
// externally-visible symbol. The link tier knows the binding/visibility
// per AssembledFunction, but MIR — where DCE actually runs — didn't.
// Threading these onto MirFunc + MirGlobal at HIR→MIR lowering time
// lets the optimizer's DCE pass consult the attributes directly,
// preserving every Global symbol (and every Default-visibility symbol
// that isn't Local) from elimination.
//
// **Closed enum, semantic only.** No bit-flag combinations; no
// numeric arithmetic. The 1-byte storage keeps MirFunc + MirGlobal
// under their 32-byte trivially-copyable POD budgets.

#include "core/types/target_schema.hpp"   // EnumNameTable<E,N>

#include <cstdint>
#include <optional>
#include <string_view>

namespace dss {

// Symbol binding — visibility within the linker's symbol-resolution
// algorithm. Local symbols never resolve across translation units;
// Weak symbols defer to Global symbols of the same name.
enum class SymbolBinding : std::uint8_t {
    Local  = 0,
    Global = 1,
    Weak   = 2,
};

inline constexpr EnumNameTable<SymbolBinding, 3> kSymbolBindingTable{{{
    { SymbolBinding::Local,  "local"  },
    { SymbolBinding::Global, "global" },
    { SymbolBinding::Weak,   "weak"   },
}}};

[[nodiscard]] constexpr std::string_view
symbolBindingName(SymbolBinding b) noexcept {
    return kSymbolBindingTable.name(b);
}
[[nodiscard]] constexpr std::optional<SymbolBinding>
symbolBindingFromName(std::string_view s) noexcept {
    return kSymbolBindingTable.fromName(s);
}

// Symbol visibility — affects whether a symbol is exported to other
// images at runtime. Default = exported (subject to binding).
enum class SymbolVisibility : std::uint8_t {
    Default   = 0,
    Hidden    = 1,
    Protected = 2,
    Internal  = 3,
};

inline constexpr EnumNameTable<SymbolVisibility, 4> kSymbolVisibilityTable{{{
    { SymbolVisibility::Default,   "default"   },
    { SymbolVisibility::Hidden,    "hidden"    },
    { SymbolVisibility::Protected, "protected" },
    { SymbolVisibility::Internal,  "internal"  },
}}};

[[nodiscard]] constexpr std::string_view
symbolVisibilityName(SymbolVisibility v) noexcept {
    return kSymbolVisibilityTable.name(v);
}
[[nodiscard]] constexpr std::optional<SymbolVisibility>
symbolVisibilityFromName(std::string_view s) noexcept {
    return kSymbolVisibilityTable.fromName(s);
}

// D-OPT1-SYMBOL-BINDING-VISIBILITY-THREAD invariant: a symbol whose
// `binding == Global` AND `visibility != Hidden` AND `visibility !=
// Internal` is externally observable — every later image (the linker,
// the dynamic loader, profile-guided optimization tools) may reference
// it by name. DCE / unused-symbol elimination MUST preserve such
// symbols even when no INTRA-MODULE use of them exists.
//
// `Weak` binding is treated as "preserve unless a Global definition
// of the same name supersedes it" — same DCE-protect treatment as
// Global at MIR tier; the linker resolves the supersede later.
//
// `Local` binding + any visibility = DCE-eligible (intra-module-only).
//
// The free function is THE source of truth for the optimizer; the
// MIR verifier's invariant rule + the link-tier emitter share it.
[[nodiscard]] constexpr bool
isExternallyVisible(SymbolBinding binding, SymbolVisibility visibility) noexcept {
    if (binding == SymbolBinding::Local) return false;
    if (visibility == SymbolVisibility::Hidden) return false;
    if (visibility == SymbolVisibility::Internal) return false;
    return true;  // Global+Default, Global+Protected, Weak+*-non-Hidden/Internal
}

} // namespace dss
