#pragma once

#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <string_view>

// HIR operator vocabulary (HR2). A `BinaryOp`/`UnaryOp` node names its operator
// in the node `payload`. The operator space is OPEN, mirroring `HirKind`/
// `TypeKind` (08.5 SP2): a fixed universal CORE in [0, 256) (`HirOpKind`) plus a
// registered-EXTENSION space >= 256 (`HirOpId`s minted by `HirOpRegistry`). The
// core is the paradigm-neutral arithmetic/bitwise/comparison/unary set that
// every imperative language shares; a genuinely novel operator a language needs
// is a registered extension operator, NEVER a new core member — the same
// open-core+extensions discipline that keeps `HirKind`/`TypeKind` fixed.
//
// The operator is LANGUAGE-NEUTRAL: HIR records `Add`, not the source token `+`.
// Short-circuit `&&`/`||`, the conditional `?:`, address-of/deref are NOT
// operators here — they are first-class `HirKind`s (`LogicalAnd`/`LogicalOr`/
// `Ternary`/`AddressOf`/`Deref`) because their evaluation semantics differ from
// a plain operator application.
//
// Payload codec: a core op packs as its `HirOpKind` enum value (< 256); an
// extension op packs as its `HirOpId.v` (>= 256) — identical to how an
// `Extension` `HirKind` node stashes its `HirKindId` in `payload`. So a single
// `payload` slot addresses the whole open operator space; `isCoreOp()` splits it.

namespace dss {

// ── HirOpKind: the open core operator vocabulary [0, 256) ────────────────────
//
// Members are stable ordinals; `Count_` must stay last (it counts the core
// members and pins the < 256 invariant). Extension operators are `HirOpId`s
// >= kFirstHirExtensionOp, minted by `HirOpRegistry`.
enum class HirOpKind : std::uint16_t {
    // ── binary: arithmetic ──
    Add, Sub, Mul, Div, Rem,
    // ── binary: bitwise ──
    BitAnd, BitOr, BitXor, Shl, Shr,
    // ── binary: comparison ──
    Eq, Ne, Lt, Le, Gt, Ge,
    // ── unary ──
    Neg,        // arithmetic negation  (-x)
    Not,        // logical negation     (!x)
    BitNot,     // bitwise complement   (~x)

    Count_      // keep last — counts the core members
};

static_assert(static_cast<std::uint32_t>(HirOpKind::Count_) < 256,
              "core HirOpKind members must occupy [0, 256); extension operators "
              "use registry-minted HirOpIds >= 256");

// First registry-minted extension operator. Core operators (the HirOpKind enum)
// occupy [0, kFirstHirExtensionOp). Mirrors kFirstHirExtensionKind / the type
// lattice's kFirstExtensionKind.
inline constexpr std::uint32_t kFirstHirExtensionOp = 256;

// Operator arity. A core op's arity is fixed by `arityOf`; an extension op
// carries its arity in its `HirOpDescriptor`. The typed builder helpers
// (`makeBinaryOp`/`makeUnaryOp`) assert arity at construction so a malformed
// node can never be built.
enum class HirOpArity : std::uint8_t { Unary, Binary };

// Lowercase label for diagnostics / fatal messages ("binary" / "unary").
[[nodiscard]] constexpr char const* arityLabel(HirOpArity a) noexcept {
    return a == HirOpArity::Binary ? "binary" : "unary";
}

// The arity of a CORE operator. Extension-operator arity lives in the registry
// descriptor, not here.
[[nodiscard]] constexpr HirOpArity arityOf(HirOpKind op) noexcept {
    switch (op) {
        case HirOpKind::Add: case HirOpKind::Sub: case HirOpKind::Mul:
        case HirOpKind::Div: case HirOpKind::Rem:
        case HirOpKind::BitAnd: case HirOpKind::BitOr: case HirOpKind::BitXor:
        case HirOpKind::Shl: case HirOpKind::Shr:
        case HirOpKind::Eq: case HirOpKind::Ne: case HirOpKind::Lt:
        case HirOpKind::Le: case HirOpKind::Gt: case HirOpKind::Ge:
            return HirOpArity::Binary;
        case HirOpKind::Neg: case HirOpKind::Not: case HirOpKind::BitNot:
            return HirOpArity::Unary;
        case HirOpKind::Count_:
            break;  // not a real operator
    }
    return HirOpArity::Binary;  // unreachable for a well-formed core op
}

// ── payload codec ────────────────────────────────────────────────────────────
//
// A node `payload` carrying an operator is split by the [0,256) / >=256 line,
// exactly like the HirKind/HirKindId split for `Extension` nodes.

// True iff `payload` names a core operator (an HirOpKind, < 256). A false result
// means the payload is an extension HirOpId minted by an HirOpRegistry.
[[nodiscard]] constexpr bool isCoreOp(std::uint32_t payload) noexcept {
    return payload < kFirstHirExtensionOp;
}

[[nodiscard]] constexpr std::uint32_t encodeOp(HirOpKind op) noexcept {
    return static_cast<std::uint32_t>(op);
}
[[nodiscard]] constexpr std::uint32_t encodeOp(HirOpId op) noexcept {
    return op.v;
}

// Decode a payload known to name a core op (caller checked `isCoreOp`).
[[nodiscard]] constexpr HirOpKind decodeCoreOp(std::uint32_t payload) noexcept {
    return static_cast<HirOpKind>(payload);
}
// Decode a payload known to name an extension op (caller checked `!isCoreOp`).
[[nodiscard]] constexpr HirOpId decodeExtOp(std::uint32_t payload) noexcept {
    return HirOpId{payload};
}

// Stable symbolic name for a CORE operator — used by the text format (HR7) and
// debug output (`binary_op<Add>`). Extension-operator names live in the registry
// descriptor.
[[nodiscard]] constexpr std::string_view opName(HirOpKind op) noexcept {
    switch (op) {
        case HirOpKind::Add:    return "Add";
        case HirOpKind::Sub:    return "Sub";
        case HirOpKind::Mul:    return "Mul";
        case HirOpKind::Div:    return "Div";
        case HirOpKind::Rem:    return "Rem";
        case HirOpKind::BitAnd: return "BitAnd";
        case HirOpKind::BitOr:  return "BitOr";
        case HirOpKind::BitXor: return "BitXor";
        case HirOpKind::Shl:    return "Shl";
        case HirOpKind::Shr:    return "Shr";
        case HirOpKind::Eq:     return "Eq";
        case HirOpKind::Ne:     return "Ne";
        case HirOpKind::Lt:     return "Lt";
        case HirOpKind::Le:     return "Le";
        case HirOpKind::Gt:     return "Gt";
        case HirOpKind::Ge:     return "Ge";
        case HirOpKind::Neg:    return "Neg";
        case HirOpKind::Not:    return "Not";
        case HirOpKind::BitNot: return "BitNot";
        case HirOpKind::Count_: break;
    }
    return "?";
}

} // namespace dss
