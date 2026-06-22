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

// FC8 bitfields (D-CSUBSET-BITFIELD / D-CSUBSET-BITFIELD-ABI-EXACT): the per-ABI
// bit-field PACKING strategy. A CLOSED, config-declared enum — the layout engine
// switches on THIS, never on a target/format name (the `ScalarAlignmentRule` /
// `AggregateClassKind` precedent). C bit-field allocation is genuinely ABI-defined
// (notably MS straddling vs GNU/SysV), so the rule is config-declared, never
// hardcoded. The strategy is FORMAT/ABI-determined (one CPU target — x86_64 —
// serves BOTH ELF-SysV and PE-MS), so it is selected from the active object
// FORMAT (ELF → gnu_packed, PE → msvc_straddle, Mach-O → gnu_packed) with the
// TARGET's value as the back-compat fallback for direct-API / test callers that
// have no format in scope. `None` ⇒ neither declared a strategy ⇒ a struct WITH
// a bit-field FAILS LOUD at layout (so a missing rule can never silently bake a
// wrong bit placement); a struct with no bit-field never consults it (every
// pre-bitfield layout is unchanged).
//
// D-CSUBSET-BITFIELD-ABI-EXACT (this cycle): both realized strategies are now
// byte-ABI-EXACT to their platform's native compiler, verified by a structural
// conformance witness (dss's computed sizeof/bit-offsets == the native
// compiler's — cl.exe for MSVC, gcc for SysV, clang for Apple via the macOS leg):
//   * GnuPacked   — SysV/Itanium/GNU/AAPCS64/Apple little-endian. (Apple arm64
//     does NOT diverge from generic AAPCS64 on bit-field PACKING — Apple's
//     enumerated arm64 divergences are char/wchar_t signedness, long double=double,
//     va_list, the red zone, stack argument area, and empty-struct handling, NOT
//     bit-field allocation; so Mach-O uses gnu_packed and the macOS clang leg
//     CONFIRMS it — no separate Apple strategy is needed.)
//   * MsvcStraddle — Microsoft x64 (PE). MSVC starts a NEW allocation unit, aligned
//     to the new field's declared-type natural alignment, whenever the next
//     bit-field's declared-type SIZE differs from the current open unit's type size
//     (it does NOT pack different-sized declared types into one unit), AND whenever
//     an ordinary field or a zero-width field intervenes. The struct size covers
//     the LAST unit's full declared-type width. Derived empirically from cl.exe
//     14.51 (e.g. `{int a:1; char b:1;}` is sizeof 8 with b@byte4, vs gnu_packed's
//     4 with b@bit1; `{char a:7; int b:25;}` is sizeof 8 with b@byte4 vs 4).
enum class BitFieldStrategy : std::uint8_t {
    None         = 0,  // not declared → fail-loud when a bit-field is laid out
    GnuPacked    = 1,  // SysV/Itanium/GNU/AAPCS64/Apple little-endian: LSB-first
                       // packing into the field's declared-type storage unit; a
                       // field that would cross its type's unit boundary starts at
                       // the next aligned unit; a zero-width unnamed field forces
                       // the next field to the unit boundary. Different-typed
                       // adjacent bit-fields may SHARE a unit.
    MsvcStraddle = 2,  // Microsoft x64 (PE): each bit-field allocation unit is
                       // aligned to its declared-type natural alignment; a unit is
                       // shared with the previous bit-field ONLY when the declared-
                       // type size matches AND the bits fit; any type-size change,
                       // intervening ordinary field, zero-width field, or straddle
                       // opens a FRESH type-aligned unit at the high-water mark; the
                       // struct size covers the last unit's full declared-type width.
};

[[nodiscard]] constexpr std::string_view
bitFieldStrategyName(BitFieldStrategy s) noexcept {
    switch (s) {
        case BitFieldStrategy::None:         return "none";
        case BitFieldStrategy::GnuPacked:    return "gnu_packed";
        case BitFieldStrategy::MsvcStraddle: return "msvc_straddle";
    }
    return {};
}
[[nodiscard]] constexpr std::optional<BitFieldStrategy>
bitFieldStrategyFromName(std::string_view s) noexcept {
    if (s == "none")          return BitFieldStrategy::None;
    if (s == "gnu_packed")    return BitFieldStrategy::GnuPacked;
    if (s == "msvc_straddle") return BitFieldStrategy::MsvcStraddle;
    return std::nullopt;
}

