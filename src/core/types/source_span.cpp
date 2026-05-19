#include "core/types/source_span.hpp"

#include <algorithm>
#include <cassert>

namespace dss {

SourceSpan SourceSpan::of(ByteOffset start, ByteOffset end) noexcept {
    // Debug-asserts on inverted spans. In release, clamp: end := max(start, end).
    // The factory pattern is the only way to construct a SourceSpan; this
    // makes "start > end" representable only via a deliberate bug, not by
    // forgetting to validate at a call site.
    assert(start <= end && "SourceSpan::of received inverted span");
    return SourceSpan{start, std::max(start, end)};
}

bool SourceSpan::contains(ByteOffset off) const noexcept {
    return off >= start_ && off < end_;
}

bool SourceSpan::containsSpan(SourceSpan other) const noexcept {
    // The empty span is trivially contained by any span that covers its insertion point.
    if (other.isEmpty()) {
        return other.start_ >= start_ && other.start_ <= end_;
    }
    return other.start_ >= start_ && other.end_ <= end_;
}

bool SourceSpan::overlaps(SourceSpan other) const noexcept {
    // Empty spans don't overlap anything in the conventional sense.
    if (isEmpty() || other.isEmpty()) return false;
    return start_ < other.end_ && other.start_ < end_;
}

SourceSpan SourceSpan::join(SourceSpan a, SourceSpan b) noexcept {
    // Empty operands are ignored. Parent span computation in the tree
    // builder relies on this for synthetic Missing-node children.
    if (a.isEmpty()) return b;
    if (b.isEmpty()) return a;
    return SourceSpan{std::min(a.start_, b.start_), std::max(a.end_, b.end_)};
}

SourceSpan SourceSpan::intersect(SourceSpan a, SourceSpan b) noexcept {
    const ByteOffset s = std::max(a.start_, b.start_);
    const ByteOffset e = std::min(a.end_, b.end_);
    return (s <= e) ? SourceSpan{s, e} : SourceSpan{s, s};
}

} // namespace dss
