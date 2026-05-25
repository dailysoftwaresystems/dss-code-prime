#pragma once

#include "core/export.hpp"

#include <compare>
#include <cstdint>
#include <functional>

// Strongly-typed integer ids.
//
// Bare uint32_t for every domain id (NodeId, RuleId, BufferId, ...) silently
// allows e.g. tree.children(someRuleId) to compile — and attribute tables
// keyed by Tree A's NodeId queried with Tree B's value to return wrong data.
//
// Each id below is a distinct struct with explicit-only construction and a
// valid() predicate (value 0 == invalid sentinel). Zero overhead vs uint32_t.

namespace dss {

// Default-constructed value is the invalid sentinel (v == 0).
#define DSS_STRONG_ID(Name)                                                  \
    struct Name {                                                            \
        constexpr Name() noexcept = default;                                 \
        constexpr explicit Name(std::uint32_t value) noexcept : v(value) {}  \
        std::uint32_t v = 0;                                                 \
        [[nodiscard]] constexpr bool valid() const noexcept { return v != 0; } \
        constexpr bool operator==(Name const&) const = default;              \
        constexpr auto operator<=>(Name const&) const = default;             \
    }

// Arena-element id: an arena index `v` plus a provenance tag `treeTag` (the
// owning arena's tag). `treeTag == 0` is the "untagged" sentinel — a literal
// `Name{3}` constructed in tests passes the validators, while a tagged id
// obtained from one arena and handed to another aborts with both tags in the
// message (the SH3 cross-arena guard, generalized in src/core/substrate/).
//
// Equality and ordering compare `.v` ONLY; `treeTag` is provenance metadata,
// not identity, so same-slot ids from different arenas compare equal and the
// validators (not equality) are the enforcement point. This is the shape
// `substrate::ArenaContainer`/`ArenaBuilder` require of their id parameter.
//
// (The field is named `treeTag` for its origin on Tree's NodeId; for other
// arenas it carries that arena's tag — e.g. the owning CompilationUnitId for
// TypeId. A rename to a neutral name is tracked in 08.5 as an HIR-era cleanup.)
#define DSS_ARENA_ID(Name)                                                   \
    struct Name {                                                            \
        constexpr Name() noexcept = default;                                 \
        constexpr explicit Name(std::uint32_t value) noexcept : v(value) {}  \
        constexpr Name(std::uint32_t value, std::uint32_t tag) noexcept      \
            : v(value), treeTag(tag) {}                                      \
        std::uint32_t v       = 0;                                           \
        std::uint32_t treeTag = 0;                                           \
        [[nodiscard]] constexpr bool valid() const noexcept { return v != 0; } \
        constexpr bool operator==(Name const& o) const noexcept { return v == o.v; } \
        constexpr auto operator<=>(Name const& o) const noexcept { return v <=> o.v; } \
    }

DSS_STRONG_ID(RuleId);
DSS_STRONG_ID(SchemaTokenId);
DSS_STRONG_ID(BufferId);
DSS_STRONG_ID(TreeId);
DSS_STRONG_ID(DiagnosticIndex);
DSS_STRONG_ID(LexerModeId);
DSS_STRONG_ID(StringStyleId);
DSS_STRONG_ID(SchemaId);
DSS_STRONG_ID(CompilationUnitId);
// CU-scoped symbol identity. Minted per-CompilationUnit (not per-Tree), so a
// SymbolId is meaningful only against the CU that produced it — the semantic
// phase keys its symbol table by SymbolId, bound through a NodeId via
// `UnitAttribute<SymbolId>` (see analysis/compilation_unit/unit_attribute.hpp).
DSS_STRONG_ID(SymbolId);
// Type-system ids (SP2). `TypeKindId` identifies an extension type-kind
// (registry-minted, >= 256; core kinds occupy the TypeKind enum's [0,256)).
// `TypeNameId` interns nominal type/extension names.
DSS_STRONG_ID(TypeKindId);
DSS_STRONG_ID(TypeNameId);

// Arena-element ids (carry `treeTag`): NodeId is the Tree's node index; TypeId
// is the CU-scoped type lattice's index (its arena tag is the CompilationUnitId).
DSS_ARENA_ID(NodeId);
DSS_ARENA_ID(TypeId);

#undef DSS_STRONG_ID
#undef DSS_ARENA_ID

inline constexpr NodeId          InvalidNode{};
inline constexpr RuleId          InvalidRule{};
inline constexpr SchemaTokenId   InvalidSchemaToken{};
inline constexpr BufferId        InvalidBuffer{};
inline constexpr TreeId          InvalidTree{};
inline constexpr DiagnosticIndex InvalidDiagnostic{};
inline constexpr LexerModeId     InvalidLexerMode{};
inline constexpr StringStyleId   InvalidStringStyle{};
inline constexpr SchemaId        InvalidSchemaId{};
inline constexpr CompilationUnitId InvalidCompilationUnit{};
inline constexpr SymbolId        InvalidSymbol{};
inline constexpr TypeId          InvalidType{};
inline constexpr TypeKindId      InvalidTypeKind{};
inline constexpr TypeNameId      InvalidTypeName{};

} // namespace dss

// std::hash specializations live outside namespace dss.
#define DSS_HASH_ID(Name)                                                    \
    template <> struct std::hash<dss::Name> {                                \
        std::size_t operator()(dss::Name id) const noexcept {                \
            return std::hash<std::uint32_t>{}(id.v);                         \
        }                                                                    \
    }

DSS_HASH_ID(NodeId);
DSS_HASH_ID(RuleId);
DSS_HASH_ID(SchemaTokenId);
DSS_HASH_ID(BufferId);
DSS_HASH_ID(TreeId);
DSS_HASH_ID(DiagnosticIndex);
DSS_HASH_ID(LexerModeId);
DSS_HASH_ID(StringStyleId);
DSS_HASH_ID(SchemaId);
DSS_HASH_ID(CompilationUnitId);
DSS_HASH_ID(SymbolId);
DSS_HASH_ID(TypeId);
DSS_HASH_ID(TypeKindId);
DSS_HASH_ID(TypeNameId);

#undef DSS_HASH_ID
