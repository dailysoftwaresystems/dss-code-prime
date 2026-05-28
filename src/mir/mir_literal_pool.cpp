#include "mir/mir_literal_pool.hpp"

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace dss {

std::uint32_t MirLiteralPool::add(MirLiteralValue v) {
    // The index is a uint32 `Const` payload; a wrap would alias the wrong value
    // silently. Practically unreachable, but a standing fail-loud invariant.
    if (pool_.size() >= std::numeric_limits<std::uint32_t>::max()) {
        std::fputs("dss::MirLiteralPool fatal: pool exhausted (uint32 index overflow)\n", stderr);
        std::abort();
    }
    auto const index = static_cast<std::uint32_t>(pool_.size());
    pool_.push_back(std::move(v));
    return index;
}

MirLiteralValue const& MirLiteralPool::at(std::uint32_t index) const {
    if (index >= pool_.size()) {
        std::fprintf(stderr,
                     "dss::MirLiteralPool fatal: index %u out of range (size %zu)\n",
                     index, pool_.size());
        std::abort();
    }
    return pool_[index];
}

} // namespace dss
