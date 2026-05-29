#pragma once

#include "core/export.hpp"

#include <cstdint>
#include <string_view>

// LIR opcode substrate (plan 12 §2.6). Per-target instruction sets,
// runtime-tagged with `TargetId`. Each target maintains its own
// closed `*Opcode` enum (e.g. `X86_64Opcode` in `targets/x86_64.hpp`,
// `ARM64Opcode` in `targets/arm64.hpp`). The Lir's instruction PODs
// store the opcode as a raw `std::uint16_t` so we can share the same
// arena layout across targets — the `Lir::targetId()` accessor tells
// consumers which enum to cast to.
//
// **Why runtime-tagged and not C++-templated `Lir<TargetTraits>`?**
// The plan §2.6 calls for "per-target" instruction sets, which the
// template approach implements with compile-time safety (passing an
// x86_64 LIR to ARM64 code is a type error). Runtime tagging trades
// that compile-time check for: (a) much simpler substrate (no
// template instantiation in arena container / attribute / verifier),
// (b) target-blind passes (text format, future optimizer
// transformations) that don't need to be re-instantiated per target,
// (c) one well-defined storage layout for hosting tools (debuggers,
// profilers) that walk the LIR.

namespace dss {

// Target tag carried on every Lir module. The opcode field in each
// `detail::LirInst` is interpreted via the active `TargetId`.
// Closed enum; adding a new backend means adding a target tag + its
// per-target opcode enum.
enum class TargetId : std::uint8_t {
    Invalid = 0,
    X86_64  = 1,
    ARM64   = 2,
};

[[nodiscard]] constexpr std::string_view targetName(TargetId t) noexcept {
    switch (t) {
        case TargetId::Invalid: return "invalid";
        case TargetId::X86_64:  return "x86_64";
        case TargetId::ARM64:   return "arm64";
    }
    return "invalid";
}

// Each target's opcode enum implements this informal "concept": an
// `enum class : std::uint16_t` with member 0 named `Invalid` (so a
// default-constructed `LirInst` carries a visibly-bogus opcode and
// a `kCount` sentinel for table sizing. See `targets/x86_64.hpp` for
// the canonical pattern.

// Result-type discipline mirrors MirResultRule. A LIR instruction
// either defines a value (then its result `VirtualReg` is meaningful),
// or doesn't (terminators, stores).
enum class LirResultRule : std::uint8_t {
    None,      // never defines a value — `result` reg field is unused
    Value,     // always defines a value
    Optional,  // may define a value (e.g. a call to a non-void fn)
};

// Per-opcode shape descriptor — single source of truth that the
// builder, verifier, text format, and instruction selector all read.
// Mirrors `MirOpcodeInfo`'s shape; each target provides its own
// `opcodeInfo(<TargetOpcode>) → LirOpcodeInfo` lookup.
struct LirOpcodeInfo {
    std::uint8_t     minOperands;
    std::uint8_t     maxOperands;     // 0xFF = variadic
    std::uint8_t     minSuccessors;   // CFG successor count (terminators only)
    std::uint8_t     maxSuccessors;   // 0xFF = variadic
    LirResultRule    result;
    bool             isTerminator;
    bool             hasSideEffects;
    std::string_view mnemonic;        // for .dsslir text + diagnostics
};

inline constexpr std::uint8_t kLirUnboundedOperands   = 0xFF;
inline constexpr std::uint8_t kLirUnboundedSuccessors = 0xFF;

} // namespace dss
