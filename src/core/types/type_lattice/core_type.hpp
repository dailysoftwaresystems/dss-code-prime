#pragma once

#include "core/export.hpp"
#include "core/types/enum_name_table.hpp"  // EnumNameTable (kTypeParamKindTable)
#include "core/types/strong_ids.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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
    // The type of the predefined constant `nullptr`, and — since
    // D-CSUBSET-NULLPTR-T-DECLARABLE — a REAL OBJECT TYPE you can declare, store,
    // pass and return, with the size, alignment and representation of `void *` and
    // exactly one value.
    //
    // ★ IT IS A SEMANTIC IDENTITY WITH A BORROWED REPRESENTATION, which is a
    // different thing from the "semantic-tier-only" kind this used to be. The
    // identity has to be distinct: the conversion rules are ONE-WAY (nullptr_t → any
    // pointer / bool, but nothing converts TO nullptr_t), and `_Generic(nullptr,
    // typeof(nullptr): …)` must not select the `void *` arm (✔MEASURED: gcc 13.3.0
    // and clang 18.1.3 both select the nullptr_t arm). The representation does not:
    // every tier below the semantic one sees a plain pointer, because
    // `TypeInterner::representationType` PROJECTS the kind to `Ptr<Void>` at the
    // semantic→HIR boundary — the same move `reprKind` makes for Enum and
    // `bitIntContainerKind` for `_BitInt(N≤64)`, one tier higher because MIR's own
    // vocabulary refuses this kind outright (`I_NullptrTypeInMir`, and `mir_text`'s
    // table omits its spelling as a stated contract). Clang draws the line in the
    // same place. So NullptrT still NEVER reaches MIR — but now because it is
    // projected there, not because objects of the type are refused.
    //
    // Appended AFTER VolatileQual (before Count_) so every pre-existing kind keeps
    // its integer value — see the VolatileQual note above.
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

// ── THE SPELLINGS HAVE ONE OWNER (D-TYPEKIND-PASCALCASE-SPELLINGS-HAVE-TWO-OWNERS) ──
//
// ★★★ THIS ENUM HAD THREE OWNERS OF ITS SPELLINGS AND TWO OWNERS OF THE
// PRIMITIVE SUBSET, AND THE FIRST PAIR HAD ALREADY DRIFTED. `lir_text.cpp`'s
// `typeKindName` (the `.dsslir` literal-pool round-trip tag) and
// `type_lattice/type_reintern.cpp`'s `typeKindName` (the abort-message name)
// were two independent exhaustive switches over the same forty PascalCase
// spellings; `grammar_schema_json.cpp`'s `kGrammarCoreTypeTable` was a third
// hand-list of twenty of them, minted deliberately because the config surface
// accepts a strict SUBSET and the lane that wrote it could not reach the other
// two files. ✔MEASURED at the drift: the two switches agreed on all forty real
// kinds and disagreed on `Count_` — `"?"` in one, `"Count_"` in the other.
//
// ★★ AND THE THING THAT MADE IT INVISIBLE WAS A COMMENT. `lir_text.cpp`'s own
// note asserted *"Names match the sibling table in
// `type_lattice/type_reintern.cpp`, which never drifted"* — a claim about
// another file, written where no reader would check it, and false on the day it
// was measured. A comment that certifies two owners agree is the artefact that
// stops the next reader from looking; the fix is to leave one owner rather than
// to correct the certificate.
//
// ⚠ `Count_` IS DELIBERATELY ABSENT, and that is the row's real question
// answered rather than deferred. It is the enum-CARDINALITY sentinel, not a
// type: no `TypeRecord` can carry it. Listing it would give `fromName("Count_")`
// a resolution, and BOTH directions of this table are routers — `.dsslir` text
// would mint a `core Count_` literal-pool tag and a `.lang.json` could declare
// `"core": "Count_"`, each accepted at load and fatal downstream. Unlisted, it
// renders EMPTY through `nameOrEmpty` and every consumer states its own
// "no spelling" rendering in its own medium (`lir_text.cpp`'s `?`, which the
// pool parser refuses because it does not lex as an identifier;
// `type_reintern.cpp`'s `<unnamed kind #N>`, which names the ordinal). Use
// `nameOrEmpty`, NEVER `name()`: `name()` would answer the sentinel with row 0's
// spelling, `"Bool"`.
inline constexpr EnumNameTable<TypeKind, 40> kTypeKindNameTable{{{
    { TypeKind::Bool,         "Bool"         },
    { TypeKind::I8,           "I8"           },
    { TypeKind::I16,          "I16"          },
    { TypeKind::I32,          "I32"          },
    { TypeKind::I64,          "I64"          },
    { TypeKind::I128,         "I128"         },
    { TypeKind::U8,           "U8"           },
    { TypeKind::U16,          "U16"          },
    { TypeKind::U32,          "U32"          },
    { TypeKind::U64,          "U64"          },
    { TypeKind::U128,         "U128"         },
    { TypeKind::F16,          "F16"          },
    { TypeKind::F32,          "F32"          },
    { TypeKind::F64,          "F64"          },
    { TypeKind::F80,          "F80"          },
    { TypeKind::F128,         "F128"         },
    { TypeKind::Char,         "Char"         },
    { TypeKind::Byte,         "Byte"         },
    { TypeKind::Void,         "Void"         },
    { TypeKind::Struct,       "Struct"       },
    { TypeKind::Union,        "Union"        },
    { TypeKind::Tuple,        "Tuple"        },
    { TypeKind::Array,        "Array"        },
    { TypeKind::Slice,        "Slice"        },
    { TypeKind::Enum,         "Enum"         },
    { TypeKind::Vector,       "Vector"       },
    { TypeKind::Matrix,       "Matrix"       },
    { TypeKind::Ptr,          "Ptr"          },
    { TypeKind::Ref,          "Ref"          },
    { TypeKind::FnPtr,        "FnPtr"        },
    { TypeKind::Nullable,     "Nullable"     },
    { TypeKind::Optional,     "Optional"     },
    { TypeKind::FnSig,        "FnSig"        },
    { TypeKind::Param,        "Param"        },
    { TypeKind::Bind,         "Bind"         },
    { TypeKind::Extension,    "Extension"    },
    { TypeKind::VolatileQual, "VolatileQual" },
    { TypeKind::NullptrT,     "NullptrT"     },
    { TypeKind::BitInt,       "BitInt"       },
    { TypeKind::Complex,      "Complex"      },
    // ⚠ NOTHING BEYOND THIS POINT BUT A NEW ENUMERATOR'S ROW. `Count_` is not a
    // kind — see the note above.
}}};
DSS_CHECK_ENUM_NAME_TABLE(kTypeKindNameTable);

