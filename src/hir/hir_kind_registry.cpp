#include "hir/hir_kind_registry.hpp"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

namespace dss {

HirKindId HirKindRegistry::registerExtension(std::string_view name,
                                             std::string_view sourceLanguage) {
    std::string key{name};
    if (auto it = byName_.find(key); it != byName_.end()) {
        // Idempotent re-declaration must agree on the owning language. A clash
        // means two domains tried to claim one HIR kind name — fail loud rather
        // than silently aliasing them onto the same kindId.
        HirKindDescriptor const& existing = extensions_[it->second.v - kFirstHirExtensionKind];
        if (existing.sourceLanguage() != sourceLanguage) {
            std::fprintf(stderr,
                         "dss::HirKindRegistry fatal: extension kind '%.*s' re-registered "
                         "under language '%.*s' but was first registered under '%.*s'\n",
                         static_cast<int>(existing.name().size()), existing.name().data(),
                         static_cast<int>(sourceLanguage.size()), sourceLanguage.data(),
                         static_cast<int>(existing.sourceLanguage().size()),
                         existing.sourceLanguage().data());
            std::abort();
        }
        return it->second;
    }

    // Overflow guard: wrapping `nextKind_` past UINT32_MAX would mint a kindId
    // that re-enters the [0, 256) core range AND has `valid() == false`
    // (HirKindId{0} == InvalidHirKind), silently corrupting downstream
    // dispatch. Practically unreachable, but a standing fail-loud invariant.
    if (nextKind_ == std::numeric_limits<std::uint32_t>::max()) {
        std::fputs("dss::HirKindRegistry fatal: extension kindId counter exhausted "
                   "(uint32 overflow)\n", stderr);
        std::abort();
    }

    HirKindId const kindId{nextKind_++};
    extensions_.emplace_back(HirKindDescriptor::MintToken{}, std::move(key), kindId,
                             std::string{sourceLanguage});
    byName_.emplace(std::string{extensions_.back().name()}, kindId);
    return kindId;
}

std::optional<HirKindId> HirKindRegistry::findExtension(std::string_view name) const {
    if (auto it = byName_.find(std::string{name}); it != byName_.end()) {
        return it->second;
    }
    return std::nullopt;
}

HirKindDescriptor const& HirKindRegistry::descriptor(HirKindId kind) const {
    if (kind.v < kFirstHirExtensionKind
        || (kind.v - kFirstHirExtensionKind) >= extensions_.size()) {
        std::fprintf(stderr,
                     "dss::HirKindRegistry fatal: descriptor() for HirKindId=%u this "
                     "registry never minted (core range is < %u)\n",
                     kind.v, kFirstHirExtensionKind);
        std::abort();
    }
    return extensions_[kind.v - kFirstHirExtensionKind];
}

} // namespace dss
