#include "hir/lowering/hir_literal_pool.hpp"

#include <cstdio>
#include <cstdlib>

namespace dss {

std::uint32_t HirLiteralPool::add(HirLiteralValue v) {
    auto const idx = static_cast<std::uint32_t>(pool_.size());
    pool_.push_back(std::move(v));
    return idx;
}

HirLiteralValue const& HirLiteralPool::at(std::uint32_t index) const {
    if (index >= pool_.size()) {
        std::fprintf(stderr,
                     "dss::HirLiteralPool fatal: index %u out of range (size %zu)\n",
                     index, pool_.size());
        std::abort();
    }
    return pool_[index];
}

} // namespace dss
