#include "hir/hir_intrinsic_registry.hpp"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

namespace dss {

HirIntrinsicId HirIntrinsicRegistry::registerIntrinsic(std::string_view name,
                                                       std::string_view sourceLanguage) {
    std::string key{name};
    if (auto it = byName_.find(key); it != byName_.end()) {
        // Idempotent re-declaration must agree on the owning language. A clash
        // means two domains tried to claim one intrinsic name — fail loud rather
        // than silently aliasing them onto the same id.
        HirIntrinsicDescriptor const& existing = intrinsics_[it->second.v - 1];
        if (existing.sourceLanguage() != sourceLanguage) {
            std::fprintf(stderr,
                         "dss::HirIntrinsicRegistry fatal: intrinsic '%.*s' re-registered "
                         "under language '%.*s' but was first registered under '%.*s'\n",
                         static_cast<int>(existing.name().size()), existing.name().data(),
                         static_cast<int>(sourceLanguage.size()), sourceLanguage.data(),
                         static_cast<int>(existing.sourceLanguage().size()),
                         existing.sourceLanguage().data());
            std::abort();
        }
        return it->second;
    }

    // Overflow guard: wrapping `nextId_` past UINT32_MAX would mint id 0
    // (== InvalidHirIntrinsic), silently corrupting downstream resolution.
    // Practically unreachable, but a standing fail-loud invariant.
    if (nextId_ == std::numeric_limits<std::uint32_t>::max()) {
        std::fputs("dss::HirIntrinsicRegistry fatal: intrinsic id counter exhausted "
                   "(uint32 overflow)\n", stderr);
        std::abort();
    }

    HirIntrinsicId const id{nextId_++};
    intrinsics_.emplace_back(HirIntrinsicDescriptor::MintToken{}, std::move(key), id,
                             std::string{sourceLanguage});
    byName_.emplace(std::string{intrinsics_.back().name()}, id);
    return id;
}

std::optional<HirIntrinsicId> HirIntrinsicRegistry::findIntrinsic(std::string_view name) const {
    if (auto it = byName_.find(std::string{name}); it != byName_.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool HirIntrinsicRegistry::contains(HirIntrinsicId id) const noexcept {
    // Ids are minted 1..N, stored at index id.v - 1; a valid id is in [1, size].
    return id.valid() && id.v <= intrinsics_.size();
}

HirIntrinsicDescriptor const& HirIntrinsicRegistry::descriptor(HirIntrinsicId id) const {
    if (!contains(id)) {
        std::fprintf(stderr,
                     "dss::HirIntrinsicRegistry fatal: descriptor() for HirIntrinsicId=%u "
                     "this registry never minted\n",
                     id.v);
        std::abort();
    }
    return intrinsics_[id.v - 1];
}

} // namespace dss
