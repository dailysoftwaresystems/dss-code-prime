#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

// The universal core type lattice (SP2). Every language compiles into this
// lattice; per-language extension type-kinds register on top of it (see
// TypeRegistry). Core kinds occupy the TypeKind enum's [0, 256); extension
// kinds are registry-minted TypeKindIds >= 256, and an extension type is a
// TypeRecord with `kind == TypeKind::Extension` + a valid `extensionKind`.

namespace dss {

enum class TypeKind : std::uint16_t {
    // ── primitives ──
    Bool,
    I8, I16, I32, I64, I128,
    U8, U16, U32, U64, U128,
    // FC17.9(e) (D-CSUBSET-LONG-DOUBLE): F80 is the x87 80-bit extended format
    // (SysV x86_64 / darwin-x86_64 `long double`, 16/16 storage); F128 is IEEE
    // binary128 (AAPCS64 `long double`). DISTINCT kinds so the two future
    // arithmetic arcs (x87 register-stack vs binary128 softfloat) can never
    // cross-fire — both wall loudly at the LIR encoded-width gate until then.
    // Inserted IN the float block (not appended) so the float widening rank
    // reads in declaration order; the ordinal shift of later kinds is safe:
    // no TypeKind ordinal is serialized (name codecs spell names, enum
    // scalar-pool entries are integer kinds ordered BEFORE the floats).
    F16, F32, F64, F80, F128,
    Char,   // Unicode codepoint
    Byte,
    Void,
    // ── aggregates ──
    Struct, Union, Tuple, Array, Slice,
    // ── enumeration (nominal int-compatible) ──
    // D5.5: a named set of integer constants. Distinct nominal identity
    // (two enums with the same underlying type don't compare equal), but
    // int-compatible at all arithmetic / cast sites. Variants are the
    // enumerator symbols (each constant is a value of this enum).
    Enum,
    // ── SIMD ──
    Vector, Matrix,
    // ── indirection ──
    Ptr, Ref, FnPtr, Nullable, Optional,
    // ── functions ──
    FnSig,
    // ── parameterized ──
    Param, Bind,
    // ── extension marker (concrete kind lives in TypeRecord::extensionKind) ──
    Extension,
    // ── type qualifier (D-CSUBSET-VOLATILE-POINTEE / c27) ──
    // `volatile T` — operands=[inner]. A TRANSPARENT skin: it has a DISTINCT
    // interned identity (so `volatile int` != `int` for equality / interning,
    // which is what carries the volatile through a declaration's type to the
    // access), but `kind()` / `operands()` / `scalars()` SEE THROUGH it to the
    // inner type, so every layout / arithmetic / codegen / classification
    // consumer that reads the kind dispatches on the MATERIAL kind WITHOUT a
    // per-site strip — only code that explicitly asks `isVolatileQualified(id)`
    // (the access-volatility chokepoints) observes the wrapper. `volatile u32` =
    // VolatileQual(U32); `volatile u32 *` = Ptr<VolatileQual(U32)> (volatile
    // binds the innermost pointee, C 6.7.3); east `u32 * volatile` =
    // VolatileQual(Ptr<U32>) (a volatile POINTER). Idempotent (no double-wrap).
    // const gets NO such wrapper — const stays ignored for type identity, since
    // it never affects codegen or layout.
    //   FC17.9(d) 1a (D-CSUBSET-QUAL-BITSET): GENERALIZED to a qualifier BITSET.
    //   The node carries a `QualBit` mask (see below) in scalar slot 0 —
    //   `volatile T` = bits{Volatile}, `_Atomic T` = bits{Atomic}, `_Atomic
    //   volatile T` = bits{Volatile,Atomic} — so a multiply-qualified scalar is
    //   ONE skin, not a nesting. Distinct masks intern distinctly (the scalar
    //   joins hashContent/equalContent, exactly like an array's length). The kind
    //   stays named `VolatileQual` (not `QualifiedType`) so pre-existing TypeId
    //   integers, the exhaustive `typeKindName` switch, and the whole volatile
    //   test surface are UNCHANGED. The mask is read ONLY via the raw
    //   `qualifierBits(id)`; the transparent `scalars(id)` sees THROUGH the skin
    //   to the inner type's scalars.
    // Placed LAST (before Count_) so every pre-existing kind keeps its integer
    // value — TypeKind ints appear in scalar pools (enum underlying / CallConv)
    // and cached/round-tripped TypeIds; renumbering would silently shift them.
    VolatileQual,