// ── TOTALITY: the protection the two switches carried, moved to the table ────
//
// ★★★ THE SWITCHES WERE NOT ONLY A DRIFT HAZARD, THEY WERE ALSO A GUARD, and
// replacing them with a table would have SILENTLY DROPPED IT. Project-wide
// `-Werror=switch` / MSVC C4062 made a no-`default` switch over `TypeKind` fail
// the build the moment an enumerator was appended without an arm — the exact
// mechanism `src/lir/CMakeLists.txt` records as the strongest evidence for that
// gate, after `VolatileQual` and `NullptrT` were both appended with no arm and
// emitted an unparseable `core ?` for their whole lifetime. A table lookup
// warns about nothing: a kind with no row would come back EMPTY and be rendered
// as the "no spelling" sentinel by a consumer that had no idea a real kind had
// gone missing.
//
// So the guarantee is re-stated HERE, where it covers EVERY consumer rather
// than only the translation units that happened to hold a switch: every
// enumerator below `Count_` must resolve to a row. Adding an enumerator without
// a row above is a COMPILE ERROR at this static_assert, not a runtime surprise.
// The companion size check is independent of it — this loop proves coverage,
// the size proves no row names a value outside the enum's range.
inline constexpr bool kTypeKindNameTableIsTotal = [] {
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(TypeKind::Count_); ++i) {
        if (!kTypeKindNameTable.findName(static_cast<TypeKind>(i))) return false;
    }
    return true;
}();
static_assert(kTypeKindNameTableIsTotal,
              "kTypeKindNameTable is not TOTAL: some TypeKind below Count_ has "
              "no row, so it would render as the caller's 'no spelling' "
              "sentinel instead of its name. Add the row. "
              "See D-TYPEKIND-PASCALCASE-SPELLINGS-HAVE-TWO-OWNERS.");
static_assert(kTypeKindNameTable.rows.size()
                  == static_cast<std::size_t>(TypeKind::Count_),
              "kTypeKindNameTable must hold exactly one row per TypeKind "
              "enumerator below Count_; a surplus row names a value outside the "
              "enum's range and would be reachable through fromName.");

// The spelling of `k`, or EMPTY when `k` has no spelling (`Count_`, or an
// out-of-range ordinal reconstructed from a cast). ⚠ The name says
// `OrEmpty` because a caller that formats the result without checking prints
// NOTHING where a type name belongs — see the two consumers, each of which
// states its own sentinel.
[[nodiscard]] constexpr std::string_view typeKindNameOrEmpty(TypeKind k) noexcept {
    return kTypeKindNameTable.nameOrEmpty(k);
}

