#pragma once

#include "core/export.hpp"
#include "core/types/target_schema.hpp"   // TargetRegClass (synchrony assert)

#include <cstdint>
#include <optional>
#include <string_view>
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
// envelope at the substrate level; finer-grained class info will
// live in the target JSON's `regClasses` section (ML5 cycle 2b).
// The LIR substrate only ever sees this envelope.
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

// Inverse of `lirRegClassName` — consumed by the `.dsslir` text
// parser to recover the class from a `%v.<id>:<class>` suffix.
// Returns `std::nullopt` on an unrecognized name (parser-side fatal).
[[nodiscard]] constexpr std::optional<LirRegClass>
lirRegClassFromName(std::string_view s) noexcept {
    if (s == "none")  return LirRegClass::None;
    if (s == "gpr")   return LirRegClass::GPR;
    if (s == "fpr")   return LirRegClass::FPR;
    if (s == "vr")    return LirRegClass::VR;
    if (s == "flags") return LirRegClass::Flags;
    return std::nullopt;
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
    // Validity discriminator is the CLASS only, not the id. Physical
    // ordinals start at 0 (e.g., x86_64 rax = 0); a class-only check
    // means rax post-regalloc remains valid. For virtual regs the
    // builder mints ids starting at 1, so the convention `id == 0
    // ⇔ virtual sentinel` is producer-enforced rather than type-
    // enforced — code that distinguishes virtual from physical does
    // so via `isPhysical`, not via id.
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

// Synchrony between the LIR substrate's `LirRegClass` and the target-
// schema-side `TargetRegClass` — they must stay numerically aligned so
// the JSON's `"class": "gpr"/"fpr"/..."` strings map to identical
// numeric tags at both ends of the LIR/regalloc/encoding pipeline. A
// future enum addition that lands in only one side fails this assert.
static_assert(static_cast<int>(LirRegClass::None)  == static_cast<int>(TargetRegClass::None));
static_assert(static_cast<int>(LirRegClass::GPR)   == static_cast<int>(TargetRegClass::GPR));
static_assert(static_cast<int>(LirRegClass::FPR)   == static_cast<int>(TargetRegClass::FPR));
static_assert(static_cast<int>(LirRegClass::VR)    == static_cast<int>(TargetRegClass::VR));
static_assert(static_cast<int>(LirRegClass::Flags) == static_cast<int>(TargetRegClass::Flags));

// Factory helpers. The substrate emits virtual regs only; physical-
// reg creation will be gated behind a passkey when the ML6 regalloc
// pass lands. (Cycle 2 of ML5 is the JSON-target pivot; regalloc is
// a separate downstream plan.)
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
