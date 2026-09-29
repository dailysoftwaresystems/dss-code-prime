#pragma once

#include "analysis/semantic/semantic_model.hpp"      // SymbolRecord
#include "analysis/semantic/type_rules.hpp"          // detail::type_rules::unsignedIntRank
#include "core/types/type_lattice/core_type.hpp"     // TypeKind
#include "core/types/type_lattice/type_interner.hpp" // TypeInterner
#include "hir/hir_literal_pool.hpp"                   // HirLiteralValue

#include <bit>
#include <cstdint>
#include <optional>

// A named CONSTANT symbol — an enum enumerator OR a shipped-descriptor-injected
// constant (`isInjectedConstant`, integer OR float) — folds its reference to a
// compile-time literal. This is the SINGLE builder shared by every fold site:
//   * the HIR Ref->Const lowering (`cst_to_hir.cpp`), and
//   * both const-eval engines' DIRECT-VALUE arm (`resolveSymbolValue`) — so a
//     constant resolves IDENTICALLY in value position (`int x = CHAR_BIT;`) and
//     constant-expression position (`int a[CHAR_BIT];` / `case INT_MAX:`).
// Funnelling all three through one builder is the §A.5 multi-site discipline: a
// drift in the core-type derivation would silently fold a constant differently
// across positions.

namespace dss {

// Returns the literal for a named-integer-constant symbol, or nullopt when
// `rec` is not such a constant (the caller then takes the ordinary Ref /
// init-expr path). Core-type derivation:
//   * enumerator       — `rec.type` is the ENUM; the literal core is its
//     underlying integer (`scalars[0]`, default I32) — an enumerator's value is
//     its underlying int (C 6.7.2.2).
//   * injected constant — `rec.type` IS the constant's own scalar; the core is
//     that type directly (a `u32`/`u64` constant must NOT collapse to I32, which
//     would miscompile its value; a FLOAT constant — c52 `INFINITY` — derives a
//     float core and reconstructs its `double` from the bit-pattern carrier).
// Signedness mirrors the HIR walker's `isSignedCore` for every integer core
// (unsigned == `unsignedIntRank > 0`); `enumValue` carries the bit-pattern (the
// int64 value for an integer core, the IEEE-754 f64 bits for a float core).
//   ★ P68 round 12 (lane `cs`, the enumeration P1): an enumerator's `rec.type` is no
//     longer always the ENUM. C types an enumeration constant `int` once its list is
//     complete and every value fits (C17 6.4.4.3p2, C23 6.7.3.3p15), and WHILE the list
//     is processed it may be a wider integer type — its constant expression's (`A =
//     0x100000000` is `long` on LP64) or the next rung `previous + 1` needed (p12). Such
//     an enumerator's core is that integer type's kind, exactly as an injected
//     constant's is: defaulting it to I32 folded `B = A + 1` inside the list in the
//     32-bit domain and wrapped it (✔MEASURED on the P1's trial build, `.temp/probe/
//     ectE`: `enum E { A = 0x100000000, B = A + 1 };` gave B == 1; gcc 13.3.0 and clang
//     18.1.3 give 0x100000001).
[[nodiscard]] inline std::optional<HirLiteralValue>
constantLiteralForSymbol(SymbolRecord const& rec, TypeInterner const& interner) {
    if (!(rec.isEnumerator || rec.isInjectedConstant)) return std::nullopt;
    TypeKind core = TypeKind::I32;
    if (rec.type.valid()) {
        TypeKind const tk = interner.kind(rec.type);
        if (tk == TypeKind::Enum) {
            auto const sc = interner.scalars(rec.type);
            if (!sc.empty()) core = static_cast<TypeKind>(sc[0]);
        } else if (rec.isInjectedConstant
                   || detail::type_rules::signedIntRank(tk) != 0
                   || detail::type_rules::unsignedIntRank(tk) != 0) {
            core = tk;
        }
    }
    HirLiteralValue lv;
    lv.core = core;
    // c52 (D-FFI-MATH-INFINITY): a FLOAT-typed injected constant (`INFINITY`)
    // carries its value as the IEEE-754 f64 BIT-PATTERN in `enumValue` (an int64
    // carrier shared with the integer/enumerator path). Reconstruct the `double`
    // and store it in the float arm — the `HirLiteralValue::core ↔ variant-arm`
    // contract requires a float core to hold a `double` (NOT an int64). Only an
    // INJECTED constant can be a float (an enumerator's underlying type is always
    // integer, C 6.7.2.2), so this arm never fires for an enumerator.
    bool const isFloat = (core == TypeKind::F16 || core == TypeKind::F32
                       || core == TypeKind::F64 || core == TypeKind::F80
                       || core == TypeKind::F128);
    if (isFloat) {
        lv.value = std::bit_cast<double>(static_cast<std::int64_t>(rec.enumValue));
    } else if (detail::type_rules::unsignedIntRank(core) == 0) {
        lv.value = static_cast<std::int64_t>(rec.enumValue);
    } else {
        lv.value = static_cast<std::uint64_t>(rec.enumValue);
    }
    return lv;
}

} // namespace dss
