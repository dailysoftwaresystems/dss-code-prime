#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dss {

// Interns grammar rule names from the language config ("functionDecl", "ifStmt",
// ...) into stable RuleId values. Comparing RuleId == RuleId is one cmp;
// comparing strings would be cache-poor. Centralized so multiple trees built
// from the same language share the same ids.
//
// Slot 0 is reserved as the InvalidRule sentinel — intern() starts assigning
// at 1, and name(InvalidRule) returns "".
//
// After freeze() the interner refuses new entries (debug-asserts; release
// returns InvalidRule). GrammarSchema::load*() calls freeze() once schema
// build is complete, locking in the rule-name namespace for the rest of the
// program's lifetime.
class DSS_EXPORT RuleInterner {
public:
    RuleInterner();

    // Returns the id for `name`, creating a new entry if needed.
    // After freeze(), returns InvalidRule for previously-unknown names.
    [[nodiscard]] RuleId intern(std::string_view name);

    // "" for InvalidRule; the original name otherwise.
    [[nodiscard]] std::string_view name(RuleId id) const noexcept;

    // True if `name` has been interned (including the reserved slot 0 = "").
    [[nodiscard]] bool contains(std::string_view name) const;

    // Total entries including the reserved slot 0.
    [[nodiscard]] std::size_t size() const noexcept { return names_.size(); }

    // Iteration over the name table. Order is insertion order; index = RuleId value.
    using const_iterator = std::vector<std::string>::const_iterator;
    [[nodiscard]] const_iterator begin() const noexcept { return names_.begin(); }
    [[nodiscard]] const_iterator end()   const noexcept { return names_.end(); }

    // After freeze(), intern() refuses new entries. Idempotent.
    void freeze() noexcept { frozen_ = true; }
    [[nodiscard]] bool isFrozen() const noexcept { return frozen_; }

private:
    std::vector<std::string>                names_;     // index = RuleId value; names_[0] == ""
    std::unordered_map<std::string, RuleId> lookup_;    // does NOT contain the empty sentinel
    bool                                    frozen_ = false;
};

} // namespace dss
