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

    // ── Arena concept (substrate/arena_tag.hpp) — lets an
    //    `ArenaAttribute<TypeInterner, T>` side-table key a per-CU attribute by
    //    TypeId (FC6's StructLayout layout table). `nodeCount()` is the full slot
    //    count INCLUDING the slot-0 sentinel so the attribute's dense vector can
    //    index by `TypeId::v` directly (v ∈ [1, size]).
    using IdType  = TypeId;
    using TagType = CompilationUnitId;
    [[nodiscard]] TagType      id()        const noexcept { return arena_.id(); }
    [[nodiscard]] std::size_t  nodeCount() const noexcept { return arena_.size(); }

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
    // incomplete array (C99 §6.7.2.1 flexible array member `T x[]`): a kind=Array
    // type whose length scalar is the `kIncompleteArrayLength` sentinel. It is an
    // INCOMPLETE type — it has no size of its own (FC6 lays it out contributing an
    // offset but 0 bytes to the enclosing struct's size), and `sizeof` of it is
    // ill-formed. Represented as a sentinel on the existing Array kind (no new
    // TypeKind) so every Array consumer keeps working; only size-bearing consumers
    // check `isIncompleteArray`.
    TypeId incompleteArray(TypeId element);
    [[nodiscard]] bool isIncompleteArray(TypeId id) const;
    // tuple: operands=[elements...].
    TypeId tuple(std::span<TypeId const> elements);
    // struct/union: nominal name + operands=[fields/variants...].
    TypeId structType(std::string_view name, std::span<TypeId const> fields);
    TypeId unionType(std::string_view name, std::span<TypeId const> variants);
    // enum: nominal name + scalars=[(int)underlyingTypeKind]. Variants
    // are NOT stored as operands (each enumerator is a Variable symbol
    // with the enum TypeId; the enum type itself is int-compatible).
    TypeId enumType(std::string_view name,
                    TypeKind underlying = TypeKind::I32);
    // fnSig: operands=[result, params...], scalars=[(int)cc, isVariadic].
    // D-LANG-VARIADIC (step 13.4): variadic flips the second scalar slot;
    // non-variadic encodings remain 1-slot for cache stability against
    // every pre-13.4 TypeId. The declared params are the FIXED arg count
    // (matches LLVM's `(i32 (i8*, ...))*` convention — `...` is a
    // marker, not a typed param).
    TypeId fnSig(std::span<TypeId const> params, TypeId result, CallConv cc);
    TypeId fnSig(std::span<TypeId const> params, TypeId result, CallConv cc,
                 bool isVariadic);
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
    // D-LANG-VARIADIC (step 13.4): true iff this FnSig was built via
    // the 4-arg `fnSig()` overload with `isVariadic=true`. Read from
    // scalars[1]. Pre-13.4 FnSigs (built via the 3-arg overload)
    // encode scalars=[(int)cc] only — `fnIsVariadic` returns false
    // for them (scalar count < 2 → no variadic encoding present).
    [[nodiscard]] bool                     fnIsVariadic(TypeId id) const;

    // ── promotion / coercion (C99 "usual arithmetic conversions") ──
    // The common arithmetic type two operands are coerced to before a binary
    // op. Returns `InvalidType` for non-arithmetic operand pairs (pointers,
    // structs, etc. — the caller decides whether that's a diagnostic or a
    // pass-through). Algorithm follows C99 §6.3.1.8 in spirit:
    //   - if either is floating-point, promote both to the wider floating
    //     type;
    //   - else apply integer promotions (Bool/Char/I8/U8/I16/U16 → I32);
    //   - then equal types → same; same-signedness → wider rank;
    //     cross-signedness → unsigned wins on equal rank, else the wider
    //     rank's signedness.
    // Pure type-level query; no constant evaluation, no diagnostics.
    [[nodiscard]] TypeId commonType(TypeId a, TypeId b);

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
