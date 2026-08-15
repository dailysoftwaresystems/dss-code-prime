#include "mir/mir_asm_descriptor.hpp"

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace dss {

std::uint32_t MirAsmDescriptorPool::add(MirAsmDescriptor d) {
    // The index is a uint32 instruction payload; a wrap would alias the wrong
    // descriptor — i.e. the wrong template and the wrong clobber list — silently.
    // Practically unreachable, but a standing fail-loud invariant
    // (`MirLiteralPool::add`'s precedent).
    if (pool_.size() >= std::numeric_limits<std::uint32_t>::max()) {
        std::fputs("dss::MirAsmDescriptorPool fatal: pool exhausted "
                   "(uint32 index overflow)\n",
                   stderr);
        std::abort();
    }
    auto const index = static_cast<std::uint32_t>(pool_.size());
    pool_.push_back(std::move(d));
    return index;
}

MirAsmDescriptor const& MirAsmDescriptorPool::at(std::uint32_t index) const {
    // Out of range means an instruction's payload outlived the pool it indexed —
    // the exact drop this pool's dedicated-builder refusal exists to prevent.
    // Aborting is the first line of defence here, not the second.
    if (index >= pool_.size()) {
        std::fprintf(stderr,
                     "dss::MirAsmDescriptorPool fatal: index %u out of range "
                     "(size %zu) — an InlineAsm payload survived a rebuild that "
                     "did not re-add its descriptor\n",
                     index, pool_.size());
        std::abort();
    }
    return pool_[index];
}

} // namespace dss
