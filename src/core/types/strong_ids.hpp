#pragma once

#include "core/export.hpp"

#include <compare>
#include <cstdint>
#include <functional>

// Strongly-typed integer ids.
//
// Bare uint32_t for every domain id (NodeId, RuleId, BufferId, ...) silently
// allows e.g. tree.children(someRuleId) to compile — and attribute tables
// keyed by Tree A's NodeId queried with Tree B's value to return wrong data.
//
// Each id below is a distinct struct with explicit-only construction and a
// valid() predicate (value 0 == invalid sentinel). Zero overhead vs uint32_t.

namespace dss {

// Default-constructed value is the invalid sentinel (v == 0).
#define DSS_STRONG_ID(Name)                                                  \
    struct Name {                                                            \
        constexpr Name() noexcept = default;                                 \
        constexpr explicit Name(std::uint32_t value) noexcept : v(value) {}  \
        std::uint32_t v = 0;                                                 \
        [[nodiscard]] constexpr bool valid() const noexcept { return v != 0; } \
        constexpr bool operator==(Name const&) const = default;              \
        constexpr auto operator<=>(Name const&) const = default;             \
    }

DSS_STRONG_ID(NodeId);
DSS_STRONG_ID(RuleId);
DSS_STRONG_ID(SchemaTokenId);
DSS_STRONG_ID(BufferId);
DSS_STRONG_ID(TreeId);
DSS_STRONG_ID(DiagnosticIndex);
DSS_STRONG_ID(LexerModeId);

#undef DSS_STRONG_ID

inline constexpr NodeId          InvalidNode{};
inline constexpr RuleId          InvalidRule{};
inline constexpr SchemaTokenId   InvalidSchemaToken{};
inline constexpr BufferId        InvalidBuffer{};
inline constexpr TreeId          InvalidTree{};
inline constexpr DiagnosticIndex InvalidDiagnostic{};
inline constexpr LexerModeId     InvalidLexerMode{};

} // namespace dss

// std::hash specializations live outside namespace dss.
#define DSS_HASH_ID(Name)                                                    \
    template <> struct std::hash<dss::Name> {                                \
        std::size_t operator()(dss::Name id) const noexcept {                \
            return std::hash<std::uint32_t>{}(id.v);                         \
        }                                                                    \
    }

DSS_HASH_ID(NodeId);
DSS_HASH_ID(RuleId);
DSS_HASH_ID(SchemaTokenId);
DSS_HASH_ID(BufferId);
DSS_HASH_ID(TreeId);
DSS_HASH_ID(DiagnosticIndex);
DSS_HASH_ID(LexerModeId);

#undef DSS_HASH_ID
