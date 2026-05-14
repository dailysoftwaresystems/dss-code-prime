#pragma once

#include "core/export.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <type_traits>

namespace dss {

// Three essential structural states. Per-payload meaning (token kind vs rule
// vs error) is enforced by Tree's discriminant-asserting accessors — Node
// itself is Tree-private (see detail::Node below).
enum class NodeKind : std::uint8_t {
    Internal,   // grammar rule with children
    Token,      // leaf produced from a Token (resolved via SchemaTokenId)
    Error,      // parser error-recovery node (always carries a ParseDiagnostic)
};

// Orthogonal markers. Multiple flags can apply to one node.
//
// EmptySpace is *the* flag for "ignore this when walking" — AST cursor mode
// is one bit-test (isEmptySpace(flags)). Missing/Synthetic are still visible
// in AST mode (IR generation needs to see them to report missing tokens).
enum class NodeFlags : std::uint8_t {
    None       = 0,
    EmptySpace = 1u << 0,    // whitespace / comments / any ignorable run
    Missing    = 1u << 1,    // schema required this node but source didn't have it
    HasError   = 1u << 2,    // this node or some descendant is/contains an Error
    Synthetic  = 1u << 3,    // inserted by builder, not derived from source tokens
    // bits 4-7 reserved
};

// Inline constexpr free operators — header-only, zero cost. Not DSS_EXPORT
// because dllimport on free functions causes MSVC warnings and the existing
// scaffold only exports classes.
[[nodiscard]] inline constexpr NodeFlags operator|(NodeFlags a, NodeFlags b) noexcept {
    using U = std::underlying_type_t<NodeFlags>;
    return static_cast<NodeFlags>(static_cast<U>(a) | static_cast<U>(b));
}
[[nodiscard]] inline constexpr NodeFlags operator&(NodeFlags a, NodeFlags b) noexcept {
    using U = std::underlying_type_t<NodeFlags>;
    return static_cast<NodeFlags>(static_cast<U>(a) & static_cast<U>(b));
}
[[nodiscard]] inline constexpr NodeFlags operator~(NodeFlags v) noexcept {
    using U = std::underlying_type_t<NodeFlags>;
    return static_cast<NodeFlags>(~static_cast<U>(v));
}
inline constexpr NodeFlags& operator|=(NodeFlags& a, NodeFlags b) noexcept {
    a = a | b;
    return a;
}
inline constexpr NodeFlags& operator&=(NodeFlags& a, NodeFlags b) noexcept {
    a = a & b;
    return a;
}

// v != None — useful for `if (any(node.flags & EmptySpace)) ...`.
[[nodiscard]] inline constexpr bool any(NodeFlags v) noexcept {
    return static_cast<std::underlying_type_t<NodeFlags>>(v) != 0;
}
// has(v, bit) — equivalent to any(v & bit), reads more naturally for callers.
[[nodiscard]] inline constexpr bool has(NodeFlags v, NodeFlags bit) noexcept {
    return any(v & bit);
}
[[nodiscard]] inline constexpr bool isEmptySpace(NodeFlags v) noexcept {
    return has(v, NodeFlags::EmptySpace);
}
[[nodiscard]] inline constexpr bool hasError(NodeFlags v) noexcept {
    return has(v, NodeFlags::HasError);
}

// Storage POD. Lives inside detail/ so consumers MUST go through Tree's
// discriminant-asserting accessors — Node fields are not meaningful in
// isolation (e.g. rule is only valid when kind == Internal).
//
// Layout (40 bytes, 8-byte aligned): comfortably fits >1 node per 64-byte
// cacheline. The plan originally targeted 32 bytes, but the DiagnosticIndex
// added during the review pass brought it to 40; cacheline coverage is still
// excellent and shrinking further would force packing firstChild/childCount
// into 16 bits each (capping max children at 65 535, which we don't want).
namespace detail {

struct alignas(8) Node {
    NodeKind        kind         = NodeKind::Internal;
    NodeFlags       flags        = NodeFlags::None;
    std::uint16_t   _pad         = 0;            // explicit padding
    SchemaTokenId   tokenKind    = InvalidSchemaToken;   // meaningful when kind == Token
    RuleId          rule         = InvalidRule;          // meaningful when kind == Internal
    SourceSpan      span         = SourceSpan::empty(0);
    NodeId          parent       = InvalidNode;
    std::uint32_t   firstChild   = 0;            // offset into Tree::childIndex_
    std::uint32_t   childCount   = 0;            // consecutive children (0 for leaves)
    DiagnosticIndex diagnostic   = InvalidDiagnostic;    // valid() iff Error or HasError
};

static_assert(sizeof(Node) <= 40, "detail::Node grew unexpectedly — review layout");
static_assert(std::is_trivially_copyable_v<Node>);

} // namespace detail

} // namespace dss
