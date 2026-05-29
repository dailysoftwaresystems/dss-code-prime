#pragma once

#include "core/export.hpp"
#include "lir/lir_opcode.hpp"

#include <cstdint>

// x86_64 target for ML5 cycle 1: minimal opcode set + register file
// stub. Cycle 2 (instruction selection) will flesh this out with the
// full ISA subset needed for c-subset corpus coverage (mov-family,
// add/sub/mul/imul/idiv, cmp + cc-jcc family, call, ret, push/pop,
// memory addressing modes). Cycle 1 just proves the substrate types
// instantiate cleanly and a tiny program can be built + read back.

namespace dss::x86_64 {

// x86_64 opcode enum. Cycle 1 minimal: just enough to verify the
// substrate. Each enumerator is the raw `std::uint16_t` value
// stored in `detail::LirInst::opcode`; consumers cast back via
// `static_cast<X86_64Opcode>(lir.instOpcode(id))` only when they
// know the active target is `TargetId::X86_64`.
enum class Opcode : std::uint16_t {
    Invalid = 0,
    Mov     = 1,  // mov dst, src
    Add     = 2,  // add dst, src1, src2 (3-addr LIR; encoder selects 2-addr form)
    Sub     = 3,  // sub dst, src1, src2
    Cmp     = 4,  // cmp src1, src2 (sets FLAGS)
    Jmp     = 5,  // unconditional br
    Jcc     = 6,  // conditional br (payload = condition code)
    Call    = 7,  // call <symbol-or-reg>
    Ret     = 8,  // ret
    Count_         // sentinel — must stay last; pins the < 16 invariant
};

static_assert(static_cast<std::uint16_t>(Opcode::Count_) < 256,
              "x86_64 Opcode must fit comfortably in uint16_t (cycle 1 keeps "
              "the table small; cycle 2 fleshes out the ISA subset)");

// x86_64 condition codes (payload value for `Jcc`).
enum class Cc : std::uint8_t {
    Eq = 0, Ne = 1, Lt = 2, Le = 3, Gt = 4, Ge = 5,
    Below = 6, BelowEq = 7, Above = 8, AboveEq = 9,
};

// Physical-register ordinals. The plan's full ML5 implementation
// gives each x86_64 GPR a distinct ordinal; cycle 1 just declares
// the integer subset (8 64-bit GPRs that don't require a REX
// prefix to address) so regalloc-stub tests have something to
// reference. Cycle 2 expands to the full GPR + FPR set.
enum class GprOrdinal : std::uint32_t {
    Rax = 0, Rcx = 1, Rdx = 2, Rbx = 3,
    Rsp = 4, Rbp = 5, Rsi = 6, Rdi = 7,
};

[[nodiscard]] constexpr LirOpcodeInfo opcodeInfo(Opcode op) noexcept {
    using R = LirResultRule;
    switch (op) {
        case Opcode::Invalid: return {0, 0, 0, 0, R::None,  false, false, "invalid"};
        case Opcode::Mov:     return {1, 1, 0, 0, R::Value, false, false, "mov"};
        case Opcode::Add:     return {2, 2, 0, 0, R::Value, false, false, "add"};
        case Opcode::Sub:     return {2, 2, 0, 0, R::Value, false, false, "sub"};
        case Opcode::Cmp:     return {2, 2, 0, 0, R::None,  false, false, "cmp"};
        case Opcode::Jmp:     return {1, 1, 1, 1, R::None,  true,  true,  "jmp"};
        case Opcode::Jcc:     return {0, kLirUnboundedOperands, 2, 2, R::None, true,  true,  "jcc"};
        case Opcode::Call:    return {1, kLirUnboundedOperands, 0, 0, R::Optional, false, true, "call"};
        case Opcode::Ret:     return {0, 1, 0, 0, R::None,  true,  true,  "ret"};
        case Opcode::Count_:  break;
    }
    return {0, 0, 0, 0, R::None, false, false, "?"};
}

} // namespace dss::x86_64
