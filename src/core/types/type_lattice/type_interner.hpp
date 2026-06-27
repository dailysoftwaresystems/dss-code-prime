#pragma once

#include "core/export.hpp"
#include "core/substrate/arena_container.hpp"
#include "core/types/interner.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_id.hpp"   // ArenaNames<TypeId, CompilationUnitId>

#include <cstdint>
#include <optional>
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

namespace detail {
// [[noreturn]] loud abort for a stale operand/scalar span read (debug guard).
// Defined in type_lattice.cpp; the message is matched by the red-on-disable pin.
[[noreturn]] DSS_EXPORT void typeInternerStaleSpan();
}  // namespace detail

// ── D-TYPEINTERNER-OPERAND-SPAN-LIFETIME-GUARD ──
// `operands()`/`scalars()`/`fnParams()` hand back a VIEW into `operandPool_` /
// `scalarPool_`. A caller that retains the view across a later intern — which
// may `insert` and REALLOCATE the pool — then reads freed memory: a
// host-dependent heap-use-after-free (MSVC's 1.5× vector growth often has spare
// capacity and survives by luck; libstdc++/libc++'s 2× realloc moves the buffer
// and aborts only under ASan). It bit TWICE (`hir_to_mir.cpp` 4660 + 2490) and
// is INVISIBLE to the Windows-only non-ASan local gate.
//
// `GuardedSpan<T>` makes the anti-pattern abort DETERMINISTICALLY on EVERY host
// in a DEBUG build: it captures the interner's pool-mutation generation when the
// view is created and re-checks it on every access; a mutation since creation →
// loud abort, no ASan required. In a RELEASE build it is a zero-overhead alias
// to `std::span<T const>`, so the public contract and codegen are unchanged. The
// guard is source/target/format-agnostic — pure substrate.
#ifdef NDEBUG
template <class T> using GuardedSpan = std::span<T const>;
#else
template <class T>
class GuardedSpan {
    // A MINIMAL std::span API mirror — only the operations the ~234 accessor
    // call sites actually use (verified at build). Extend (add `rbegin`, an
    // `iterator` typedef, `size_bytes`, …) only when a real caller needs it; the
    // release alias is full std::span, so any addition must keep both arms in sync.
public:
    using element_type = T const;
    using value_type   = T;

    GuardedSpan() = default;
    GuardedSpan(std::span<T const> s, std::uint64_t const* gen, std::uint64_t captured)
        : span_(s), gen_(gen), captured_(captured) {}