    // ── C23 nullptr_t (D-CSUBSET-NULLPTR / C23 §6.2.5, §6.4.4.6) ──
    // The type of the predefined constant `nullptr`. A SEMANTIC-TIER-ONLY kind: it
    // exists so the conversion rules can be ONE-WAY (nullptr_t → any pointer / bool,
    // but nothing converts TO nullptr_t) and so `_Generic(nullptr, ...)` sees a
    // distinct type — but the `nullptr` literal LOWERS to the target-agnostic null
    // constant at the HIR tier (exactly like an integer-0 null pointer constant), so
    // NullptrT NEVER reaches MIR (the `I_NullptrTypeInMir` verifier tripwire enforces
    // the invariant). Appended AFTER VolatileQual (before Count_) so every
    // pre-existing kind keeps its integer value — see the VolatileQual note above.
    NullptrT,

    // ── C23 _BitInt(N) bit-precise integer (D-CSUBSET-BITINT / C23 §6.2.5) ──
    // A bit-precise signed/unsigned integer of an EXACT programmer-chosen width N.
    // Carries `scalars=[N, signed]` (N in bits; signed = 1 for `_BitInt`/`signed
    // _BitInt`, 0 for `unsigned _BitInt`) — NOT a distinct kind per signedness
    // (signedness never lives in the kind for any integer). Distinct from the
    // standard I*/U* ranks: `_BitInt(N)` does NOT integer-PROMOTE (C23 §6.3.1.1 —
    // its rank sits between adjacent standard widths), so `_BitInt(4)+_BitInt(4)`
    // is `_BitInt(4)`, and arithmetic WRAPS mod-2^N (masked by construction at the
    // MIR value-materialization boundary). The width tier projects a `_BitInt(N≤64)`
    // to its signed/unsigned native CONTAINER kind (I8/I16/I32/I64 by size) via
    // `reprKind`/`bitIntContainerKind` — the enum→underlying projection precedent —
    // so `requireNativeIntWidth`/`widthFlagsForType` see a native kind and the
    // masking reuses the bit-field extract/insert shift+mask primitive. Appended
    // AFTER NullptrT (before Count_) so every pre-existing kind keeps its integer
    // value — the VolatileQual/NullptrT placement precedent.
    BitInt,

    // ── C99 _Complex (D-CSUBSET-COMPLEX / C99 §6.2.5) ──
    // A complex number = an ordered pair {real, imaginary} of an element FLOAT
    // type (`double _Complex` → element F64, `float _Complex` → F32, `long double
    // _Complex` → the long-double-axis element F80/F128/F64). operands=[element];
    // no scalars, no name (structural identity — two `double _Complex` collapse to
    // one TypeId, like every other single-operand structural kind). Layout is a
    // MEMORY-RESIDENT by-value aggregate {re@0, im@elemSize}, sized 2×elemSize —
    // it enters `isByValueClass`/`isMemoryResidentType`, so a complex rvalue NEVER
    // becomes a bare SSA value: it lives in a slot reached BY ADDRESS, mirroring a
    // wide `_BitInt(N>64)` EXACTLY. Componentwise arithmetic emits F64/F32 ops (they
    // pass the LIR encoded-width gate); an F80/F128 component walls loud at the
    // existing requireEncodedFloatWidth (no new wall — long-double-complex arithmetic
    // rides the long-double arith deferrals). Appended AFTER BitInt (before Count_)
    // so every pre-existing kind keeps its integer value — the VolatileQual/NullptrT/
    // BitInt placement precedent (no TypeKind ordinal is serialized).
    Complex,

