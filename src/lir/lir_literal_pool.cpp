#include "lir/lir_literal_pool.hpp"

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace dss {

namespace {

[[noreturn]] void litPoolFatal(char const* what) {
    std::fputs("dss::LirLiteralPool fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

} // namespace

std::uint32_t LirLiteralPool::add(LirLiteralValue v) {
    auto const idx = static_cast<std::uint32_t>(pool_.size());
    pool_.push_back(std::move(v));
    return idx;
}

LirLiteralValue const& LirLiteralPool::at(std::uint32_t index) const {
    if (index >= pool_.size()) litPoolFatal("LirLiteralPool::at: index out of range");
    return pool_[index];
}

} // namespace dss
