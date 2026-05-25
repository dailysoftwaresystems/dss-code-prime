#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

// Cross-arena guard primitive shared by ArenaContainer (element access) and
// ArenaAttribute (side-table access). Both stamp a per-arena tag onto every id
// and validate it on every access — an id from arena A used against arena B
// aborts loudly. This is the generalization of Tree's SH3 `treeTag` discipline.
//
// Fatal-message wording is driven by `ArenaNames<Id, Tag>` so each domain
// reproduces its own diagnostics. The primary template gives generic names;
// domains specialize it (e.g. `tree.hpp` specializes <NodeId, TreeId> to the
// exact "NodeAttribute … TreeId … NodeId … Tree::node_" strings the existing
// Tree death tests pin).

namespace dss::substrate {

template <class Id, class Tag>
struct ArenaNames {
    static constexpr char const* attribute = "ArenaAttribute";
    static constexpr char const* element   = "id";
    static constexpr char const* tag       = "ArenaTag";
    static constexpr char const* access    = "ArenaContainer::at";
};

namespace detail::arena {

// ── ArenaContainer element-access fatals ──

[[noreturn]] inline void outOfRange(char const* access, char const* element) {
    std::fprintf(stderr, "dss::substrate fatal: %s: %s out of range\n", access, element);
    std::abort();
}

[[noreturn]] inline void crossArenaAccess(char const* access, char const* element, char const* tag,
                                          std::uint32_t idTag, std::uint32_t arenaTag) {
    std::fprintf(stderr, "dss::substrate fatal: %s: %s from %s=%u used on %s=%u\n",
                 access, element, tag, idTag, tag, arenaTag);
    std::abort();
}

// ── ArenaAttribute side-table fatals ──

[[noreturn]] inline void attrInvalidSentinel(char const* attribute, char const* element) {
    std::fprintf(stderr, "dss::%s fatal: invalid %s (%s{} sentinel)\n", attribute, element, element);
    std::abort();
}

[[noreturn]] inline void attrOutOfBounds(char const* attribute, char const* element) {
    std::fprintf(stderr, "dss::%s fatal: %s out of bounds for bound arena\n", attribute, element);
    std::abort();
}

[[noreturn]] inline void attrNoValue(char const* attribute, char const* element) {
    std::fprintf(stderr, "dss::%s fatal: get: no value for %s\n", attribute, element);
    std::abort();
}

// Emits both tags so death-test regexes can pin both numbers.
[[noreturn]] inline void attrCrossArena(char const* attribute, char const* element, char const* tag,
                                        std::uint32_t boundTag, std::uint32_t idTag) {
    std::fprintf(stderr, "dss::%s fatal: %s bound to %s=%u got %s from %s=%u\n",
                 attribute, attribute, tag, boundTag, element, tag, idTag);
    std::abort();
}

[[noreturn]] inline void truncateOutOfRange(char const* access) {
    std::fprintf(stderr, "dss::substrate fatal: %s: truncateTo size out of range\n", access);
    std::abort();
}

// The element-access guard shared by ArenaContainer::at and ArenaBuilder::at:
// bounds first, then the cross-arena tag check (untagged ids — treeTag == 0 —
// pass, preserving literal-id test ergonomics). `Names` supplies the wording.
template <class Names, class Id, class Tag>
inline void validateElement(Id id, Tag tag, std::size_t count) {
    if (!id.valid() || id.v >= count) {
        outOfRange(Names::access, Names::element);
    }
    if (id.treeTag != 0 && id.treeTag != tag.v) {
        crossArenaAccess(Names::access, Names::element, Names::tag, id.treeTag, tag.v);
    }
}

} // namespace detail::arena

} // namespace dss::substrate
