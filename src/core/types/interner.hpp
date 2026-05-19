#pragma once

#include "core/export.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Generic string-interner parameterized by a DSS strong id type
// (NodeId / RuleId / SchemaTokenId / ...). Slot 0 is reserved as the
// invalid-id sentinel — intern() starts assigning at 1, and name() of
// the default id returns "".
//
// `RuleInterner` and `SchemaTokenInterner` are using-aliases of this
// template (see rule_id.hpp, schema_token_interner.hpp). When we add an
// identifier interner later (plan §9), it's a one-line addition.

namespace dss {

namespace detail {

// Transparent hasher/equality so `unordered_map<string, ...>` can be
// queried with a `string_view` without allocating an intermediate
// `string`. Enabled by providing a nested `is_transparent` tag — C++20+
// container behavior.
struct StringHash {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
    [[nodiscard]] std::size_t operator()(std::string const& s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
    [[nodiscard]] std::size_t operator()(char const* s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

} // namespace detail

template <typename Id>
class Interner {
public:
    Interner() {
        // Slot 0 is the invalid sentinel; intern() never returns 0.
        names_.emplace_back();
    }

    // Returns the id for `name`, creating a new entry if needed.
    // After freeze() the interner refuses new entries (debug-asserts;
    // release returns the invalid sentinel — Id{}).
    [[nodiscard]] Id intern(std::string_view name) {
        if (auto it = lookup_.find(name); it != lookup_.end()) {
            return it->second;
        }
        if (frozen_) {
            assert(false && "Interner::intern called after freeze()");
            return Id{};
        }
        const auto value = static_cast<std::uint32_t>(names_.size());
        names_.emplace_back(name);
        const Id id{value};
        lookup_.emplace(names_.back(), id);
        return id;
    }

    [[nodiscard]] std::string_view name(Id id) const noexcept {
        if (id.v >= names_.size()) return {};
        return names_[id.v];
    }

    [[nodiscard]] bool contains(std::string_view name) const {
        return lookup_.find(name) != lookup_.end();
    }

    // Const lookup — returns Id{} (invalid) when the name isn't interned.
    // Use this in read-only paths (e.g. after the interner is frozen)
    // instead of intern(), which requires non-const access.
    [[nodiscard]] Id find(std::string_view name) const {
        auto it = lookup_.find(name);
        return (it == lookup_.end()) ? Id{} : it->second;
    }

    [[nodiscard]] std::size_t size() const noexcept { return names_.size(); }

    using const_iterator = typename std::vector<std::string>::const_iterator;
    [[nodiscard]] const_iterator begin() const noexcept { return names_.begin(); }
    [[nodiscard]] const_iterator end()   const noexcept { return names_.end(); }

    void freeze() noexcept                 { frozen_ = true; }
    [[nodiscard]] bool isFrozen() const noexcept { return frozen_; }

private:
    std::vector<std::string>             names_;     // index = Id::v; names_[0] == ""
    // Transparent hasher + heterogeneous equality (`std::equal_to<>`) so
    // string_view queries don't allocate a temporary std::string. Hot
    // path: every Word-kind pushToken does a find() for "Identifier".
    std::unordered_map<std::string, Id,
                       detail::StringHash, std::equal_to<>>  lookup_;
    bool                                 frozen_ = false;
};

} // namespace dss
