#pragma once

#include "core/export.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

// Heterogeneous string-key lookup support for `std::unordered_map`/`set`.
// Together with `TransparentStringEq`, allows callers to query a map by
// `std::string_view` (or `char const*`) without allocating a temporary
// `std::string` per lookup. Critical for hot paths.
//
// Current consumers (cycle 3a):
//   - `TargetSchema` opcode / register / calling-convention indexes (the
//     three identical maps these aliases collapse) — hit by cycle 3 isel
//     pattern-matching per emitted LIR instruction.
//
// Predicted next consumers: SE1's `SymbolTable` name lookup; HR8's
// `SchemaTokenInterner` reverse-lookup.
//
// Usage:
//   using NameIndex = std::unordered_map<std::string, V,
//       substrate::TransparentStringHash, substrate::TransparentStringEq>;
//   map.find(string_view{key});   // no allocation
//
// Convenience alias `TransparentStringMap<V>` provided for the common
// "string → V" shape; it is exactly the type the three TargetSchema
// index maps (mnemonic, register, calling-convention) collapse into.

namespace dss::substrate {

struct DSS_EXPORT TransparentStringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
    std::size_t operator()(std::string const& s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
    std::size_t operator()(char const* s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

struct DSS_EXPORT TransparentStringEq {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return a == b;
    }
};

template <class V>
using TransparentStringMap =
    std::unordered_map<std::string, V, TransparentStringHash, TransparentStringEq>;

} // namespace dss::substrate
