#pragma once

#include "core/export.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

// ── Aggregate layout parameters (FC6, plan 23 — the D-FF3-1 layout half) ──
//
// The per-ABI PARAMETERS the generic `type_layout` engine reads to compute
// struct/union/array byte layout (field offsets, alignment, padding, total
// size). Declared as an `"aggregateLayout"` block on a `.target.json`. It is
// OPTIONAL at load (a minimal target — e.g. an inline-JSON test fixture, or a
// non-aggregate-emitting target — may omit it, exactly as `callingConventions`
// / `registers` are relaxed); the fail-loud lives at the CONSUMER, not the
// loader: the layout/`sizeof` path asserts `target.aggregateLayoutLoaded()` and
// emits a positioned diagnostic (no artifact) when it is absent, so a silent
// default can NEVER bake a wrong alignment rule. The engine NEVER branches on
// the target name — only on these declared params, so a future ABI with
// different rules (i386 `double`→4-byte alignment, a packed ABI) is a config
// change, not an engine change.
//
// THE AGNOSTICISM LOCUS (plan-23's "C2 fix"): this is parameters-in-config +
// a bounded natural-alignment ALGORITHM in the engine — NOT a fiction that the
// whole algorithm is data. Two params, both read on EVERY layout (neither inert):
//   * scalarAlignment — the rule mapping a scalar's byte size to its alignment.
//     `Natural` = align == size (capped at maxAlignment); the single knob a
//     non-natural ABI flips. Pointer size+align both come from the format's
//     `dataModel` (the OS-dependent width), aligned by the SAME natural rule.
//   * maxAlignment — the ISA's largest fundamental alignment (the cap applied to
//     every scalar align; x86_64/arm64 = 16 for __int128/long double/max_align_t).
//
// Lives under `core/types/` (NOT `target_schema.hpp`) for the same reason
// `data_model.hpp` does: the lattice/layout engine speaks it without pulling the
// link/target substrate; target_schema.hpp INCLUDES this to store the parsed block.

namespace dss {

// The rule mapping a scalar type's byte size to its required alignment. Closed
// enum (the engine switches on it; a non-natural ABI adds a member, never a
// target-name branch). `Natural` covers all four current ABIs
// (SysV / Win64 / AAPCS64 / Apple); the door is reserved for an `Explicit`
// per-primitive table when a real non-natural target fires the trigger.
enum class ScalarAlignmentRule : std::uint8_t {
    Natural = 1,  // align(scalar) = min(byteSize(scalar), maxAlignment)
};

[[nodiscard]] constexpr std::string_view
scalarAlignmentRuleName(ScalarAlignmentRule r) noexcept {
    switch (r) {
        case ScalarAlignmentRule::Natural: return "natural";
    }
    return {};
}
[[nodiscard]] constexpr std::optional<ScalarAlignmentRule>
scalarAlignmentRuleFromName(std::string_view s) noexcept {
    if (s == "natural") return ScalarAlignmentRule::Natural;
    return std::nullopt;
}

// FC7 by-value aggregate ABI (D-FC7-STRUCT-BY-VALUE-ARG-RETURN): the per-CC
// discipline for classifying a struct/union passed or returned BY VALUE. A
// CLOSED, config-declared strategy enum — the realization tier (the
// `aggregate_abi` classifier) switches on THIS, never on `cc.name` / target /
// format identity (the established `ScalarAlignmentRule` / `slotAligned`
// precedent). `None` ⇒ the CC has no by-value aggregate support built yet ⇒
// FAIL-LOUD (so an un-built CC can never silently mis-pass a struct). Phased:
// SysVEightbyte (C1) is live; Win64BySize (C2) + Aapcs64Hfa (C3) are
// declared-but-not-yet-realized (the strategy-availability guard keeps them
// loud until built). Lives here (NOT target_schema.hpp) so the HIR→MIR lowering
// config + the classifier speak it without pulling the link/target substrate.
enum class AggregateClassKind : std::uint8_t {
    None          = 0,  // unsupported → fail-loud
    SysVEightbyte = 1,  // System V AMD64 §3.2.3: eightbyte INTEGER/SSE classes
    Win64BySize   = 2,  // MS x64: ≤8B (power-of-2) in 1 reg, else by reference
    Aapcs64Hfa    = 3,  // AAPCS64/Apple: HFA in SIMD, ≤16B in X regs, x8 sret
};

[[nodiscard]] constexpr std::string_view
aggregateClassKindName(AggregateClassKind k) noexcept {
    switch (k) {
        case AggregateClassKind::None:          return "none";
        case AggregateClassKind::SysVEightbyte: return "sysv_eightbyte";
        case AggregateClassKind::Win64BySize:   return "win64_by_size";
        case AggregateClassKind::Aapcs64Hfa:    return "aapcs64_hfa";
    }
    return {};
}
[[nodiscard]] constexpr std::optional<AggregateClassKind>
aggregateClassKindFromName(std::string_view s) noexcept {
    if (s == "none")           return AggregateClassKind::None;
    if (s == "sysv_eightbyte") return AggregateClassKind::SysVEightbyte;
    if (s == "win64_by_size")  return AggregateClassKind::Win64BySize;
    if (s == "aapcs64_hfa")    return AggregateClassKind::Aapcs64Hfa;
    return std::nullopt;
}

// The per-ABI aggregate-layout parameter block parsed from `.target.json`. A
// default-constructed value is NOT valid (scalarAlignment 0 / maxAlignment 0 —
// the loader requires both; validate() rejects a zero/non-pow2 maxAlignment).
struct AggregateLayoutParams {
    ScalarAlignmentRule scalarAlignment{};  // required
    std::uint32_t       maxAlignment = 0;   // required; power of two, [1, 256]
};

} // namespace dss