    [[nodiscard]] std::size_t size()  const { check_(); return span_.size(); }
    [[nodiscard]] bool        empty() const { check_(); return span_.empty(); }
    [[nodiscard]] T const&    operator[](std::size_t i) const { check_(); return span_[i]; }
    [[nodiscard]] T const&    front() const { check_(); return span_.front(); }
    [[nodiscard]] T const&    back()  const { check_(); return span_.back(); }
    [[nodiscard]] T const*    data()  const { check_(); return span_.data(); }
    [[nodiscard]] auto        begin() const { check_(); return span_.begin(); }
    [[nodiscard]] auto        end()   const { check_(); return span_.end(); }
    [[nodiscard]] GuardedSpan subspan(std::size_t off) const {
        check_(); return GuardedSpan{span_.subspan(off), gen_, captured_};
    }
    [[nodiscard]] GuardedSpan first(std::size_t n) const {
        check_(); return GuardedSpan{span_.first(n), gen_, captured_};
    }
    [[nodiscard]] GuardedSpan last(std::size_t n) const {
        check_(); return GuardedSpan{span_.last(n), gen_, captured_};
    }
    // Implicit decay to a plain span (the historic return type). The check runs
    // at the decay point; the resulting raw span is unguarded for LATER reads —
    // acceptable, as the two historical bugs retained the ACCESSOR result (the
    // `auto ops = interner.operands(id)` shape), which keeps the guard.
    [[nodiscard]] operator std::span<T const>() const { check_(); return span_; }

private:
    void check_() const {
        if (gen_ != nullptr && *gen_ != captured_) detail::typeInternerStaleSpan();
    }
    std::span<T const>   span_{};
    std::uint64_t const* gen_      = nullptr;
    std::uint64_t        captured_ = 0;
};
#endif

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

    // ── NOMINAL composites (D-CSUBSET-SELF-REFERENTIAL-STRUCT) ──
    // A struct/union is a NOMINAL type: its identity is (kind, name, decl-site),
    // NOT its field shape. This is what lets a SELF-REFERENTIAL composite
    // (`struct N { struct N *next; }`, sqlite's `struct sqlite3`) intern — the
    // self-ref field is a `Ptr<N>` whose pointee is THIS type's TypeId, minted
    // BEFORE the body is known. A name-only identity would silently merge two
    // block-scoped same-name DIFFERENT-layout structs in one CU (a layout
    // miscompile), so the caller supplies a STABLE per-declaration `declSiteKey`
    // (a decl-site node-id / scope-id packed into 64 bits — two distinct
    // definitions never share a key, hence never a TypeId).
    //
    // `forwardComposite` mints an INCOMPLETE nominal TypeId (kind+name+declSiteKey,
    // no fields yet). Re-minting the SAME (kind, name, declSiteKey) returns the
    // SAME TypeId ("one composite → one TypeId") — the Pass-1.5 completion and the
    // self-ref field both resolve to it. `completeComposite` attaches the fields
    // (and bit-field widths) ONCE into the IMMUTABLE side-table — the interned
    // TypeRecord is NEVER back-patched (its operand pool range stays empty), so the
    // arena's append-only invariant (arena_container.hpp) holds. After completion
    // `operands(id)` / `scalars(id)` read the fields from the side-table
    // transparently, so every existing composite-operand consumer is unchanged.
    TypeId forwardComposite(TypeKind kind, std::string_view name,
                            std::uint64_t declSiteKey);
    // Attach `fields` (+ per-field bit-field widths, `kNotBitfield` for ordinary —
    // same encoding as the bitfield-aware structType) to a forward-minted composite.
    // Idempotent for an IDENTICAL re-completion (same fields + widths) — a benign
    // re-resolution; a CONFLICTING re-completion (different fields) of an already-
    // complete composite is a caller bug and aborts loud. `id` MUST be a composite
    // minted by `forwardComposite` (else fatal).
    void completeComposite(TypeId id, std::span<TypeId const> fields,
                           std::span<std::int64_t const> fieldBitWidths = {});
    // True iff `id` is a Struct/Union that was forward-minted but NOT yet completed.
    // An EXPLICIT flag, NOT "operands empty": `struct E {}` is a LEGAL COMPLETE
    // zero-field struct (size 0). A non-composite kind is never incomplete here.
    [[nodiscard]] bool isIncompleteComposite(TypeId id) const;

    // struct/union: nominal name + fields. The 2-arg / 3-arg overloads are the
    // COMPLETE-AT-ONCE convenience path (shipped descriptors, reintern, text
    // round-trip): they `forwardComposite` + `completeComposite` in one call,
    // deriving the `declSiteKey` from the FIELD CONTENT so two complete-at-once
    // composites with the SAME (name, fields, widths) collapse to one TypeId
    // (canonicalization preserved) while same-name DIFFERENT-field composites stay
    // distinct (StructIsNominalAndStructural). The semantic analyzer's
    // self-referential path uses `forwardComposite` + `completeComposite` directly
    // with a decl-site key instead.
    TypeId structType(std::string_view name, std::span<TypeId const> fields);
    // FC8 bitfields (D-CSUBSET-BITFIELD): a struct with per-field bitfield widths.
    // `fieldBitWidths[i]` is `kNotBitfield` for an ordinary field, or the field's
    // declared bit-width in [0, 64] for a bitfield (0 = a zero-width unnamed
    // packing-break marker). Stored in the struct's scalar pool as (width + 1);
    // when EVERY field is ordinary the struct interns with EMPTY scalars —
    // bit-identical to the 2-arg overload (no TypeId churn for existing structs).
    // The width is part of the struct's CONTENT identity (it changes layout/size),
    // so two structs differing only in a bitfield width are distinct interned types.
    TypeId structType(std::string_view name, std::span<TypeId const> fields,
                      std::span<std::int64_t const> fieldBitWidths);
    TypeId unionType(std::string_view name, std::span<TypeId const> variants);
    // FC8 bitfields (D-CSUBSET-BITFIELD): a union with per-member bit-field widths
    // (same `kNotBitfield`/width encoding + empty-scalars-when-none rule as the
    // bitfield-aware structType). A union bit-field member occupies bits [0, W) of
    // its own allocation unit at offset 0 (each member independent).
    TypeId unionType(std::string_view name, std::span<TypeId const> variants,
                     std::span<std::int64_t const> fieldBitWidths);
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
    [[nodiscard]] GuardedSpan<TypeId>        operands(TypeId id) const;
    [[nodiscard]] GuardedSpan<std::int64_t> scalars(TypeId id)  const;
    [[nodiscard]] std::string_view           name(TypeId id)     const;

    // FC8 bitfields (D-CSUBSET-BITFIELD): the declared bit-width of struct field
    // `fieldIndex`, or nullopt if that field is ordinary (non-bitfield). A
    // zero-width bitfield returns 0 (distinct from nullopt). A struct interned
    // with no bitfields (empty scalars) returns nullopt for every field. The
    // layout engine reads this to pack bitfields; codegen reads it to emit the
    // extract/insert shift+mask.
    [[nodiscard]] std::optional<std::uint32_t>
    fieldBitWidth(TypeId structId, std::size_t fieldIndex) const;

    // FnSig decoders — hand back the result/params without the caller needing
    // to know the operands=[result, params...] storage convention. Abort if
    // `id` is not a FnSig.
    [[nodiscard]] TypeId               fnResult(TypeId id) const;
    [[nodiscard]] GuardedSpan<TypeId>  fnParams(TypeId id) const;
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

    // D-CSUBSET-SELF-REFERENTIAL-STRUCT: forward-mint helper. Dedups a composite by
    // its NOMINAL identity (kind, name, declSiteKey) through `byHash_` — verifying
    // the candidate's kind+name AND its side-table `declSiteKey` so a hash
    // collision can never alias two distinct composites. Returns the existing
    // forward record on a repeat (Pass-1.5 completion / self-ref field find the
    // same TypeId); else mints a fresh INCOMPLETE record + an incomplete side-table
    // entry carrying `declSiteKey`. Fields are NOT in the operand pool (the record's
    // operand range stays empty); they arrive later via `completeComposite`.
    TypeId internComposite(TypeKind kind, std::string_view name,
                           std::uint64_t declSiteKey);

    // Wrap a raw pool view in a GuardedSpan tagged with the current pool
    // generation (debug) — or return it unchanged (release alias). The single
    // chokepoint every accessor routes through.
    template <class T>
    [[nodiscard]] GuardedSpan<T> guard_(std::span<T const> raw) const noexcept {
#ifdef NDEBUG
        return raw;
#else
        return GuardedSpan<T>{raw, &poolGen_, poolGen_};
#endif
    }

    // D-CSUBSET-SELF-REFERENTIAL-STRUCT: the IMMUTABLE field side-table for nominal
    // composites, keyed by composite TypeId.v. The incomplete entry (fields empty,
    // complete=false) is created at `forwardComposite`; `completeComposite` fills
    // `fields` / `bitWidthScalars` ONCE and flips `complete`. `operands(id)` /
    // `scalars(id)` read it for a Struct/Union, so the interned TypeRecord's own
    // operand pool range stays EMPTY — the arena stays append-only (no operand-pool
    // back-patch, the plan-lock's #2 charge). A direct `unordered_map` (not the
    // auto-promoting ArenaAttribute) so each entry's `fields` vector buffer is
    // pointer-STABLE across later inserts: a GuardedSpan into it stays valid until
    // the next pool/side-table mutation bumps `poolGen_` (the usual span contract).
    struct CompositeFields {
        std::vector<TypeId>       fields;            // field/variant TypeIds
        std::vector<std::int64_t> bitWidthScalars;   // (width+1)/0 encoding; EMPTY when
                                                     // no bit-field (cf. structType)
        std::uint64_t             declSiteKey = 0;   // the nominal-identity discriminator
        bool                      complete    = false;
    };

    substrate::ArenaBuilder<TypeRecord, TypeId, CompilationUnitId> arena_;
    std::vector<TypeId>                            operandPool_;
    std::vector<std::int64_t>                      scalarPool_;
    // Bumped on every pool-mutating intern AND every composite forward-mint /
    // completion (D-TYPEINTERNER-OPERAND-SPAN-LIFETIME-GUARD). A GuardedSpan
    // captures this at creation and aborts on a stale read.
    std::uint64_t                                  poolGen_ = 0;
    Interner<TypeNameId>                           names_;
    std::unordered_multimap<std::uint64_t, TypeId> byHash_;
    std::unordered_map<std::uint32_t, CompositeFields> compositeFields_;
};

} // namespace dss
