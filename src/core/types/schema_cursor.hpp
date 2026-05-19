#pragma once

#include "core/export.hpp"
#include "core/types/rule_id.hpp"

#include <compare>
#include <cstdint>

namespace dss {

class GrammarSchema;   // friend

// Opaque cursor into a compiled GrammarSchema shape graph. Trivially
// copyable, passed by value. The cursor represents one position in one
// rule's body — descent into nested rules is caller-managed via a stack
// of cursors. The schema methods `enterRule` / `leaveRule` / `advance`
// each return a new cursor; nothing about the schema-side state is
// mutable through this type.
//
// A default-constructed cursor is invalid (`valid() == false`). The only
// publicly-reachable starting state is `GrammarSchema::rootCursor()`.

class DSS_EXPORT SchemaCursor {
public:
    constexpr SchemaCursor() noexcept = default;

    [[nodiscard]] constexpr bool valid() const noexcept {
        // posId == 0 is the per-rule sentinel "invalid position".
        return rule_.v != 0 && posId_ != 0;
    }

    constexpr bool operator==(SchemaCursor const&) const noexcept = default;
    constexpr auto operator<=>(SchemaCursor const&) const noexcept = default;

    [[nodiscard]] constexpr RuleId        rule()  const noexcept { return rule_; }
    [[nodiscard]] constexpr std::uint32_t posId() const noexcept { return posId_; }

private:
    friend class GrammarSchema;

    constexpr SchemaCursor(RuleId rule, std::uint32_t pos) noexcept
        : rule_(rule), posId_(pos) {}

    RuleId        rule_;          // current rule; default-constructed RuleId is invalid
    std::uint32_t posId_  = 0;    // position-id within the rule's compiled positions table; 0 = invalid
    std::uint64_t _pad_   = 0;    // reserve room for future fields without breaking the 16-byte budget
};

static_assert(sizeof(SchemaCursor) == 16, "SchemaCursor target size: 16 bytes");

} // namespace dss
