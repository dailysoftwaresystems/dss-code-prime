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

struct LirReg;

// The ONLY two ways to build a register. Declared ahead of `LirReg` so the
// struct can befriend them: they hold the sole key to its private
// field-order-bearing constructor (see "THE AGGREGATE PATH IS SEALED").
// The substrate emits virtual regs only; physical-reg creation will be gated
// behind a passkey when the ML6 regalloc pass lands. (Cycle 2 of ML5 is the
// JSON-target pivot; regalloc is a separate downstream plan.)
[[nodiscard]] constexpr LirReg makeVirtualReg(std::uint32_t id,
                                              LirRegClass cls) noexcept;
[[nodiscard]] constexpr LirReg makePhysicalReg(std::uint32_t ordinal,
                                               LirRegClass cls) noexcept;

// A register operand. Pre-regalloc: `isPhysical == false`, `id` is
// the virtual register number minted by the LIR builder. Post-
// regalloc: `isPhysical == true`, `id` is the target-specific
// physical-register ordinal (e.g. x86_64 rax=0, rcx=1, ...).
struct LirReg {
    std::uint32_t id          : 24;  // virtual number OR physical ordinal
    std::uint32_t classKind   :  6;  // LirRegClass (5 values fit in 6 bits)
    std::uint32_t isPhysical  :  1;

    // ── THE AGGREGATE PATH IS SEALED, AND THAT IS THE WHOLE POINT ─────────
    // D-LIR-POSITIONAL-LIRREG-INIT-MISCLASSIFIES-SILENTLY.
    //
    // ⚠ WHAT THIS TYPE USED TO PERMIT. `LirReg` was a plain aggregate, so
    // `LirReg{ord, 1, cls}` compiled — and **97 sites across 12 test files
    // spelled exactly that**, with the two trailing arguments TRANSPOSED
    // against the declaration order: the literal `1` bound to `classKind`
    // and `cls` bound to the ONE-BIT `isPhysical`. Every one of them was
    // correct, and correct **twice over by coincidence**, because
    // `LirRegClass::GPR` IS `1` and `1` is also `true`. ✔MEASURED
    // 2026-08-27: the identical spelling with `LirRegClass::FPR` (2) yields
    // `classKind = GPR` and `isPhysical = 0` — a virtual GPR where an
    // FPR was written. Reorder or insert ANY enumerator in `LirRegClass`
    // and all 97 change meaning at once, with no compile error and no test
    // failure: the suite would keep passing while asserting about the wrong
    // register class. That is a silent miscompile inside the measuring
    // instrument, which is worse than one in the product — it is the thing
    // that certifies the product.
    //
    // ⚠⚠ AND THE COUNT IS THE SECOND LESSON. A `grep "LirReg{"` sees **17**
    // of those 97. The other **80** are spelled `LirReg const rax{ord, 1,
    // cls}` — direct-initialization of a NAMED variable, where the type and
    // the brace are separated by the variable name, so no textual search
    // for type-adjacent-brace can see them. ✔MEASURED 2026-08-27: grep
    // reported the sweep complete, and the COMPILER then found 80 more.
    // A hazard that only a complete instrument can enumerate is a hazard
    // that has to be closed by construction, never by discipline — which is
    // the argument for this seal over "just remember to use the factories".
    //
    // ★ WHY A CONSTRUCTOR AND NOT A TYPED `classKind`. The obvious fix is to
    // declare `LirRegClass classKind : 6;` so a bare `1` stops converting.
    // ✔MEASURED 2026-08-27 and **REFUTED**: an enum bit-field (underlying
    // type `std::uint8_t`) adjacent to `std::uint32_t` bit-fields is a
    // DIFFERENT declared type, so neither bit-field algorithm packs it into
    // the shared allocation unit — `sizeof` goes **4 → 12** under the GNU
    // layout AND under the MSVC layout (`g++ -mms-bitfields`, which
    // implements MSVC's rule). That breaks `LirOperand`'s 4-byte union arm
    // and the `sizeof(LirReg) == 4` assert below. Declaring one user
    // constructor costs ZERO bits: the layout below is byte-identical to
    // what it replaced, and `is_standard_layout` is preserved because every
    // non-static data member is still public.
    //
    // ⇒ Members stay public and READABLE (`r.id` / `r.isPhysical` are read
    // at 143 sites, including `src/asm/`, which must not churn); only the
    // field-ORDER-bearing write path becomes unreachable. The next omission
    // is a compile error, which is the loudest failure available.
    constexpr LirReg() noexcept = default;

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
    // ★ DEFAULTED — AND IT IS THE SEAL ABOVE THAT MAKES THAT SAFE. This used
    // to be hand-written specifically to exclude a fourth `_pad : 1` member,
    // because "any future construction path" could leave that bit
    // un-zeroed and make two semantically-identical registers compare
    // unequal. There is no longer any such path — every construction runs
    // through the private constructor below, which names every field — so
    // the padding bit needs no name, `==` compares exactly the three
    // semantic fields, and one member, one comment, one hand-written
    // operator and one test that existed only to police it are all gone.
    constexpr bool operator==(LirReg const&) const noexcept = default;

private:
    // The one field-order-bearing write path in the codebase, reachable
    // only by the two factories below. `cls` is TYPED, so the transposition
    // that made the old positional form a coincidence cannot be spelled:
    // an ordinal will not convert to `LirRegClass`, and a `LirRegClass`
    // will not convert to `bool`.
    constexpr LirReg(std::uint32_t regId, LirRegClass cls,
                     bool physical) noexcept
        : id(regId),
          classKind(static_cast<std::uint32_t>(cls)),
          isPhysical(physical ? 1u : 0u) {}

