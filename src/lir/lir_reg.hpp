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

// ── THE SPELLINGS HAVE ONE OWNER, AND IT IS `kTargetRegClassTable` ────────
// D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET.
//
// ★★ WHAT THIS REPLACED, AND WHY THE EXISTING PIN DID NOT COVER IT. These two
// helpers were a hand-written `switch` and a hand-written if-chain over exactly
// the five spellings `kTargetRegClassTable` already owns — three owners of one
// vocabulary. The `static_assert`s below have pinned the two enums since ML5,
// and they pin **VALUES ONLY**: rename the vector class's spelling in the target
// table and every one of them still passes, while `.target.json` starts
// declaring a class
// the `.dsslir` text parser refuses and the `.dsslir` writer emits a name the
// target loader refuses. Two file formats silently disagreeing, with a green
// suite — the exact failure mode this class describes.
//
// ★ WHY DELEGATE RATHER THAN GIVE `LirRegClass` ITS OWN TABLE + AN EQUALITY
// ASSERT. The tiers ARE separate and stay separate: two ENUMS, two tiers, and
// the include direction is unchanged (this header already included
// `target_schema.hpp` for the synchrony pin, and `core/` still does not know
// LIR exists). But the SPELLING SET is not two facts — the header comment on
// `TargetRegClass` has always said so in words ("both enums declare the same
// set"), and the whole point of `%v.3:gpr` matching `"class": "gpr"` is that a
// register class has ONE name across the pipeline. A second table plus an
// assert that the two agree is still two owners with a gate bolted on; the bar's
// ruling is that a fact with an owner does not get a second owner. So the enum
// stays LIR-tier and the names come from the one table that has them.
//
// ⚠ The `unknown` fall-back is UNCHANGED. `kTargetRegClassTable`'s row 0 is
// `{None, "none"}`, so `EnumNameTable::name`'s row-0 fall-back renders exactly
// the `"none"` this switch used to return for an out-of-range value.
[[nodiscard]] constexpr std::string_view lirRegClassName(LirRegClass c) noexcept {
    // The `-Werror=switch` backstop, and it owns no spelling: a new LIR class
    // with no `case` fails the BUILD rather than becoming a value whose name
    // silently comes back as `none`.
    switch (c) {
        case LirRegClass::None:
        case LirRegClass::GPR:
        case LirRegClass::FPR:
        case LirRegClass::VR:
        case LirRegClass::Flags:
            break;
    }
    return targetRegClassName(static_cast<TargetRegClass>(c));
}

// Inverse of `lirRegClassName` — consumed by the `.dsslir` text
// parser to recover the class from a `%v.<id>:<class>` suffix.
// Returns `std::nullopt` on an unrecognized name (parser-side fatal).
[[nodiscard]] constexpr std::optional<LirRegClass>
lirRegClassFromName(std::string_view s) noexcept {
    auto const t = targetRegClassFromName(s);
    if (!t.has_value()) return std::nullopt;
    return static_cast<LirRegClass>(*t);
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

// ★★ AND THE TOTALITY OF THE MAPPING, WHICH THE FIVE ABOVE DO NOT COVER.
// Each of them names ONE pair, so they say nothing about a SIXTH row appearing
// in `kTargetRegClassTable`: `targetRegClassFromName` would resolve it, this
// header's `lirRegClassFromName` would hand the caller a `static_cast` of a
// value no `LirRegClass` enumerator has, and `LirReg::classKind` would carry a
// number the LIR tier cannot name. The `-Werror=switch` backstop in
// `lirRegClassName` catches the reverse direction (a new LIR class), and this
// catches the forward one — so neither table can grow alone.
//
// It walks the ROWS, so it needs no maintenance when a class is added: the new
// row either has a numerically-equal `LirRegClass` (and its spelling round-trips
// through both helpers) or this build stops.
static_assert([] {
    for (auto const& row : kTargetRegClassTable.rows) {
        auto const back = lirRegClassFromName(row.second);
        if (!back.has_value()) return false;                 // spelling lost
        if (static_cast<int>(*back) != static_cast<int>(row.first)) return false;
        if (lirRegClassName(*back) != row.second) return false;  // round trip
    }
    return true;
}(), "every kTargetRegClassTable row must map to a numerically-equal "
     "LirRegClass whose name round-trips — the LIR tier and the target tier "
     "share ONE spelling set, and a row present in only one of them makes the "
     ".dsslir text format and .target.json disagree in silence");

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
