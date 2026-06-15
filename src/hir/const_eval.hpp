#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "hir/hir_literal_pool.hpp"

#include <cstdint>
#include <functional>
#include <optional>

// Shared constants-evaluation engine (plan 12.5 CE1). Computes the compile-
// time value of a HIR expression by walking the tree bottom-up. The engine
// is language-blind: callers (MIR-globals lowering today; future D5.5
// enum-discriminant evaluation, v1 optimizer constant-fold pass, HIR
// verifier richer array-length / enum-bounds rules) share one source of
// truth here.
//
// Lives in `src/hir/` because it READS HIR (literals + child structure +
// node types via the interner). Plan 12.5 §2.1 originally named `src/core/
// types/type_lattice/` — corrected because the lattice library must not
// depend on HIR (HIR depends on lattice). The engine RETURNS a
// `HirLiteralValue` (the same variant the HIR uses); MIR callers do a
// trivial field-by-field copy into a `MirLiteralValue` (the two pools
// are structurally identical).
//
// Coverage today (CE1 + CE2 + CE3):
//   - HirKind::Literal — direct map from HirLiteralPool. Pulls bool /
//     int64 / uint64 variant arms uniformly via the `asInt64` bridge.
//   - HirKind::Ref (CE2) — resolves through `EvalOptions::resolveConstSymbol`
//     with stack-discipline visited-symbols cycle safety.
//   - HirKind::UnaryOp(Neg/BitNot) — integer literal in, integer out.
//   - HirKind::BinaryOp — integer arithmetic (Add/Sub/Mul/Div/Rem),
//     bitwise (BitAnd/Or/Xor), shifts (Shl/Shr), comparisons (Eq/Ne/
//     Lt/Le/Gt/Ge). Div-by-zero and out-of-range shift counts refuse to
//     fold per the EvalOptions policy. Result core is the C99-UAC
//     `commonType` of the operands (or Bool for comparisons; or the
//     promoted LHS for shifts per C99 §6.5.7p3) (CE3).
//   - HirKind::Cast (CE3) — target-type-aware integer truncate/extend.
//     Refuses with `Overflow` when the value doesn't fit AND
//     `refuseOnOverflow=true` (D5.5 verifier path); wraps modularly
//     otherwise (MIR-globals path, matches runtime). Cast-to-Bool folds
//     to `N != 0`. Cast to unsigned-≥64 from a negative source refuses
//     regardless of the knob (the int64 arm cannot reconcile signedness).
//   - HirKind::LogicalAnd / HirKind::LogicalOr (CE4) — C99 short-circuit:
//     `0 && x` folds to 0 regardless of whether `x` is foldable;
//     `1 || x` folds to 1 likewise. Without short-circuit
//     `(non_const && x)` correctly reports `x` as non-foldable. Result
//     core is always Bool.
//   - HirKind::Ternary (CE4) — fold cond, then recurse only into the
//     SELECTED arm. `cond ? const : non_const` still folds when cond
//     is true. The selected arm's failure propagates verbatim with its
//     own blame anchor.
//   - Float folding (CE5) — gated by `EvalOptions::allowFloat`. Floats
//     route through `applyUnaryFloat` / `applyBinaryFloat` using host
//     `<cmath>` IEEE 754 semantics (round-to-nearest-even rounding;
//     NaN propagates; ±inf on overflow; comparisons against NaN return
//     false — including `NaN == NaN`). Cast quadrants: int↔float (host
//     conversion; F32 target narrows via `static_cast<float>` round-trip;
//     F16 target narrows via the engine's soft-float `narrowToHalf`
//     helper — IEEE 754 binary16, plan-12.5 §0.2 D1 closed),
//     float→bool (`x != 0.0`, NaN/±inf → true), float→int (truncate
//     toward zero; refuses with `Overflow` for NaN/±inf and for
//     out-of-int64 truncated values — refusal is UNCONDITIONAL of
//     `refuseOnOverflow` because the C99 bit-pattern is
//     implementation-defined; the wrap-knob applies only to the
//     inner-range "fits int64 but not target" case), float→float
//     (F32 / F16 narrow, F64 identity; F128 refuses with
//     `UnsupportedTypeKind` — no host backing AND no F128 arithmetic
//     consumer today; plan-12.5 §0.2 D1b mapped to plan 19).
//     Logical and Ternary cond accept floats too (via the shared
//     `asBool(value, allowFloat)` helper). When `allowFloat=false`
//     any float-involving operand refuses with `UnsupportedTypeKind`.
//     C99-undefined operator+float combinations (BitNot/Not on float,
//     Rem/Bitwise/Shift on float) surface `UnsupportedTypeKind` (the
//     operator IS modelled by the engine; the type is wrong) — the
//     `UnsupportedOperator` code is reserved for the genuine "engine
//     doesn't fold this op yet" case. `refuseOnLossyFloatConversion`
//     (plan-12.5 §0.2 D2 closed) makes int→float Cast refuse with
//     `LossyFloatConversion` when the value can't round-trip through
//     the target precision — for verifier consumers that want
//     "you're losing bits" diagnostics; off by default for codegen.

