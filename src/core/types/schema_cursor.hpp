#pragma once

#include "core/export.hpp"
#include "core/types/rule_id.hpp"

#include <compare>
#include <cstdint>
#include <type_traits>

namespace dss {

class GrammarSchema;   // friend

// Opaque cursor into a compiled GrammarSchema shape graph. Represents one
// position in one rule's body; descent into nested rules is caller-managed
// via a stack of cursors. The schema methods `enterRule` / `leaveRule` /
// `advance` each return a new cursor; nothing mutates through this type.
//
// `posId == 0` is the per-rule invalid sentinel; default-construction
// yields an invalid cursor. The only publicly-reachable starting state is
// `GrammarSchema::rootCursor()`.

class DSS_EXPORT SchemaCursor {
public:
    constexpr SchemaCursor() noexcept = default;

    [[nodiscard]] constexpr bool valid() const noexcept {
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
};

static_assert(sizeof(SchemaCursor) == 8,
              "SchemaCursor target size: 8 bytes (4 ruleId + 4 posId)");
static_assert(std::is_trivially_copyable_v<SchemaCursor>,
              "SchemaCursor must be trivially copyable for value-pass semantics");

} // namespace dss
