#pragma once

#include "core/export.hpp"
#include "core/types/enum_name_table.hpp"   // EnumNameTable<E,N> — dependency-free leaf

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

// ── THE ONE OWNER OF THIS FILE'S FOUR CLOSED VOCABULARIES ────────────────────
//
// Each of the four strategy enums below used to spell its names TWICE — a
// `switch` returning literals and an `if`-chain comparing the same literals —
// and every loader message that advertised the accepted set spelled them a
// THIRD time (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET).
// Three owners of one fact, and nothing forcing them to agree: ✔MEASURED
// 2026-08-20, `target_schema_json.cpp`'s bitFieldStrategy refusal named ONE of
// the three spellings its own check accepts, and had done so since the enum
// gained its second realized strategy. Adding an enumerator is now one table
// row; the helpers and every diagnostic follow it automatically.
//
// The shape is `kDataModelTable`'s, including the `-Werror=switch` backstop:
// the table owns the spellings, the switch names only the ENUMERATORS, so a new
// enumerator without a row still fails the BUILD.
inline constexpr EnumNameTable<ScalarAlignmentRule, 1> kScalarAlignmentRuleTable{{{
    { ScalarAlignmentRule::Natural, "natural" },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kScalarAlignmentRuleTable);

[[nodiscard]] constexpr std::string_view
scalarAlignmentRuleName(ScalarAlignmentRule r) noexcept {
    // The `-Werror=switch` backstop — it owns no spelling. See `dataModelName`.
    switch (r) {
        case ScalarAlignmentRule::Natural:
            break;
    }
    return kScalarAlignmentRuleTable.nameOrEmpty(r);
}
[[nodiscard]] constexpr std::optional<ScalarAlignmentRule>
scalarAlignmentRuleFromName(std::string_view s) noexcept {
    return kScalarAlignmentRuleTable.fromName(s);
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

// ★ `None` IS A ROW HERE, and deliberately so — the `ObjectFormatKind::Unknown`
// discipline, not the `LongDoubleFormat::None` one. A calling convention with no
// by-value aggregate support yet is a thing a `.target.json` may STATE, and the
// realization tier turns that statement into a fail-loud at classification. It
// is therefore SELECTABLE, and the whole table is what a message here must
// advertise. (Contrast `BitFieldStrategy` below, whose `none` a FORMAT may not
// select because the format's value participates in a fallback chain.)
inline constexpr EnumNameTable<AggregateClassKind, 4> kAggregateClassKindTable{{{
    { AggregateClassKind::None,          "none"           },
    { AggregateClassKind::SysVEightbyte, "sysv_eightbyte" },
    { AggregateClassKind::Win64BySize,   "win64_by_size"  },
    { AggregateClassKind::Aapcs64Hfa,    "aapcs64_hfa"    },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kAggregateClassKindTable);

[[nodiscard]] constexpr std::string_view
aggregateClassKindName(AggregateClassKind k) noexcept {
    // The `-Werror=switch` backstop — it owns no spelling. See `dataModelName`.
    switch (k) {
        case AggregateClassKind::None:
        case AggregateClassKind::SysVEightbyte:
        case AggregateClassKind::Win64BySize:
        case AggregateClassKind::Aapcs64Hfa:
            break;
    }
    return kAggregateClassKindTable.nameOrEmpty(k);
}
[[nodiscard]] constexpr std::optional<AggregateClassKind>
aggregateClassKindFromName(std::string_view s) noexcept {
    return kAggregateClassKindTable.fromName(s);
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
//   * GnuPacked   — SysV/Itanium/GNU/AAPCS64/Apple little-endian, for the PACKING
//     of bit-fields that have storage: LSB-first into the declared-type unit, with
//     the same straddle bump. Mach-O and both ELF families share this strategy and
//     one Apple strategy enumerator is still not needed.
//     ⚠⚠ THIS COMMENT USED TO ASSERT THAT APPLE arm64 DOES NOT DIVERGE FROM GENERIC
//     AAPCS64 ON BIT-FIELD ALLOCATION AT ALL, AND THAT IS MEASURED FALSE. It does
//     diverge, on the alignment an UNNAMED bit-field contributes:
//         `struct { char c; unsigned :0; char d; }`
//         Apple arm64 5/1 · Apple x86_64 5/1 · x86_64-linux 5/1  (contributes NOTHING)
//         aarch64-linux 8/4 · arm-linux-gnueabihf 8/4            (contributes 4)
//     ✔MEASURED P45: Apple clang 21.0.0 via `/usr/bin/clang -arch arm64` on the
//     physical macOS host (`_Static_assert` on sizeof AND _Alignof, rc=0), and
//     independently by gcc 13.3.0 + clang 18.1.3 per target — the two references
//     agree with EACH OTHER per target rather than with each other across targets,
//     which is what makes it an ABI property and not a compiler quirk.
//     ⇒ That axis is NOT part of this enum. It is its own config key,
//     `UnnamedBitFieldAlignment` below, because it cuts ACROSS gnu_packed rather
//     than selecting between strategies. See D-CSUBSET-ZERO-WIDTH-BITFIELD-ALIGNMENT.
//     ★ The stale sentence is recorded rather than deleted because it was load-bearing:
//     it is why the divergence went unmodelled, and a `$comment` asserting a settled
//     answer gets probed LESS than one in code.
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

// ★ `none` IS A ROW — the `ObjectFormatKind::Unknown` discipline that
// `object_format_kind.hpp` names after THIS vocabulary. The sentinel keeps a
// spelling so a site that must refuse it can tell "you wrote the reserved word"
// apart from "you wrote a spelling nobody claims" — two different author
// mistakes earning two different diagnostics. The two consumers differ, and
// each message must state ITS OWN site's accepted set:
//   * a `.target.json` `aggregateLayout` may write `none` (it means exactly
//     what omitting the key means: undeclared) ⇒ the whole table;
//   * a `.format.json` may NOT (the format's value FALLS BACK to the target's
//     when absent, so `none` would be ambiguous between "unset, use the
//     target's" and "override the target with nothing") ⇒ the SELECTABLE
//     projection below, and an explicit sentinel refusal at that site.
inline constexpr EnumNameTable<BitFieldStrategy, 3> kBitFieldStrategyTable{{{
    { BitFieldStrategy::None,         "none"          },
    { BitFieldStrategy::GnuPacked,    "gnu_packed"    },
    { BitFieldStrategy::MsvcStraddle, "msvc_straddle" },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kBitFieldStrategyTable);

[[nodiscard]] constexpr bool
isSelectableBitFieldStrategy(BitFieldStrategy s) noexcept {
    return s != BitFieldStrategy::None;
}

// The spellings a site that refuses the sentinel accepts — the table minus
// `none`. A new strategy, or a second unselectable enumerator, makes this
// initializer a COMPILE ERROR rather than a list that quietly stops matching
// what the check takes — but ONLY because of the two lines below TOGETHER.
//
// ⚠⚠ THE COUNT USED TO BE `kBitFieldStrategyTable.rows.size() - 1`, AND THAT
// SPELLING COULD NOT FAIL. D-CORE-NAMESWHERE-COUNT-DERIVED-FROM-THE-TABLE-IS-A-TAUTOLOGY.
// `namesWhere<M>` compares `M` against the rows the predicate ACCEPTS; deriving
// `M` from the same table the predicate walks makes both sides move together,
// so a fourth strategy compiled clean while the sentence above claimed it would
// not. ✔MEASURED with `g++ -std=c++23 -fsyntax-only` over a nine-arm probe
// (written out at `kSelectableExitMechanismNames` in `core/types/target_schema.hpp`):
// the literal count reds on a new SELECTABLE enumerator and NOT on a second
// UNSELECTABLE one, which is the case the derived form did catch — so the
// literal alone moves the blind spot rather than closing it, and the
// `static_assert` pinning the REJECTED-row count is what makes the claim true
// in both directions.
inline constexpr auto kSelectableBitFieldStrategyNames =
    namesWhere<2>(kBitFieldStrategyTable, isSelectableBitFieldStrategy);
static_assert(kBitFieldStrategyTable.rows.size()
                  == kSelectableBitFieldStrategyNames.size() + 1,
              "kBitFieldStrategyTable must have exactly ONE unselectable row "
              "(the 'none' sentinel) — a second one leaves `namesWhere`'s "
              "literal count matching while the `.format.json` refusal silently "
              "stops naming the set the check accepts");

[[nodiscard]] constexpr std::string_view
bitFieldStrategyName(BitFieldStrategy s) noexcept {
    // The `-Werror=switch` backstop — it owns no spelling. See `dataModelName`.
    switch (s) {
        case BitFieldStrategy::None:
        case BitFieldStrategy::GnuPacked:
        case BitFieldStrategy::MsvcStraddle:
            break;
    }
    return kBitFieldStrategyTable.nameOrEmpty(s);
}
[[nodiscard]] constexpr std::optional<BitFieldStrategy>
bitFieldStrategyFromName(std::string_view s) noexcept {
    return kBitFieldStrategyTable.fromName(s);
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

// No sentinel: every enumerator is a strategy a `.target.json` may select, so
// the whole table is what a message here advertises.
inline constexpr EnumNameTable<VaListStrategy, 3> kVaListStrategyTable{{{
    { VaListStrategy::SysVRegisterSave,   "sysv_register_save"  },
    { VaListStrategy::HomogeneousPointer, "homogeneous_pointer" },
    { VaListStrategy::Aapcs64DualCursor,  "aapcs64_dual_cursor" },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kVaListStrategyTable);

[[nodiscard]] constexpr std::string_view
vaListStrategyName(VaListStrategy s) noexcept {
    // The `-Werror=switch` backstop — it owns no spelling. See `dataModelName`.
    switch (s) {
        case VaListStrategy::SysVRegisterSave:
        case VaListStrategy::HomogeneousPointer:
        case VaListStrategy::Aapcs64DualCursor:
            break;
    }
    return kVaListStrategyTable.nameOrEmpty(s);
}
[[nodiscard]] constexpr std::optional<VaListStrategy>
vaListStrategyFromName(std::string_view s) noexcept {
    return kVaListStrategyTable.fromName(s);
}

// D-CSUBSET-ZERO-WIDTH-BITFIELD-ALIGNMENT: does an UNNAMED bit-field contribute its
// declared type's alignment to the enclosing composite?
//
// ★★ WHY THIS IS ITS OWN AXIS AND NOT A `BitFieldStrategy` ROW. It cuts ACROSS
// gnu_packed instead of selecting between strategies: `elf64-x86_64-linux` and
// `elf64-aarch64-linux` are the SAME strategy, the same LSB-first packing, the same
// straddle bump — and they answer THIS question differently. A new strategy
// enumerator would have to duplicate every other gnu_packed rule to vary one bit,
// and the duplicate is where the two copies drift.
//
// ✔MEASURED P45, `struct Z { char c; unsigned :0; char d; }`, gcc 13.3.0 and clang
// 18.1.3 agreeing PER TARGET (so: an ABI property, not a compiler quirk), plus Apple
// clang 21.0.0 on the physical macOS host for the Apple rows:
//     Ignored     5/1 : x86_64-linux · arm64-apple · x86_64-apple · riscv64 · ppc64le
//     Contributes 8/4 : aarch64-linux · arm-linux-gnueabihf
// This is gcc's `TARGET_ALIGN_ANON_BITFIELD` and clang's
// `UseZeroLengthBitfieldAlignment` — one hook, and the references implement it as
// one, which is why it is ONE key here.
//
// ★★ IT IS NAMED FOR *UNNAMED*, NOT FOR *ZERO-WIDTH*, DELIBERATELY. The same hook
// governs an unnamed bit-field WITH storage: `{char c; unsigned :3; char d;}` is 3/1
// where `{char c; unsigned x:3; char d;}` is 4/4 (✔MEASURED on x86_64-linux AND on
// the macOS host — Apple clang 21.0.0 gives 3/1). ⚠ THE ENGINE CANNOT SEE THAT CASE
// TODAY: `TypeInterner` stores a per-field WIDTH and no per-field NAME, so a
// width-3 unnamed field is indistinguishable from a width-3 named one. Zero width is
// unaffected — a NAMED zero-width bit-field is a hard error, so width 0 is unnamed BY
// CONSTRUCTION and needs no name channel. The key is therefore named for the rule it
// states rather than for the subset currently observable, so that threading field
// names through the interner later needs NO second decision and mints no second key.
// See D-CSUBSET-ZERO-WIDTH-BITFIELD-ALIGNMENT for the blocked half.
enum class UnnamedBitFieldAlignment : std::uint8_t {
    None        = 0,  // not declared → fail-loud when the rule is actually needed
    Ignored     = 1,  // SysV/Darwin/RISC-V/PowerPC: an unnamed bit-field contributes
                      // NO alignment. It still breaks the packing cursor (zero-width)
                      // and still consumes bits (non-zero width) — only the alignment
                      // contribution is dropped.
    Contributes = 2,  // AAPCS64 / AAPCS32 ELF: an unnamed bit-field contributes its
                      // declared type's NATURAL alignment. ⚠ UNCAPPED by `#pragma
                      // pack(N)` — MEASURED, `pack(1) {char c; u64 :0; char d;}` is
                      // 16/8 on aarch64-linux, the same uncapped quantity that
                      // already determines the zero-width cursor break.
};

// ★ `none` IS A ROW, for the `BitFieldStrategy` reason: a site that must refuse it
// tells "you wrote the reserved word" apart from "you wrote a spelling nobody
// claims". A `.format.json` may NOT write it — this axis is FORMAT-ONLY (no
// target-side fallback field exists, the `longDoubleFormat` shape), so `none` there
// would mean nothing an omission does not already mean.
inline constexpr EnumNameTable<UnnamedBitFieldAlignment, 3>
kUnnamedBitFieldAlignmentTable{{{
    { UnnamedBitFieldAlignment::None,        "none"        },
    { UnnamedBitFieldAlignment::Ignored,     "ignored"     },
    { UnnamedBitFieldAlignment::Contributes, "contributes" },
}}};

DSS_CHECK_ENUM_NAME_TABLE(kUnnamedBitFieldAlignmentTable);

[[nodiscard]] constexpr bool
isSelectableUnnamedBitFieldAlignment(UnnamedBitFieldAlignment a) noexcept {
    return a != UnnamedBitFieldAlignment::None;
}

// The selectable projection a diagnostic advertises. Derived, never typed — the
// `kSelectableBitFieldStrategyNames` discipline, including its static_assert that
// exactly ONE row is unselectable (a second sentinel would silently shrink the list).
inline constexpr auto kSelectableUnnamedBitFieldAlignmentNames =
    namesWhere<2>(kUnnamedBitFieldAlignmentTable, isSelectableUnnamedBitFieldAlignment);
static_assert(kUnnamedBitFieldAlignmentTable.rows.size()
                  == kSelectableUnnamedBitFieldAlignmentNames.size() + 1,
              "kUnnamedBitFieldAlignmentTable must have exactly ONE unselectable row "
              "(none); the selectable projection is derived from the predicate");

[[nodiscard]] constexpr std::string_view
unnamedBitFieldAlignmentName(UnnamedBitFieldAlignment a) noexcept {
    // The `-Werror=switch` backstop — it owns no spelling. See `dataModelName`.
    switch (a) {
        case UnnamedBitFieldAlignment::None:
        case UnnamedBitFieldAlignment::Ignored:
        case UnnamedBitFieldAlignment::Contributes:
            break;
    }
    return kUnnamedBitFieldAlignmentTable.nameOrEmpty(a);
}
[[nodiscard]] constexpr std::optional<UnnamedBitFieldAlignment>
unnamedBitFieldAlignmentFromName(std::string_view s) noexcept {
    return kUnnamedBitFieldAlignmentTable.fromName(s);
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
    // D-CSUBSET-ZERO-WIDTH-BITFIELD-ALIGNMENT: overlaid by
    // `effectiveUnnamedBitFieldAlignment` from the FORMAT document. Consulted ONLY
    // by the gnu_packed packer and only when an UNNAMED bit-field is present, so a
    // composite without one lays out byte-identically whatever this says — and
    // msvc_straddle never reads it, because the MSVC rule is a property OF that
    // strategy (a zero-width field is inert unless it terminates an open unit) and
    // is measured identical on x64 and arm64 PE.
    UnnamedBitFieldAlignment unnamedBitFieldAlignment =
        UnnamedBitFieldAlignment::None;
};

} // namespace dss
