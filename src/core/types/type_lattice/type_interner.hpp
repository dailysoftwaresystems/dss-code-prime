#pragma once

#include "core/export.hpp"
#include "core/substrate/arena_container.hpp"
#include "core/types/interner.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_id.hpp"   // ArenaNames<TypeId, CompilationUnitId>

#include <cstdint>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

// Canonicalizing type interner (SP2). Stores TypeRecords in an
// ArenaBuilder<TypeRecord, TypeId, CompilationUnitId> (dogfooding the SP1
// substrate, plan 08.5 §7) — so the arena's tag is the owning CompilationUnitId
// and every TypeId carries its CU provenance. Variable-length operands/scalars
// live in pools (mirroring Tree's childIndex); nominal names live in a string
// interner. Structurally-identical types collapse to one TypeId via an FNV-1a
// hash-multimap + pool-based equality.
//
// CU-scoped, owned by the consumer (phase #8), bound to a CompilationUnit by id
// — the CompilationUnit itself stays immutable (the NodeAttribute/UnitAttribute
// pattern). Move-only.

namespace dss {

class DSS_EXPORT TypeInterner {
public:
    // `owner` MUST be a valid CompilationUnitId: it is the arena tag stamped
    // onto every TypeId, so an invalid (zero) owner would make every TypeId
    // untagged and silently void the cross-CU isolation guard. Aborts loud
    // rather than degrade quietly.
    explicit TypeInterner(CompilationUnitId owner);

    TypeInterner(TypeInterner const&)            = delete;
    TypeInterner& operator=(TypeInterner const&) = delete;
    TypeInterner(TypeInterner&&)                 = default;
    TypeInterner& operator=(TypeInterner&&)      = default;

    [[nodiscard]] CompilationUnitId owner() const noexcept { return arena_.id(); }
    // Count of distinct interned types (excludes the slot-0 sentinel).
    [[nodiscard]] std::size_t size() const noexcept { return arena_.size() - 1; }

    // ── canonicalizing builders ──
    // primitive: a leaf kind (Bool/I*/U*/F*/Char/Byte/Void) — no operands/scalars.
    TypeId primitive(TypeKind kind);
    // vector: operands=[element], scalars=[lanes].
    TypeId vector(TypeId element, std::int64_t lanes);
    // matrix: operands=[element], scalars=[rows, cols].
    TypeId matrix(TypeId element, std::int64_t rows, std::int64_t cols);
    // single-operand indirection: operands=[target].
    TypeId pointer(TypeId pointee);
    TypeId reference(TypeId referent);
    TypeId nullable(TypeId inner);
    TypeId optional(TypeId inner);
    // array: operands=[element], scalars=[length]. slice: operands=[element].
    TypeId array(TypeId element, std::int64_t length);
    TypeId slice(TypeId element);
    // tuple: operands=[elements...].
    TypeId tuple(std::span<TypeId const> elements);
    // struct/union: nominal name + operands=[fields/variants...].
    TypeId structType(std::string_view name, std::span<TypeId const> fields);
    TypeId unionType(std::string_view name, std::span<TypeId const> variants);
    // fnSig: operands=[result, params...], scalars=[(int)cc].
    TypeId fnSig(std::span<TypeId const> params, TypeId result, CallConv cc);
    // extension: kind = TypeKind::Extension, extensionKind = `kind`, nominal name,
    // operands=[type args...], scalars=[integer args...].
    TypeId extension(TypeKindId kind, std::string_view name,
                     std::span<TypeId const> typeArgs,
                     std::span<std::int64_t const> scalarArgs = {});

    // ── accessors ──
    [[nodiscard]] TypeRecord const&  get(TypeId id)      const { return arena_.at(id); }
    [[nodiscard]] TypeKind           kind(TypeId id)     const { return arena_.at(id).kind; }
    [[nodiscard]] std::span<TypeId const>       operands(TypeId id) const;
    [[nodiscard]] std::span<std::int64_t const> scalars(TypeId id)  const;
    [[nodiscard]] std::string_view              name(TypeId id)     const;

    // FnSig decoders — hand back the result/params without the caller needing
    // to know the operands=[result, params...] storage convention. Abort if
    // `id` is not a FnSig.
    [[nodiscard]] TypeId                   fnResult(TypeId id) const;
    [[nodiscard]] std::span<TypeId const>  fnParams(TypeId id) const;

private:
    TypeId internContent(TypeKind kind, TypeKindId extensionKind,
                         std::span<TypeId const> operands,
                         std::span<std::int64_t const> scalars,
                         TypeNameId name);

    [[nodiscard]] std::uint64_t hashContent(TypeKind kind, TypeKindId extensionKind,
                                            std::span<TypeId const> operands,
                                            std::span<std::int64_t const> scalars,
                                            TypeNameId name) const;
    [[nodiscard]] bool equalContent(TypeId existing, TypeKind kind, TypeKindId extensionKind,
                                    std::span<TypeId const> operands,
                                    std::span<std::int64_t const> scalars,
                                    TypeNameId name) const;

    substrate::ArenaBuilder<TypeRecord, TypeId, CompilationUnitId> arena_;
    std::vector<TypeId>                            operandPool_;
    std::vector<std::int64_t>                      scalarPool_;
    Interner<TypeNameId>                           names_;
    std::unordered_multimap<std::uint64_t, TypeId> byHash_;
};

} // namespace dss
