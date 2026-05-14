#include "core/types/rule_id.hpp"

#include <cassert>
#include <cstdint>
#include <string>
#include <utility>

namespace dss {

RuleInterner::RuleInterner() {
    // Reserve slot 0 for the InvalidRule sentinel. intern() never returns 0.
    names_.emplace_back();
}

RuleId RuleInterner::intern(std::string_view name) {
    if (auto it = lookup_.find(std::string{name}); it != lookup_.end()) {
        return it->second;
    }
    if (frozen_) {
        assert(false && "RuleInterner::intern called after freeze()");
        return InvalidRule;
    }
    const auto value = static_cast<std::uint32_t>(names_.size());
    names_.emplace_back(name);
    const RuleId id{value};
    lookup_.emplace(names_.back(), id);
    return id;
}

std::string_view RuleInterner::name(RuleId id) const noexcept {
    if (id.v >= names_.size()) return {};
    return names_[id.v];
}

bool RuleInterner::contains(std::string_view name) const {
    return lookup_.find(std::string{name}) != lookup_.end();
}

} // namespace dss
