#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace dss {

// Two-axis classification consulted by a Pratt-style parser: the
// `(OperatorAssoc, OperatorArity)` pair selects how operand binding
// proceeds. The schema declares these via the `operators` JSON section;
// the table is populated only at load time and is read-only afterward.

enum class OperatorAssoc : std::uint8_t {
    None,
    Left,
    Right,
};

[[nodiscard]] constexpr std::string_view operatorAssocName(OperatorAssoc a) noexcept {
    switch (a) {
        case OperatorAssoc::None:  return "None";
        case OperatorAssoc::Left:  return "Left";
        case OperatorAssoc::Right: return "Right";
    }
    std::unreachable();
}

enum class OperatorArity : std::uint8_t {
    Infix,
    Prefix,
    Postfix,
};

[[nodiscard]] constexpr std::string_view operatorArityName(OperatorArity a) noexcept {
    switch (a) {
        case OperatorArity::Infix:   return "Infix";
        case OperatorArity::Prefix:  return "Prefix";
        case OperatorArity::Postfix: return "Postfix";
    }
    std::unreachable();
}

namespace detail {

[[noreturn]] inline void opTableFatal(char const* what) {
    std::fputs("dss::OperatorTable fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

} // namespace detail

// Maps `(SchemaTokenId, OperatorArity)` to `{ precedence, associativity }`.
// Higher precedence binds tighter, matching the conventional Pratt-table
// reading. A lexeme may appear with multiple arities (e.g. `-` as both
// Infix and Prefix); the `(id, arity)` key distinguishes them.
//
// Uniqueness contract: the loader rejects duplicate `(id, arity)` entries
// upstream with `C_InvalidPrecedenceTable`. `insert()` itself silently
// last-write-wins — the table is the wrong layer to surface the diagnostic
// (it has no JSON pointer to attach), so this header trusts the caller.
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

    // Last write wins (see uniqueness contract above).
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

    // Maximum SchemaTokenId value the key-packing scheme can represent.
    // 30 bits → ~1.07B distinct kinds. Real configs use a few hundred at
    // most; the ceiling is here so future fuzz-input or generated configs
    // can't silently corrupt the table.
    static constexpr std::uint32_t kMaxSchemaTokenIdValue = (1u << 30) - 1u;

private:
    // Packed key: 30 bits of SchemaTokenId value + 2 bits of arity. The
    // shift assumes `id.v <= kMaxSchemaTokenIdValue`; key() aborts via
    // opTableFatal if that's ever violated rather than silently aliasing
    // arities into the wrong slot.
    [[nodiscard]] static std::uint32_t key(SchemaTokenId id,
                                            OperatorArity arity) {
        if (id.v > kMaxSchemaTokenIdValue) {
            detail::opTableFatal("SchemaTokenId exceeds 30-bit key budget; cannot pack with arity");
        }
        return (id.v << 2) | static_cast<std::uint32_t>(arity);
    }

    std::unordered_map<std::uint32_t, Entry> entries_;
};

// ── Compile-time invariants ────────────────────────────────────────────────
//
// Enum widths are part of the key-packing contract: arity must fit in
// 2 bits, which means `OperatorArity` must have at most 4 variants and
// each variant's value must be < 4. `Entry` is a leaf POD; making it
// trivially-copyable lets `std::optional<Entry>` (returned by lookup)
// stay cheap.
static_assert(sizeof(OperatorAssoc) == 1,
              "OperatorAssoc must remain 1 byte to keep Entry compact");
static_assert(sizeof(OperatorArity) == 1,
              "OperatorArity must remain 1 byte to keep the packed key narrow");
static_assert(static_cast<std::uint8_t>(OperatorArity::Postfix) < 4,
              "OperatorArity must fit in 2 bits for the key-packing scheme");
static_assert(std::is_trivially_copyable_v<OperatorTable::Entry>,
              "OperatorTable::Entry must be trivially copyable");

} // namespace dss