    friend constexpr LirReg makeVirtualReg(std::uint32_t, LirRegClass) noexcept;
    friend constexpr LirReg makePhysicalReg(std::uint32_t, LirRegClass) noexcept;
};
static_assert(sizeof(LirReg) == 4, "LirReg POD must stay 4 bytes");
static_assert(std::is_trivially_copyable_v<LirReg>);
// `LirOperand`'s union arm and `LirRegAssignment`'s `std::variant` both
// default-construct a `LirReg`; a non-trivial default constructor would
// silently make both of those non-trivial too.
static_assert(std::is_trivially_default_constructible_v<LirReg>);
static_assert(std::is_standard_layout_v<LirReg>);

// ★★ THE SEAL IS ITSELF PINNED, ON EVERY LEG, ON EVERY BUILD — and these two
// lines are the red-on-disable arm for
// D-LIR-POSITIONAL-LIRREG-INIT-MISCLASSIFIES-SILENTLY. Delete the
// constructors above and `LirReg` becomes an aggregate again; C++20
// parenthesized aggregate initialization (P0960) means `std::is_constructible`
// SEES that, so the property "a register cannot be built positionally" is a
// compile-time fact rather than a convention someone has to remember.
// ✔MEASURED 2026-08-27 both ways: against the pre-fix struct both traits are
// `true`, against this one both are `false`.
static_assert(!std::is_aggregate_v<LirReg>,
              "LirReg must NOT be an aggregate: aggregate init binds arguments "
              "BY POSITION, and its two middle fields (classKind, isPhysical) "
              "are silently interchangeable whenever the class is GPR — "
              "97 sites were transposed and green");
static_assert(!std::is_constructible_v<LirReg, std::uint32_t, std::uint32_t,
                                       std::uint32_t>,
              "three bare integers must not build a LirReg — use "
              "makeVirtualReg / makePhysicalReg, whose class argument is typed");

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

// Factory definitions (declared above the struct, which befriends them).
[[nodiscard]] constexpr LirReg makeVirtualReg(std::uint32_t id,
                                              LirRegClass cls) noexcept {
    return LirReg{id, cls, /*physical=*/false};
}
[[nodiscard]] constexpr LirReg makePhysicalReg(std::uint32_t ordinal,
                                               LirRegClass cls) noexcept {
    return LirReg{ordinal, cls, /*physical=*/true};
}

// Value-initialized: every bit zero, so `classKind == None` ⇒ `!valid()`.
// This is the same object the old `{0, 0, 0, 0}` produced, spelled in the
// one way that survives the seal.
inline constexpr LirReg InvalidLirReg{};

} // namespace dss
