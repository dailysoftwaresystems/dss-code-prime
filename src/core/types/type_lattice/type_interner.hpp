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
    // C99 _Complex (D-CSUBSET-COMPLEX): a complex number over the element FLOAT
    // type `element` (F32/F64/F80/F128). operands=[element]; NO scalars, NO name —
    // structural identity (two `double _Complex` collapse to one TypeId, the
    // single-operand `slice`/`pointer` precedent; the "2 components" is implicit in
    // the kind, so unlike vector NO lane scalar is carried). The interner dedups on
    // the element for free.
    TypeId complex(TypeId element);
    // The element FLOAT type of a `_Complex` (operands[0]). Aborts if `id` is not a
    // Complex (a caller bug — every consumer gates on `kind(id)==Complex` first, the
    // `bitIntWidth`/`complexElement` decoder precedent).
    [[nodiscard]] TypeId complexElement(TypeId id) const;
    // single-operand indirection: operands=[target].
    TypeId pointer(TypeId pointee);
    TypeId reference(TypeId referent);
    TypeId nullable(TypeId inner);
    TypeId optional(TypeId inner);

    // ── type qualifiers (D-CSUBSET-VOLATILE-POINTEE / c27 · D-CSUBSET-QUAL-BITSET) ──
    // A qualified scalar — `volatile T`, `_Atomic T`, or `_Atomic volatile T` — is a
    // kind=VolatileQual record, operands=[inner], carrying a `QualBit` BITSET in
    // scalar slot 0. DISTINCT interned identity per (inner, mask): `volatile int`,
    // `_Atomic int`, and `int` are three distinct TypeIds (what carries the qualifier
    // through a declaration's type to its access sites), but a TRANSPARENT skin: the
    // `kind()` / `operands()` / `scalars()` accessors SEE THROUGH it to `inner`, so
    // every structural consumer dispatches on the material kind with NO per-site strip
    // (the access chokepoints query `isVolatileQualified` / `isAtomicQualified`).
    //
    // `qualified(inner, addBits)` is the primitive: it STRIPS any qualifier skin
    // already on `inner`, UNIONs its bits with `addBits`, and re-interns a SINGLE skin
    // over the material type. So it is idempotent and order-independent
    // (`qualified(qualified(T,A),B) == qualified(T, A|B)`) and — critically — never
    // DROPS a bit: qualifying an already-qualified type preserves what was there (a
    // naive "return inner if already qualified" would silently lose the new bit, e.g.
    // `_Atomic` over `volatile` staying merely volatile — a loss-of-atomicity
    // miscompile). Wrapping an INVALID id returns InvalidType; a zero mask returns the
    // material type (no skin). const is NOT a bit (it never affects codegen/layout).
    TypeId qualified(TypeId inner, std::int64_t addBits);
    // `volatile T` / `_Atomic T` — thin wrappers over `qualified` setting one bit.
    TypeId volatileQualified(TypeId inner);
    TypeId atomicQualified(TypeId inner);
    // The material type under `id`'s qualifier skin (`id` unchanged if none). ONE
    // strip chokepoint for the rare consumer that must look past the skin where the
    // transparent accessors are bypassed (e.g. the layout entry's raw incomplete
    // checks, or building a derived type that must drop the qualifier). Strips the
    // WHOLE skin (all bits) — the material type is qualifier-free, which is what every
    // caller (layout / assignment compat / scope resolution) wants.
    [[nodiscard]] TypeId stripVolatile(TypeId id) const;
    // The raw QualBit mask on `id`'s OWN record (0 if `id` is not a qualifier skin).
    // Reads the RAW scalar slot directly — NOT the transparent `scalars()`, which sees
    // THROUGH the skin to the inner type's scalars. The single reader of the bitset.
    [[nodiscard]] std::int64_t qualifierBits(TypeId id) const;
    // True iff `id`'s OWN record carries the Volatile / Atomic bit (the access-
    // qualifier queries). Read the RAW mask (not the transparent `kind()`), so they
    // answer "is this exact type volatile / atomic-qualified?" — used at the deref /
    // member / index / scalar access sites (volatile → MirInstFlags::Volatile;
    // atomic → the FC17.9(d) 1b atomic-access lowering).
    [[nodiscard]] bool isVolatileQualified(TypeId id) const;
    [[nodiscard]] bool isAtomicQualified(TypeId id) const;
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
    // VLA C1a (D-CSUBSET-VLA): a VARIABLE-LENGTH array (`int a[n]`) — a kind=Array
    // type whose length scalar is the `kVlaLength` (-2) sentinel. DISTINCT from an
    // incomplete array (-1): a VLA is a COMPLETE object type with a runtime size,
    // but that size lives OUT-OF-BAND (a decl-keyed size side-table), NOT on the
    // type — so all VLAs of one element dedup to a single TypeId (the incomplete-
    // array precedent, Fork A1). No new TypeKind, so every Array consumer keeps
    // working; only runtime-size-bearing consumers (alloca, later sizeof) check
    // `isVlaArray`. `computeLayout` already nullopts on the negative length scalar.
    TypeId vlaArray(TypeId element);
    [[nodiscard]] bool isVlaArray(TypeId id) const;
    // VLA C3 (D-CSUBSET-VLA): does `id` CONTAIN a variable-length array at ANY level
    // of its array spine — i.e. is `id` itself a VLA, or an ARRAY (fixed or VLA)
    // whose element (recursively, via ops[0]) is a VLA? This is the transitive
    // predicate that routes a FIXED-outer multi-dim VLA (`int a[5][n]` —
    // `array(vlaArray(int),5)`, whose top is a fixed Array, NOT `isVlaArray`) to the
    // runtime alloca / stride / sizeof paths that `isVlaArray` alone would miss. It
    // walks ONLY the array-element chain (`ops[0]`) and tests `isVlaArray` at EACH
    // level — NOT `kind==Array`, so a fully-fixed `int[5][5]` does NOT over-fire
    // (every level is a non-VLA Array → false). A non-array base terminates the walk.
    // The `typeContainsFlexibleArray` mirror, on the interner (callable from
    // semantic + hir_to_mir + mir_verifier). An invalid id is not a VLA container.
    [[nodiscard]] bool typeContainsVla(TypeId id) const;
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
    // Idempotent for an IDENTICAL re-completion (same fields + widths + packed) — a
    // benign re-resolution; a CONFLICTING re-completion (different fields) of an
    // already-complete composite is a caller bug and aborts loud. `id` MUST be a
    // composite minted by `forwardComposite` (else fatal).
    //
    // D-CSUBSET-PACKED: `packed` is the whole-composite packed flag (C/C23
    // `__attribute__((packed))`) and is DELIBERATELY NON-DEFAULTED — every call site
    // MUST decide it, so a caller that forgets it FAILS TO COMPILE rather than
    // silently dropping packed (the reintern / cross-CU channel is the one that
    // matters: a silently-unpacked round-trip is an ABI miscompile). packed +
    // non-empty `fieldOffsets` is contradictory (explicit offsets place fields
    // wholesale, overriding padding entirely) → fail loud here.
    void completeComposite(TypeId id, std::span<TypeId const> fields, bool packed,
                           std::span<std::int64_t const> fieldBitWidths = {},
                           std::span<std::uint64_t const> fieldOffsets = {},
                           std::span<std::uint32_t const> fieldAligns = {});
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
    // c107 (D-FFI-DESCRIPTOR-UNION-OVERLAY): a struct with per-field EXPLICIT byte
    // offsets — a foreign OVERLAPPING layout (an FFI union modeled as a struct whose
    // members may share/overlap byte ranges: ULARGE_INTEGER {QuadPart u64@0,
    // LowPart u32@0, HighPart u32@4}). `fieldOffsets[i]` is field i's byte offset; the
    // span is ALL-fields-or-EMPTY (a partial offset set is a caller bug). The offsets
    // are part of the struct's CONTENT identity (they change layout), so an
    // explicit-offset struct is a DISTINCT interned type from the same field-types
    // laid out naturally — and both the shipped `structs` entry and the bare typedef's
    // inline `struct "X" { T @off }` text carry identical offsets so they collapse to
    // ONE TypeId (the tag/typedef canonicalization the field-scope injection relies on).
    // Independent of bitfields (offsets need not compose with bit-widths); passing a
    // non-empty offsets span with bitfields is unsupported (fail loud at layout).
    TypeId structType(std::string_view name, std::span<TypeId const> fields,
                      std::span<std::int64_t const> fieldBitWidths,
                      std::span<std::uint64_t const> fieldOffsets);
    // C11/C23 `alignas` on a struct member (D-CSUBSET-MEMBER-ALIGNAS): a struct with
    // per-field EXPLICIT alignment overrides. `fieldAligns[i]` is field i's declared
    // alignment in bytes (a power of two), or 0 = "no override, use natural
    // alignment". The span is ALL-fields-or-EMPTY (a partial set is a caller bug) —
    // an align-free struct passes EMPTY and interns byte-identically to the 2-arg
    // overload (zero TypeId churn). The aligns are part of the struct's CONTENT
    // identity (they change layout/size), so `struct{alignas(16) int x;}` is a
    // DISTINCT interned type from `struct{int x;}`. A member alignas RAISES a field's
    // alignment (max(natural, override)) at layout; it never lowers it. Mirrors the
    // explicit-offsets channel but is INDEPENDENT: explicit offsets place fields
    // wholesale (overriding alignment entirely), so combining aligns WITH offsets on
    // the same struct is a caller bug (fail loud at completion).
    TypeId structType(std::string_view name, std::span<TypeId const> fields,
                      std::span<std::int64_t const> fieldBitWidths,
                      std::span<std::uint64_t const> fieldOffsets,
                      std::span<std::uint32_t const> fieldAligns);
    // True iff `id` is a Struct carrying c107 explicit field offsets (non-empty
    // `fieldOffsets`). Struct/Union only; false for every naturally-laid-out composite.
    [[nodiscard]] bool hasExplicitOffsets(TypeId id) const;
    // Field `i`'s explicit byte offset, or nullopt when the composite derives offsets
    // from natural alignment (the ordinary path). Mirrors `fieldBitWidth`.
    [[nodiscard]] std::optional<std::uint64_t> explicitFieldOffset(
        TypeId id, std::size_t i) const;
    // True iff `id` is a Struct/Union carrying member-alignas overrides (non-empty
    // `fieldAligns`). Struct/Union only; false for every naturally-aligned composite.
    [[nodiscard]] bool hasExplicitAligns(TypeId id) const;
    // Field `i`'s explicit alignment override in bytes (a power of two), or 0 when
    // that field has no override (use natural alignment). Returns 0 for every field
    // of a composite interned with no aligns. Mirrors `explicitFieldOffset`.
    [[nodiscard]] std::uint32_t explicitFieldAlign(TypeId id, std::size_t i) const;
    // D-CSUBSET-PACKED: true iff `id` is a Struct/Union declared `packed` (C/C23
    // `__attribute__((packed))` / `[[gnu::packed]]`) — all inter-field padding
    // removed, aggregate natural alignment 1. Struct/Union only; false for every
    // ordinary (padded) composite. Mirrors `hasExplicitAligns`. The layout engine
    // reads it to seed the per-field baseline alignment to 1.
    [[nodiscard]] bool isPacked(TypeId id) const;
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
    // C23 _BitInt(N) (D-CSUBSET-BITINT / C23 §6.2.5): a bit-precise integer of
    // EXACT width `widthBits`, signed iff `isSigned`. scalars=[widthBits, signed?1:0];
    // no operands, no name (structural identity — two `_BitInt(N)` of the same width
    // + signedness collapse to one TypeId, like every other scalar-parameterized
    // kind). The interner dedups on the scalar pair for free.
    TypeId bitInt(std::int64_t widthBits, bool isSigned);
    // The declared bit-width N of a `_BitInt(N)` (scalars[0]). Aborts if `id` is not
    // a BitInt (a caller bug — every consumer gates on `kind(id)==BitInt` first).
    [[nodiscard]] std::int64_t bitIntWidth(TypeId id) const;
    // True iff `id` is a SIGNED `_BitInt(N)` (scalars[1]==1). Aborts if not a BitInt.
    [[nodiscard]] bool bitIntIsSigned(TypeId id) const;
    // The native signed/unsigned CONTAINER kind a `_BitInt(N≤64)` projects to at the
    // width tier — N≤8→I8/U8, ≤16→I16/U16, ≤32→I32/U32, ≤64→I64/U64 by signedness
    // (the smallest native integer holding N bits). The enum→underlying `reprKind`
    // precedent: it makes `requireNativeIntWidth`/`widthFlagsForType`/`memAccess-
    // WidthFlags` see a native kind, and it IS the ABI container (`computeLayout`
    // sizes a `_BitInt(N)` as this kind). N>64 (a C2 multi-limb width) returns
    // `TypeKind::Void` — the fail-loud sentinel; the C1 semantic gate rejects N>64
    // before any consumer queries this. Aborts if `id` is not a BitInt.
    [[nodiscard]] TypeKind bitIntContainerKind(TypeId id) const;
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
    // NOTE on VolatileQual transparency (c27): `kind()` / `operands()` /
    // `scalars()` SEE THROUGH a VolatileQual skin to its inner type, so a caller
    // reading a possibly-volatile-qualified id gets the MATERIAL kind/shape. The
    // RAW record (incl. a VolatileQual marker) is reachable only via `get()` (used
    // by reintern / text round-trip, which must preserve the wrapper) and the
    // dedicated `isVolatileQualified` / `stripVolatile` queries.
    [[nodiscard]] TypeRecord const&  get(TypeId id)      const { return arena_.at(id); }
    [[nodiscard]] TypeKind           kind(TypeId id)     const;
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

    // c27 VolatileQual transparency: the material (non-VolatileQual) TypeId an
    // id resolves to — `id` itself unless its RAW record kind is VolatileQual, in
    // which case its single operand (recursively, though idempotency keeps it one
    // level). The single internal chokepoint `kind()`/`operands()`/`scalars()`
    // route through so the wrapper is transparent. Reads `arena_` directly (never
    // the public accessors) to avoid recursion.
    [[nodiscard]] TypeId materialId_(TypeId id) const;

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
        // c107 (D-FFI-DESCRIPTOR-UNION-OVERLAY): per-field EXPLICIT byte offsets, for
        // a struct whose foreign layout OVERLAPS (an FFI union modeled as an
        // explicit-offset struct — ULARGE_INTEGER {QuadPart@0, LowPart@0, HighPart@4}).
        // A SEPARATE channel from `bitWidthScalars` on purpose: the bitfield-presence
        // test is `!scalars(id).empty()` at three lowering sites, and reintern decodes
        // scalars as (width+1) — offsets in that pool would route the struct through
        // the bitfield packer AND reintern as garbage widths. EMPTY = derive offsets
        // from natural alignment (every existing struct → byte-identical TypeId).
        std::vector<std::uint64_t> fieldOffsets;
        // D-CSUBSET-MEMBER-ALIGNAS: per-field EXPLICIT alignment override (bytes,
        // power of two; 0 = no override), for C11/C23 `alignas` on a struct/union
        // member. A SEPARATE channel from `fieldOffsets` on purpose: offsets place
        // fields wholesale (overriding alignment), aligns only RAISE the padding a
        // naturally-placed field gets — the two never combine (fail loud at
        // completion if both are set). EMPTY = every field uses natural alignment
        // (each existing composite → byte-identical TypeId, exactly like offsets).
        std::vector<std::uint32_t> fieldAligns;
        // D-CSUBSET-PACKED (C/C23 `__attribute__((packed))` / `[[gnu::packed]]`): the
        // WHOLE-COMPOSITE packed flag. When true, `computeLayout` feeds a natural
        // baseline alignment of 1 into every field (removing ALL inter-field padding)
        // and the aggregate's own alignment starts at 1 — a member `alignas` still
        // RAISES per-field via the unchanged MAX-fold (alignas wins per-field). A
        // SEPARATE channel from `fieldAligns`/`fieldOffsets`: packed is per-COMPOSITE,
        // the spans are per-FIELD. `false` = ordinary padded layout (every existing
        // composite → byte-identical TypeId — packed enters `contentDeclSiteKey`
        // GUARDED on true, exactly like offsets/aligns). packed + explicit offsets is
        // contradictory (offsets place fields wholesale) → fail loud at completion.
        bool                      packed      = false;
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
