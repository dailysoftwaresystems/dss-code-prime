#pragma once

#include "core/export.hpp"

#include <compare>
#include <cstdint>

namespace dss {

class GrammarSchema;   // friend

// Opaque cursor into a compiled GrammarSchema shape graph. Trivially
// copyable, passed by value through the builder. Construction is
// schema-internal — `GrammarSchema::rootCursor()` returns the only
// publicly-reachable starting state; subsequent positions come from
// `advance()` / `enterRule()` / `leaveRule()`.
//
// A default-constructed cursor is invalid (valid() == false).
class DSS_EXPORT SchemaCursor {
public:
    constexpr SchemaCursor() noexcept = default;

    [[nodiscard]] constexpr bool valid() const noexcept { return shapeId_ != 0; }

    constexpr bool operator==(SchemaCursor const&) const noexcept = default;
    constexpr auto operator<=>(SchemaCursor const&) const noexcept = default;

    // Public accessors for tests / debugging only; the schema is the only
    // entity that interprets these.
    [[nodiscard]] constexpr std::uint32_t shapeId()   const noexcept { return shapeId_; }
    [[nodiscard]] constexpr std::uint32_t position()  const noexcept { return position_; }
    [[nodiscard]] constexpr std::uint32_t parentRet() const noexcept { return parentRet_; }
    [[nodiscard]] constexpr std::uint16_t altIndex()  const noexcept { return altIndex_; }

private:
    friend class GrammarSchema;

    constexpr SchemaCursor(std::uint32_t shape, std::uint32_t pos,
                           std::uint32_t parent = 0, std::uint16_t alt = 0) noexcept
        : shapeId_(shape), position_(pos), parentRet_(parent), altIndex_(alt) {}

    std::uint32_t shapeId_   = 0;   // index into schema's compiled shape table; 0 = invalid
    std::uint32_t position_  = 0;   // step within the shape's production
    std::uint32_t parentRet_ = 0;   // saved return position for nested rule descent
    std::uint16_t altIndex_  = 0;   // which alternative was taken (for diagnostics)
    std::uint16_t _pad_      = 0;
};
static_assert(sizeof(SchemaCursor) == 16, "SchemaCursor target size: 16 bytes");

} // namespace dss
