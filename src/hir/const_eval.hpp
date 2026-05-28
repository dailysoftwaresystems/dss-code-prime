#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "hir/hir_literal_pool.hpp"

#include <cstdint>
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
// CE1 coverage (parity with ML2's previous inline `tryConstFold`):
//   - HirKind::Literal — direct map from HirLiteralPool.
//   - HirKind::UnaryOp(Neg/BitNot) — integer literal in, integer out.
//   - HirKind::BinaryOp — integer arithmetic (Add/Sub/Mul/Div/Rem),
//     bitwise (BitAnd/Or/Xor), shifts (Shl/Shr), comparisons (Eq/Ne/
//     Lt/Le/Gt/Ge). Div-by-zero and out-of-range shift counts refuse to
//     fold per the EvalOptions policy.
//   - HirKind::Cast — narrowing/widening integer cast; retags the value
//     to the target's core kind.
// CE2–CE5 in plan 12.5 will extend: Ref-to-constant-symbol resolution
// (via a callback), short-circuit LogicalAnd/Or/Ternary, allowFloat with
// IEEE 754 policy.

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
    Overflow,                  // refuseOnOverflow (CE1: not detected yet)
    UnsupportedOperator,       // op not modelled at this CE level
    UnsupportedTypeKind,       // e.g. float folding when allowFloat=false
};

// Caller-controlled policy knobs.
struct EvalOptions {
    bool allowFloat              = false;  // CE5 opens this gate
    bool refuseOnOverflow        = true;
    bool refuseOnDivByZero       = true;
    bool refuseOnShiftOutOfRange = true;
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
                 EvalOptions options = {});

} // namespace dss
