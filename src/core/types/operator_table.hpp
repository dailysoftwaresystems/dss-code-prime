#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace dss {

// Two-axis classification for an operator declared in the schema's
// `operators` table. The pair `(OperatorAssoc, OperatorArity)` is what
// a Pratt-style parser consults — but the parser does not live in this
// repo yet (v2 PR1 lands the data; parser is a parent-plan deliverable).

enum class OperatorAssoc : std::uint8_t {
    None,
    Left,
    Right,
};

[[nodiscard]] inline std::string_view operatorAssocName(OperatorAssoc a) noexcept {
    switch (a) {
        case OperatorAssoc::None:  return "None";
        case OperatorAssoc::Left:  return "Left";
        case OperatorAssoc::Right: return "Right";
    }
    return "Unknown";
}

enum class OperatorArity : std::uint8_t {
    Infix,
    Prefix,
    Postfix,
};

[[nodiscard]] inline std::string_view operatorArityName(OperatorArity a) noexcept {
    switch (a) {
        case OperatorArity::Infix:   return "Infix";
        case OperatorArity::Prefix:  return "Prefix";
        case OperatorArity::Postfix: return "Postfix";
    }
    return "Unknown";
}

// Maps `(SchemaTokenId, OperatorArity)` to `{ precedence, associativity }`.
// Higher precedence binds tighter, matching the conventional Pratt-table
// reading. A lexeme may appear with multiple arities (e.g. `-` as both
// Infix and Prefix); the `(id, arity)` key distinguishes them.
//
// Populated only by `grammar_schema_json.cpp` at load time; once a
// GrammarSchema is published the table is read-only.
class DSS_EXPORT OperatorTable {
public:
    struct Entry {
        std::int32_t  precedence    = 0;
        OperatorAssoc associativity = OperatorAssoc::None;
    };

    OperatorTable()                                = default;
    OperatorTable(OperatorTable const&)            = default;
    OperatorTable(OperatorTable&&) noexcept        = default;
    OperatorTable& operator=(OperatorTable const&) = default;
    OperatorTable& operator=(OperatorTable&&) noexcept = default;

    void insert(SchemaTokenId id, OperatorArity arity, Entry entry) {
        entries_.insert_or_assign(key(id, arity), entry);
    }

    [[nodiscard]] std::optional<Entry> lookup(SchemaTokenId id,
                                              OperatorArity arity) const noexcept {
        const auto it = entries_.find(key(id, arity));
        if (it == entries_.end()) return std::nullopt;
        return it->second;
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool        empty() const noexcept { return entries_.empty(); }

private:
    // Packed key: 30 bits of SchemaTokenId value + 2 bits of arity.
    // SchemaTokenId is a u32 strong-id wrapping a uint32_t counter that
    // starts at 1; the schema never reaches anywhere near 2^30 unique
    // token kinds. Compact key keeps the map dense in cache.
    [[nodiscard]] static constexpr std::uint32_t key(SchemaTokenId id,
                                                     OperatorArity arity) noexcept {
        return (id.v << 2) | static_cast<std::uint32_t>(arity);
    }

    std::unordered_map<std::uint32_t, Entry> entries_;
};

} // namespace dss
