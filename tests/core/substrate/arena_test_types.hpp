#pragma once

// Two non-Tree arena type families for the substrate tests, proving
// ArenaContainer / ArenaBuilder / ArenaAttribute are not coupled to
// detail::Node / NodeId / TreeId. Each id mirrors NodeId's shape (index `.v`
// + provenance `.arenaTag`, equality on `.v` only) and each tag mirrors a
// DSS_STRONG_ID (just `.v`).

#include "core/substrate/arena_tag.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace dss::substrate_test {

// ── "shape" arena ──
struct ShapeId {
    std::uint32_t v       = 0;
    std::uint32_t arenaTag = 0;
    constexpr ShapeId() noexcept = default;
    constexpr explicit ShapeId(std::uint32_t value) noexcept : v(value) {}
    constexpr ShapeId(std::uint32_t value, std::uint32_t tag) noexcept : v(value), arenaTag(tag) {}
    [[nodiscard]] constexpr bool valid() const noexcept { return v != 0; }
    constexpr bool operator==(ShapeId const& o) const noexcept { return v == o.v; }
};
struct ShapeTag {
    std::uint32_t v = 0;
    constexpr ShapeTag() noexcept = default;
    constexpr explicit ShapeTag(std::uint32_t value) noexcept : v(value) {}
    [[nodiscard]] constexpr bool valid() const noexcept { return v != 0; }
};
struct ShapePod {
    int           kind = 0;
    std::uint32_t aux  = 0;
};

// ── "value" arena (a distinct pod + id family) ──
struct ValueId {
    std::uint32_t v       = 0;
    std::uint32_t arenaTag = 0;
    constexpr ValueId() noexcept = default;
    constexpr explicit ValueId(std::uint32_t value) noexcept : v(value) {}
    constexpr ValueId(std::uint32_t value, std::uint32_t tag) noexcept : v(value), arenaTag(tag) {}
    [[nodiscard]] constexpr bool valid() const noexcept { return v != 0; }
    constexpr bool operator==(ValueId const& o) const noexcept { return v == o.v; }
};
struct ValueTag {
    std::uint32_t v = 0;
    constexpr ValueTag() noexcept = default;
    constexpr explicit ValueTag(std::uint32_t value) noexcept : v(value) {}
    [[nodiscard]] constexpr bool valid() const noexcept { return v != 0; }
};
struct ValuePod {
    double weight = 0.0;
};

} // namespace dss::substrate_test

template <>
struct std::hash<dss::substrate_test::ShapeId> {
    std::size_t operator()(dss::substrate_test::ShapeId id) const noexcept {
        return std::hash<std::uint32_t>{}(id.v);
    }
};
template <>
struct std::hash<dss::substrate_test::ValueId> {
    std::size_t operator()(dss::substrate_test::ValueId id) const noexcept {
        return std::hash<std::uint32_t>{}(id.v);
    }
};

// ArenaNames specializations — required since the primary template is a
// must-specialize tripwire. These give the test arenas their own diagnostic
// wording (which the death-test regexes pin).
namespace dss::substrate {

template <>
struct ArenaNames<substrate_test::ShapeId, substrate_test::ShapeTag> {
    static constexpr char const* attribute = "ShapeAttr";
    static constexpr char const* element   = "ShapeId";
    static constexpr char const* tag       = "ShapeTag";
    static constexpr char const* access    = "ShapeArena::at";
};

template <>
struct ArenaNames<substrate_test::ValueId, substrate_test::ValueTag> {
    static constexpr char const* attribute = "ValueAttr";
    static constexpr char const* element   = "ValueId";
    static constexpr char const* tag       = "ValueTag";
    static constexpr char const* access    = "ValueArena::at";
};

} // namespace dss::substrate