    Count_  // keep last — counts the core members
};

static_assert(static_cast<std::uint32_t>(TypeKind::Count_) < 256,
              "core TypeKind members must occupy [0, 256); extensions use "
              "registry-minted TypeKindIds >= 256");

// First registry-minted extension kind. Core kinds (the TypeKind enum) occupy
// the [0, kFirstExtensionKind) range of the open kind space.
inline constexpr std::uint32_t kFirstExtensionKind = 256;

// FC6: the length-scalar sentinel marking a kind=Array as an INCOMPLETE array
// (C99 §6.7.2.1 flexible array member `T x[]`). Distinct from a real length
// (which is >= 0; the semantic phase rejects 0/negative declared lengths, so -1
// can never collide with a user-written array length).
inline constexpr std::int64_t kIncompleteArrayLength = -1;

// VLA C1a (D-CSUBSET-VLA): the length-scalar sentinel marking a kind=Array as a
// VARIABLE-LENGTH array (C99/C11 §6.7.6.2 `int a[n]` with a runtime bound).
// DISTINCT from `kIncompleteArrayLength` (-1, a FAM) — a VLA has a real (runtime)
// size that is NOT known at compile time, so it carries no static layout
// (`computeLayout` already nullopts on any negative length scalar) but is a
// COMPLETE object type. All VLAs of the same element dedup to one TypeId (the
// per-declaration runtime bound lives OUT-OF-BAND in a size side-table, NOT on the
// type — the incomplete-array precedent, Fork A1). Like -1 it can never collide
// with a user-written length (the semantic phase rejects 0/negative declared
// constant lengths).
inline constexpr std::int64_t kVlaLength = -2;

// FC8 bitfields (D-CSUBSET-BITFIELD): the per-field bitfield-width sentinel
// marking an ORDINARY (non-bitfield) struct field in `structType`'s
// `fieldBitWidths` argument. A bitfield passes its declared width in [0, 64]
// (0 = a zero-width unnamed packing-break marker); a non-bitfield passes this.
// The widths are stored in the struct's scalar pool as (width + 1), so 0 in the
// pool = non-bitfield and a struct with NO bitfields interns with EMPTY scalars
// (bit-identical to a pre-bitfield struct — no TypeId churn).
inline constexpr std::int64_t kNotBitfield = -1;

// FC17.9(d) 1a (D-CSUBSET-QUAL-BITSET): the type-qualifier bits carried in a
// VolatileQual node's scalar pool (slot 0). The single generalized qualifier
// skin carries a BITSET so a multiply-qualified scalar — e.g. `_Atomic volatile
// int` — is ONE skin (bits {Volatile,Atomic}) rather than a nesting.
// `volatileQualified` sets Volatile; `atomicQualified` sets Atomic; `qualified`
// merges bits idempotently and order-independently. Read ONLY via the raw
// `qualifierBits(id)` — the transparent `scalars(id)` sees THROUGH the skin to
// the inner type. const is NOT a bit here (it never affects codegen/layout, so
// it is not materialized as a qualifier skin at all). The underlying type is
// int64 because the mask rides an int64 scalar slot.
enum class QualBit : std::int64_t {
    Volatile = 1 << 0,  // C `volatile`  — drives MirInstFlags::Volatile at access.
    Atomic   = 1 << 1,  // C11 `_Atomic` — consumed by FC17.9(d) cycle 1b codegen.
};

// Calling conventions are machine-shaped (not language-shaped) — core lattice
// members, attached to FnSig and consumed by the FFI plan. Stored in a
// TypeRecord's scalar pool as the underlying integer.
enum class CallConv : std::uint8_t {
    CcSysV, CcMS64, CcAAPCS64, CcApple, CcFastcall, CcThiscall, CcVectorcall, CcWasm, CcSpirv,
};

// Trivially-copyable type record stored in the interner's arena (like
// detail::Node). Variable-length data lives in interner-owned pools, addressed
// by half-open [start, count) slices:
//   - operands: child TypeIds (element / field / variant / param / result types).
//   - scalars:  int64 parameters (array length, vector lanes, matrix R/C, the
//               CallConv-as-int, extension scalar args).
//   - name:     interned nominal name (Struct / Union / Extension); invalid for
//               purely-structural types.
// The per-kind encoding convention is documented on each TypeInterner builder.
struct DSS_EXPORT TypeRecord {
    TypeKind      kind          = TypeKind::Void;
    TypeKindId    extensionKind{};            // valid() iff kind == Extension
    std::uint32_t operandStart  = 0;
    std::uint32_t operandCount  = 0;
    std::uint32_t scalarStart   = 0;
    std::uint32_t scalarCount   = 0;
    TypeNameId    name{};                      // valid() for nominal types
};
static_assert(std::is_trivially_copyable_v<TypeRecord>);

// A formal parameter of an extension type-kind, e.g. Varchar<N : Integer> or
// Boxed<T : Type>.
enum class TypeParamKind : std::uint8_t { Integer, Type };

struct DSS_EXPORT TypeParam {
    std::string   name;
    TypeParamKind kind = TypeParamKind::Type;
};

// A type extension as DECLARED by a language schema (`.lang.json`
// typeExtensions[]) — name + formal parameters. No kindId yet: kinds are
// minted when registered into a per-CU TypeRegistry.
struct DSS_EXPORT TypeExtensionDescriptor {
    std::string            name;          // language-qualified, e.g. "TSQL::Varchar"
    std::vector<TypeParam> parameters;
};

// A type extension as REGISTERED in a per-CU TypeRegistry: the declaration plus
// its minted kindId and the owning language.
struct DSS_EXPORT ExtensionDescriptor {
    std::string            name;
    TypeKindId             kindId;        // monotonic, >= kFirstExtensionKind
    std::vector<TypeParam> parameters;
    std::string            sourceLanguage;
};

} // namespace dss
