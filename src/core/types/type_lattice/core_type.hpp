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
    F16, F32, F64, F128,
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
