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

// Arena-element id: an arena index `v` plus a provenance tag `arenaTag` (the
// owning arena's tag). `arenaTag == 0` is the "untagged" sentinel — a literal
// `Name{3}` constructed in tests passes the validators, while a tagged id
// obtained from one arena and handed to another aborts with both tags in the
// message (the SH3 cross-arena guard, generalized in src/core/substrate/).
//
// Equality and ordering compare `.v` ONLY; `arenaTag` is provenance metadata,
// not identity, so same-slot ids from different arenas compare equal and the
// validators (not equality) are the enforcement point. This is the shape
// `substrate::ArenaContainer`/`ArenaBuilder` require of their id parameter.
//
// (The field is named `arenaTag` — neutral across arenas: for NodeId it is the
// source TreeId; for TypeId it is the owning CompilationUnitId.)
#define DSS_ARENA_ID(Name)                                                   \
    struct Name {                                                            \
        constexpr Name() noexcept = default;                                 \
        constexpr explicit Name(std::uint32_t value) noexcept : v(value) {}  \
        constexpr Name(std::uint32_t value, std::uint32_t tag) noexcept      \
            : v(value), arenaTag(tag) {}                                      \
        std::uint32_t v       = 0;                                           \
        std::uint32_t arenaTag = 0;                                           \
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
// CU-scoped lexical-scope identity. Minted per-CompilationUnit by the
// semantic phase's scope tree. A ScopeId is only meaningful against the CU
// that produced it; the sentinel `InvalidScope` (`v == 0`) means "no scope"
// — slot 0 of the scope tree is reserved. Real scopes start at id 1
// (typically the CU root scope).
DSS_STRONG_ID(ScopeId);
// Type-system ids (SP2). `TypeKindId` identifies an extension type-kind
// (registry-minted, >= 256; core kinds occupy the TypeKind enum's [0,256)).
// `TypeNameId` interns nominal type/extension names.
DSS_STRONG_ID(TypeKindId);
DSS_STRONG_ID(TypeNameId);
// HIR ids (HR1). `HirModuleId` is a HIR arena's identity tag (the arena-tag
// stamped onto every HirNodeId for the cross-module guard) — minted by a
// monotonic counter, mirroring TreeId. `HirKindId` identifies an extension
// HIR-kind (registry-minted, >= 256; core HirKinds occupy the HirKind enum's
// [0,256)), mirroring TypeKindId.
DSS_STRONG_ID(HirModuleId);
DSS_STRONG_ID(HirKindId);
// HIR operator id (HR2). Identifies an extension operator (registry-minted,
// >= 256; core operators occupy the HirOpKind enum's [0,256)), the operator
// analog of HirKindId. Carried in a BinaryOp/UnaryOp node's `payload`.
DSS_STRONG_ID(HirOpId);

// A registered HIR intrinsic id (HR6). Unlike HirKindId/HirOpId there is NO
// universal core intrinsic set — every intrinsic is registry-minted — so ids run
// monotonically from 1 (0 == InvalidHirIntrinsic). Carried in an IntrinsicCall
// node's `payload`; resolved against the module's HirIntrinsicRegistry.
DSS_STRONG_ID(HirIntrinsicId);

// MIR ids (ML1). `MirModuleId` is a MIR module's identity tag — the arena-tag
// stamped onto every MirInstId/MirBlockId/MirFuncId of that module (one tag,
// three element-id arenas), minted by a monotonic counter, mirroring
// HirModuleId. The optimizer rebuilds MIR functionally (read old module → build
// a new one), so each rebuilt module gets a fresh MirModuleId; CU-scoped TypeIds
// survive the rebuild untouched (their arenaTag is the CompilationUnitId, not
// the module).
DSS_STRONG_ID(MirModuleId);

// Arena-element ids (carry `arenaTag`): NodeId is the Tree's node index; TypeId
// is the CU-scoped type lattice's index (its arena tag is the CompilationUnitId);
// HirNodeId is a HIR module's node index (its arena tag is the HirModuleId).
DSS_ARENA_ID(NodeId);
DSS_ARENA_ID(TypeId);
DSS_ARENA_ID(HirNodeId);
// MIR arena-element ids (ML1), all tagged by the owning MirModuleId. In the
// FUSED value model a non-void instruction IS its SSA value, so there is no
// separate value arena: `MirValueId` is an alias of `MirInstId` (declared in
// mir/mir_node.hpp, where it has the id type in scope).
DSS_ARENA_ID(MirInstId);
DSS_ARENA_ID(MirBlockId);
DSS_ARENA_ID(MirFuncId);
DSS_ARENA_ID(MirGlobalId);

// LIR ids (ML5). `LirModuleId` is the LIR module's identity tag — the
// arena-tag stamped onto every LirInstId/LirBlockId/LirFuncId of that
// module. The LIR substrate is target-blind; each module carries a
// runtime `TargetSchemaId` that refers to a JSON-loaded `TargetSchema`
// (parallel to `SchemaId` for the frontend). ML5 cycle 2 pivot:
// targets are config files (`src/dss-config/targets/*.target.json`),
// not C++ enums.
DSS_STRONG_ID(LirModuleId);
DSS_STRONG_ID(TargetSchemaId);
DSS_ARENA_ID(LirInstId);
DSS_ARENA_ID(LirBlockId);
DSS_ARENA_ID(LirFuncId);

// `LirSpillSlot` is the strong-typed stack-slot id minted by the
// register allocator's spill path (ML6). A bare `std::uint32_t` would
// alias every other count/ordinal in scope; the strong id makes the
// variant arm of `LirRegAssignment` nominally distinct from physical
// register ordinals. Allocator counts spills via a plain `uint32_t`
// counter (a cardinality, not an identity) and mints `LirSpillSlot{n}`
// values from it.
DSS_STRONG_ID(LirSpillSlot);

// `RelocationKind` is the opaque target-declared tag the assembler
// writes onto `Relocation::kind` (plan 13 AS1 §2.6). Each target
// schema declares its own kind set in `*.target.json::relocations[]`;
// the substrate never branches on the value (read it via
// `schema.relocationInfo(kind)`). The strong-id wrapper prevents the
// assembler from fabricating an int and writing it directly —
// values flow only through `TargetRelocationInfo::kind` and the
// schema's lookup accessors. Slot 0 is the invalid sentinel
// (`RelocationKind{}` default-constructs to invalid; loader rejects
// any declared kind with `v == 0`).
DSS_STRONG_ID(RelocationKind);

// `ObjectFormatSchemaId` is the monotonic identity tag minted by
// the loader for each JSON-loaded `ObjectFormatSchema` (plan 14
// LK4). Mirrors `TargetSchemaId` for the linker tier — the
// substrate is target-blind AND format-blind; the schema's
// identity is opaque to consumers, who hold a `shared_ptr` and
// read via accessors.
DSS_STRONG_ID(ObjectFormatSchemaId);

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
inline constexpr ScopeId         InvalidScope{};
inline constexpr TypeId          InvalidType{};
inline constexpr TypeKindId      InvalidTypeKind{};
inline constexpr TypeNameId      InvalidTypeName{};
inline constexpr HirModuleId     InvalidHirModule{};
inline constexpr HirKindId       InvalidHirKind{};
inline constexpr HirOpId         InvalidHirOp{};
inline constexpr HirIntrinsicId  InvalidHirIntrinsic{};
inline constexpr HirNodeId       InvalidHirNode{};
inline constexpr MirModuleId     InvalidMirModule{};
inline constexpr MirInstId       InvalidMirInst{};
inline constexpr MirBlockId      InvalidMirBlock{};
inline constexpr LirModuleId     InvalidLirModule{};
inline constexpr LirInstId       InvalidLirInst{};
inline constexpr LirBlockId      InvalidLirBlock{};
inline constexpr LirFuncId       InvalidLirFunc{};
inline constexpr LirSpillSlot    InvalidLirSpillSlot{};
inline constexpr RelocationKind         InvalidRelocationKind{};
inline constexpr ObjectFormatSchemaId   InvalidObjectFormatSchema{};
inline constexpr TargetSchemaId  InvalidTargetSchema{};
inline constexpr MirFuncId       InvalidMirFunc{};
inline constexpr MirGlobalId     InvalidMirGlobal{};

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
DSS_HASH_ID(ScopeId);
DSS_HASH_ID(TypeId);
DSS_HASH_ID(TypeKindId);
DSS_HASH_ID(TypeNameId);
DSS_HASH_ID(HirModuleId);
DSS_HASH_ID(HirKindId);
DSS_HASH_ID(HirOpId);
DSS_HASH_ID(HirIntrinsicId);
DSS_HASH_ID(HirNodeId);
DSS_HASH_ID(MirModuleId);
DSS_HASH_ID(MirInstId);
DSS_HASH_ID(MirBlockId);
DSS_HASH_ID(MirFuncId);
DSS_HASH_ID(MirGlobalId);
DSS_HASH_ID(LirModuleId);
DSS_HASH_ID(LirInstId);
DSS_HASH_ID(LirBlockId);
DSS_HASH_ID(LirFuncId);
DSS_HASH_ID(TargetSchemaId);
DSS_HASH_ID(RelocationKind);
DSS_HASH_ID(ObjectFormatSchemaId);

#undef DSS_HASH_ID