namespace dss {

class Hir;
class TypeInterner;

// Why a fold did not succeed. `None` means success; the rest are
// callers' diagnostic anchors (the engine emits NO diagnostics itself —
// callers map each cause to the right code).
enum class ConstEvalFailure : std::uint8_t {
    None,
    NotAConstantExpression,    // expression contains non-foldable kinds
    DivisionByZero,            // refuseOnDivByZero (default true)
    ShiftCountOutOfRange,      // refuseOnShiftOutOfRange (default true)
    Overflow,                  // refuseOnOverflow: value computed but does
                               // not fit the target type (CE3: detected on
                               // narrowing Cast and on negative-to-unsigned-
                               // ≥64; arithmetic overflow deferred to a
                               // future cycle alongside float folding)
    UnsupportedOperator,       // op not modelled at this CE level
    UnsupportedTypeKind,       // e.g. float folding when allowFloat=false
    LossyFloatConversion,      // refuseOnLossyFloatConversion: int→double widening
                               // didn't round-trip (≥2^53-ish), caller wants strict
                               // (verifier-evolution consumer, plan-09 mapped item)
};

// Language-blind callback for `Ref`-to-constant-symbol resolution (CE2).
// Given a referenced symbol, return its DEFINING HIR initializer
// expression (e.g. the `1` in `int a = 1;`) so the engine can recurse
// and fold transitively. Return `nullopt` when the symbol is NOT a
// compile-time constant (e.g. a function parameter, a mutable local) —
// the engine surfaces `NotAConstantExpression` blamed at the Ref node.
//
// The callback carries any caller state via the closure (D5.5 enum
// context, MIR-globals' pendingGlobals table, future verifier state)
// without templating the engine or leaking opaque user-data pointers.
//
// Cycle safety: the engine tracks a per-call visited-symbols set so a
// chain like `int a = b; int b = a;` surfaces `NotAConstantExpression`
// at the second encounter rather than infinite-recursing.
using ConstSymbolResolver =
    std::function<std::optional<HirNodeId>(SymbolId)>;

// FC6: the SizeOf-folding resolver the comment below long predicted. Given the
// TypeId being sized (the SizeOf node's type-ref child), return its byte size,
// or `nullopt` when the type is incomplete / un-sizeable / the target declared
// no layout params — the engine then surfaces `NotAConstantExpression`. The
// closure carries the target's `AggregateLayoutParams` + `DataModel` + the
// `computeLayout` engine (kept OUT of `const_eval`, which must not depend on the
// layout library beyond the `TypeId` it already passes); absent closure ⇒ SizeOf
// is non-constant, exactly as before (verifier consumers keep that behaviour).
using TypeSizeResolver =
    std::function<std::optional<std::uint64_t>(TypeId)>;

// Caller-supplied environment — closure-carrying capabilities (resolvers)
// the engine uses to descend into things outside the local subtree. Kept
// SEPARATE from `EvalOptions` (policy bools) so the two structurally
// different concerns don't tangle as more resolvers accrete (architecture-
// review folded item D4 from plan 12.5 §0.2). The SizeOf resolver (`resolveTypeSize`)
// is the second function-typed field this comment anticipated.
struct EvalEnvironment {
    ConstSymbolResolver resolveConstSymbol{};   // CE2
    TypeSizeResolver    resolveTypeSize{};      // FC6 — SizeOf folding
};

// Caller-controlled policy. Pure bool knobs — no closures, no environment.
// Defaults are the "strictest sensible" choice; codegen consumers
// (MIR-globals) relax `refuseOnOverflow` (runtime-equivalent wrap)
// and opt `allowFloat` on; verifier consumers keep defaults.
struct EvalOptions {
    bool allowFloat                  = false;   // gates float folding (CE5)
    bool refuseOnOverflow            = true;
    bool refuseOnDivByZero           = true;
    bool refuseOnShiftOutOfRange     = true;
    // Plan 12.5 §0.2 D2 → mapped to plan 09 verifier evolution. When
    // ON, `asDouble`'s int64/uint64 → double widening checks the
    // round-trip; values that lose precision (≥2^53 magnitude) refuse
    // with `LossyFloatConversion`. OFF by default — codegen consumers
    // want runtime-equivalent silent precision loss.
    bool refuseOnLossyFloatConversion = false;
};

struct ConstEvalResult {
    std::optional<HirLiteralValue> value;
    ConstEvalFailure               failure   = ConstEvalFailure::None;
    HirNodeId                      blamedNode{};  // for diagnostic anchoring
};

// Evaluate `expr` to a compile-time `HirLiteralValue`. Pure function;
// no diagnostics, no HIR mutation. The interner is taken by non-const
// reference to match the rest of the lattice API (and to enable the
// future Cast-fold path's use of `interner.commonType`); CE1 only
// READS from the interner (via `kind`) — no new types are interned.
[[nodiscard]] DSS_EXPORT ConstEvalResult
evaluateConstant(Hir const& hir,
                 TypeInterner& interner,
                 HirLiteralPool const& literals,
                 HirNodeId expr,
                 EvalEnvironment env     = {},
                 EvalOptions     options = {});

} // namespace dss
