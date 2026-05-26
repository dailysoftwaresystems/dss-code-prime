#include "hir/hir_op_registry.hpp"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

namespace dss {

HirOpId HirOpRegistry::registerExtension(std::string_view name, HirOpArity arity,
                                         std::string_view sourceLanguage) {
    std::string key{name};
    if (auto it = byName_.find(key); it != byName_.end()) {
        // Idempotent re-declaration must agree on the owning language AND arity.
        // A clash means two domains tried to claim one operator name, or one
        // domain disagreed with itself on arity — fail loud rather than silently
        // aliasing them onto the same opId.
        HirOpDescriptor const& existing = extensions_[it->second.v - kFirstHirExtensionOp];
        if (existing.sourceLanguage() != sourceLanguage) {
            std::fprintf(stderr,
                         "dss::HirOpRegistry fatal: extension operator '%.*s' re-registered "
                         "under language '%.*s' but was first registered under '%.*s'\n",
                         static_cast<int>(existing.name().size()), existing.name().data(),
                         static_cast<int>(sourceLanguage.size()), sourceLanguage.data(),
                         static_cast<int>(existing.sourceLanguage().size()),
                         existing.sourceLanguage().data());
            std::abort();
        }
        if (existing.arity() != arity) {
            std::fprintf(stderr,
                         "dss::HirOpRegistry fatal: extension operator '%.*s' re-registered "
                         "as %s but was first registered as %s (arity is part of operator "
                         "identity)\n",
                         static_cast<int>(existing.name().size()), existing.name().data(),
                         arityLabel(arity), arityLabel(existing.arity()));
            std::abort();
        }
        return it->second;
    }

    // Overflow guard: wrapping `nextOp_` past UINT32_MAX would mint an opId that
    // re-enters the [0, 256) core range AND has `valid() == false`
    // (HirOpId{0} == InvalidHirOp), silently corrupting downstream dispatch.
    // Practically unreachable, but a standing fail-loud invariant.
    if (nextOp_ == std::numeric_limits<std::uint32_t>::max()) {
        std::fputs("dss::HirOpRegistry fatal: extension opId counter exhausted "
                   "(uint32 overflow)\n", stderr);
        std::abort();
    }

    HirOpId const opId{nextOp_++};
    extensions_.emplace_back(HirOpDescriptor::MintToken{}, std::move(key), opId, arity,
                             std::string{sourceLanguage});
    byName_.emplace(std::string{extensions_.back().name()}, opId);
    return opId;
}

std::optional<HirOpId> HirOpRegistry::findExtension(std::string_view name) const {
    if (auto it = byName_.find(std::string{name}); it != byName_.end()) {
        return it->second;
    }
    return std::nullopt;
}

HirOpDescriptor const& HirOpRegistry::descriptor(HirOpId op) const {
    if (op.v < kFirstHirExtensionOp
        || (op.v - kFirstHirExtensionOp) >= extensions_.size()) {
        std::fprintf(stderr,
                     "dss::HirOpRegistry fatal: descriptor() for HirOpId=%u this "
                     "registry never minted (core range is < %u)\n",
                     op.v, kFirstHirExtensionOp);
        std::abort();
    }
    return extensions_[op.v - kFirstHirExtensionOp];
}

} // namespace dss
