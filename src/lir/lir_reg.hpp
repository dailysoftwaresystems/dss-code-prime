#pragma once

#include "core/export.hpp"

#include <cstdint>
#include <type_traits>

// LIR register substrate (plan 12 §2.7). Registers carry a CLASS
// (GPR / FPR / VR / FLAGS — per-target sets) and either a virtual
// number (pre-regalloc) or a physical ordinal (post-regalloc).
// `LirReg` is a 4-byte POD that fits as an operand in `LirInst`'s
// operand pool entries; the operand variant tag tells consumers
// whether the slot holds a register, an immediate, or a memory ref.

namespace dss {

// Universal register-class envelope. Each target maps its concrete
// register classes (e.g. x86_64's GPR8/16/32/64 family) to this
// envelope at the substrate level; finer-grained class info lives
// in the target's `RegClass` enum (ML5 cycle 2). Cycle 1 uses the
// universal envelope only.
enum class LirRegClass : std::uint8_t {
    None    = 0,
    GPR     = 1,  // general-purpose integer
    FPR     = 2,  // floating-point
    VR      = 3,  // vector
    Flags   = 4,  // condition flags (single per arch)
};

[[nodiscard]] constexpr std::string_view lirRegClassName(LirRegClass c) noexcept {
    switch (c) {
        case LirRegClass::None:  return "none";
        case LirRegClass::GPR:   return "gpr";
        case LirRegClass::FPR:   return "fpr";
        case LirRegClass::VR:    return "vr";
        case LirRegClass::Flags: return "flags";
    }
    return "none";
}

// A register operand. Pre-regalloc: `isPhysical == false`, `id` is
// the virtual register number minted by the LIR builder. Post-
// regalloc: `isPhysical == true`, `id` is the target-specific
// physical-register ordinal (e.g. x86_64 rax=0, rcx=1, ...).
struct LirReg {
    std::uint32_t id          : 24;  // virtual number OR physical ordinal
    std::uint32_t classKind   :  6;  // LirRegClass (5 values fit in 6 bits)
    std::uint32_t isPhysical  :  1;
    std::uint32_t _pad        :  1;

    [[nodiscard]] constexpr LirRegClass regClass() const noexcept {
        return static_cast<LirRegClass>(classKind);
    }
    [[nodiscard]] constexpr bool valid() const noexcept {
        return classKind != static_cast<std::uint32_t>(LirRegClass::None);
    }
    // Compare semantic fields only — exclude the `_pad` bit. A
    // defaulted `==` would include `_pad` in the comparison; if any
    // future construction path failed to zero-initialize it, two
    // semantically-identical registers would compare unequal.
    constexpr bool operator==(LirReg const& o) const noexcept {
        return id == o.id && classKind == o.classKind && isPhysical == o.isPhysical;
    }
};
static_assert(sizeof(LirReg) == 4, "LirReg POD must stay 4 bytes");
static_assert(std::is_trivially_copyable_v<LirReg>);

// Factory helpers. Cycle 1 needs the virtual variant; cycle 2 (regalloc)
// gates physical-reg creation behind a passkey.
[[nodiscard]] constexpr LirReg makeVirtualReg(std::uint32_t id,
                                              LirRegClass cls) noexcept {
    return LirReg{id, static_cast<std::uint32_t>(cls), 0, 0};
}
[[nodiscard]] constexpr LirReg makePhysicalReg(std::uint32_t ordinal,
                                               LirRegClass cls) noexcept {
    return LirReg{ordinal, static_cast<std::uint32_t>(cls), 1, 0};
}

inline constexpr LirReg InvalidLirReg{0, 0, 0, 0};

} // namespace dss