// FC12b (D-FC12B-WIN64-VARIADIC-CALLEE): the per-CC va_list TYPE + va_start/va_arg/
// va_end lowering STRATEGY. A CLOSED, config-declared enum — every va seam
// (semantic `va_list` type injection, HIR→MIR lowering, LIR prologue spill)
// switches on THIS, never on cc.name / arch / format identity (the established
// `AggregateClassKind` / `BitFieldStrategy` precedent). Lives HERE (NOT
// target_schema.hpp) for the same layering reason `AggregateClassKind` does: the
// SEMANTIC va_list-type injection reads the strategy to size the `ap` local WITHOUT
// pulling the link/target substrate. The geometry FIELDS the strategy gates live on
// `VaListLayout` (target_schema.hpp); the SysV arm reads them, Win64 reads only
// `namedArgSlotBytes`, AAPCS64 is fail-loud-until-FC12c.
//
//   * SysVRegisterSave   — SysV AMD64 §3.5.7: `__va_list_tag[1]` (24B), a
//     register-save-area the prologue spills the arg regs into, and a per-class
//     gp_offset/fp_offset reg-vs-overflow walk.
//   * HomogeneousPointer — Microsoft x64: `va_list` is a plain `char*` (8B). The
//     named args' home space (rcx/rdx/r8/r9 slots) and the stack overflow are
//     CONTIGUOUS in the caller's outgoing area, so `va_arg` is a LINEAR pointer bump
//     by `namedArgSlotBytes` (8) — no register-save-area, no diamond. The callee
//     prologue spills the named integer arg regs into the home slots. FP varargs
//     are read from the home (GPR) slot, so the CALLER duplicates each FP vararg
//     into the matching integer register (lir_callconv, strategy-gated).
//   * Aapcs64DualCursor  — AAPCS64 (ARM64-ELF, FC12c): the 5-field `__va_list`
//     {void* __stack; void* __gr_top; void* __vr_top; int __gr_offs; int __vr_offs;}
//     (32B). The prologue spills x0..x7 (GR, 8×8) then v0..v7 (VR, 8×16) into a
//     callee-local register-save-area; `va_arg` runs a PER-CLASS dual cursor: a
//     NEGATIVE byte offset (__gr_offs/__vr_offs) counts up toward 0 from the head of
//     that class's save block — while < 0 a register slot remains (read
//     `<gr|vr>_top + offs`, sign-extend the i32 cursor, bump by the slot stride);
//     once 0 it walks `__stack` (bump by the 8-byte NSAA quantum). (Apple arm64 does
//     NOT use this: it ships HomogeneousPointer + `variadicArgsAlwaysStack` —
//     varargs are ALWAYS stacked, so a plain pointer-bump over the overflow area
//     suffices; see `variadicUsesOverflowBase`.)
enum class VaListStrategy : std::uint8_t {
    SysVRegisterSave   = 0,  // SysV AMD64 §3.5.7 register-save-area + per-class walk
    HomogeneousPointer = 1,  // Win64 + Apple arm64: a pointer into a contiguous arg area
    Aapcs64DualCursor  = 2,  // AAPCS64 ARM64-ELF dual gr/vr cursor `__va_list` (FC12c)
};

[[nodiscard]] constexpr std::string_view
vaListStrategyName(VaListStrategy s) noexcept {
    switch (s) {
        case VaListStrategy::SysVRegisterSave:   return "sysv_register_save";
        case VaListStrategy::HomogeneousPointer: return "homogeneous_pointer";
        case VaListStrategy::Aapcs64DualCursor:  return "aapcs64_dual_cursor";
    }
    return {};
}
[[nodiscard]] constexpr std::optional<VaListStrategy>
vaListStrategyFromName(std::string_view s) noexcept {
    if (s == "sysv_register_save")  return VaListStrategy::SysVRegisterSave;
    if (s == "homogeneous_pointer") return VaListStrategy::HomogeneousPointer;
    if (s == "aapcs64_dual_cursor") return VaListStrategy::Aapcs64DualCursor;
    return std::nullopt;
}

// The per-ABI aggregate-layout parameter block parsed from `.target.json`. A
// default-constructed value is NOT valid (scalarAlignment 0 / maxAlignment 0 —
// the loader requires both; validate() rejects a zero/non-pow2 maxAlignment).
struct AggregateLayoutParams {
    ScalarAlignmentRule scalarAlignment{};  // required
    std::uint32_t       maxAlignment = 0;   // required; power of two, [1, 256]
    // FC8 bitfields: the bit-field packing strategy (default None = not declared
    // → fail-loud only when a bit-field is actually laid out). Consulted ONLY for
    // a struct containing a bit-field, so a target that omits it keeps every
    // existing (bitfield-free) layout byte-identical.
    BitFieldStrategy    bitFieldStrategy = BitFieldStrategy::None;
};

} // namespace dss
