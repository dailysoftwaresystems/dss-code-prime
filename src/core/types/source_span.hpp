#pragma once

#include "core/export.hpp"

#include <compare>
#include <cstdint>

namespace dss {

// 4 GiB cap per source buffer — uint32_t offsets. Documented in plan §5.2.
using ByteOffset = std::uint32_t;

// Half-open byte range [start, end) within a single SourceBuffer.
// Construction goes through SourceSpan::of(start, end) which enforces start <= end
// (the factory-only pattern makes inverted spans a type-level impossibility).
class DSS_EXPORT SourceSpan {
public:
    [[nodiscard]] static SourceSpan of(ByteOffset start, ByteOffset end) noexcept;
    [[nodiscard]] static constexpr SourceSpan empty(ByteOffset at) noexcept { return SourceSpan{at, at}; }

    [[nodiscard]] constexpr ByteOffset start()  const noexcept { return start_; }
    [[nodiscard]] constexpr ByteOffset end()    const noexcept { return end_; }
    [[nodiscard]] constexpr ByteOffset length() const noexcept { return end_ - start_; }
    [[nodiscard]] constexpr bool       isEmpty() const noexcept { return end_ == start_; }

    [[nodiscard]] bool contains(ByteOffset off) const noexcept;
    [[nodiscard]] bool containsSpan(SourceSpan other) const noexcept;
    [[nodiscard]] bool overlaps(SourceSpan other) const noexcept;

    // Smallest span covering both. Empty operands are *ignored*, not
    // "merged at point zero" — load-bearing for synthetic Missing-node parents
    // whose only child has an empty span.
    [[nodiscard]] static SourceSpan join(SourceSpan a, SourceSpan b) noexcept;

    // Intersection; may be empty.
    [[nodiscard]] static SourceSpan intersect(SourceSpan a, SourceSpan b) noexcept;

    constexpr bool operator==(SourceSpan const&) const noexcept = default;
    constexpr auto operator<=>(SourceSpan const&) const noexcept = default;

private:
    constexpr SourceSpan(ByteOffset s, ByteOffset e) noexcept : start_(s), end_(e) {}
    ByteOffset start_ = 0;
    ByteOffset end_   = 0;
};
static_assert(sizeof(SourceSpan) == 8, "SourceSpan should stay 8 bytes");

} // namespace dss