// The inverse. `Count_` is unlisted, so no spelling resolves to it.
[[nodiscard]] constexpr std::optional<TypeKind>
typeKindFromName(std::string_view s) noexcept {
    return kTypeKindNameTable.fromName(s);
}

// ── THE PRIMITIVE SUBSET HAS ONE OWNER TOO ──────────────────────────────────
//
// A LEAF kind: rebuildable from the kind alone, with no operands, no scalars and
// no name — exactly the set `TypeInterner::primitive(k)` can realize. ✔MEASURED
// that this predicate existed TWICE, as the same twenty kinds in the same order:
// `type_reintern.cpp`'s file-local `isPrimitiveKind` (the rebuild gate) and
// `grammar_schema_json.cpp`'s `kGrammarCoreTypeTable` (which spelled out the
// config surface's accepted `core` names and DEFINED itself, in its own comment,
// as "a fixed-width scalar the interner can build with `primitive(k)`"). One
// concept, stated twice, in two shapes — so the config surface now derives its
// twenty spellings as `namesWhere<20>(kTypeKindNameTable, isPrimitiveTypeKind)`
// rather than retyping them.
//
// ⚠ NO `default:` ARM, DELIBERATELY. The old copy had one (`default: return
// false`), so a new enumerator silently answered "not primitive" — and the
// config surface would silently not accept it. Written out in full, project-wide
// `-Werror=switch` / C4062 makes a new enumerator fail the build here until
// somebody decides which side it belongs on. That is the same protection the
// totality static_assert above restores for the SPELLINGS.
[[nodiscard]] constexpr bool isPrimitiveTypeKind(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::Bool:
        case TypeKind::I8:   case TypeKind::I16:  case TypeKind::I32:
        case TypeKind::I64:  case TypeKind::I128:
        case TypeKind::U8:   case TypeKind::U16:  case TypeKind::U32:
        case TypeKind::U64:  case TypeKind::U128:
        case TypeKind::F16:  case TypeKind::F32:  case TypeKind::F64:
        case TypeKind::F80:  case TypeKind::F128:
        case TypeKind::Char: case TypeKind::Byte: case TypeKind::Void:
        // C23 nullptr_t: an operand-less scalar kind — `primitive(k)` builds it.
        case TypeKind::NullptrT:
            return true;
        // ── NOT leaf-rebuildable, each for a stated reason ──
        case TypeKind::Struct: case TypeKind::Union:                // fields + nominal name
        case TypeKind::Tuple:  case TypeKind::Array: case TypeKind::Slice:
        case TypeKind::Enum:                                        // variants + underlying
        case TypeKind::Vector: case TypeKind::Matrix:               // element + lane scalars
        case TypeKind::Ptr:    case TypeKind::Ref:   case TypeKind::FnPtr:
        case TypeKind::Nullable: case TypeKind::Optional:
        case TypeKind::FnSig:                                       // params + result
        case TypeKind::Param:  case TypeKind::Bind:
        case TypeKind::Extension:                                   // registry-minted kindId
        case TypeKind::VolatileQual:                                // inner + qualifier bits
        case TypeKind::BitInt:                                      // width + signedness scalars
        case TypeKind::Complex:                                     // element float operand
        case TypeKind::Count_:                                      // not a type at all
            return false;
    }
    // Out-of-range-ordinal backstop only (an enum switch is not exhaustive for
    // control-flow purposes, so this satisfies -Wreturn-type / MSVC C4715).
    return false;
}

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

// ── THE SPELLINGS HAVE ONE OWNER (D-CONFIG-GRAMMAR-LOADER-INLINE-CHAIN-VOCABULARIES-REMAIN) ──
//
// `typeExtensions[].parameters[].kind`. The spellings are PascalCase because
// that is what a language document writes (`Varchar<N : Integer>`), not because
// anything renders an enumerator name — there is exactly one spelling set here,
// which is the property this table exists to keep. Previously an inline
// `kindStr == "Integer" / "Type"` chain in the grammar loader, with the accepted
// pair retyped in the refusal beside it.
inline constexpr EnumNameTable<TypeParamKind, 2> kTypeParamKindTable{{{
    { TypeParamKind::Integer, "Integer" },
    { TypeParamKind::Type,    "Type"    },
}}};
DSS_CHECK_ENUM_NAME_TABLE(kTypeParamKindTable);

[[nodiscard]] constexpr std::string_view
typeParamKindName(TypeParamKind k) noexcept {
    return kTypeParamKindTable.name(k);
}
[[nodiscard]] constexpr std::optional<TypeParamKind>
typeParamKindFromName(std::string_view s) noexcept {
    return kTypeParamKindTable.fromName(s);
}

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
