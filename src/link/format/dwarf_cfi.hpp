#pragma once

#include "core/types/cfi.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
// For `TargetRegisterInfo::dwarfNumber` — the numbering's single owner.
// This is the realization tier, where reading the target's own tables is
// the norm (`pe.cpp`'s unwind builder already takes a `TargetSchema&`);
// it is `core/types/cfi.hpp`, the REPRESENTATION, that must stay free of
// it.
#include "core/types/target_schema.hpp"
#include "link/format/byte_emit.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════
// DWARF Call Frame Information encoder — `.eh_frame` (CIE + FDEs) and
// `.eh_frame_hdr` (the loader's binary-search index).
//
// Plan 15 DB4. Closes the ENCODER half of
// `D-UNWIND-NO-EH-FRAME-ANY-LANGUAGE-ON-ELF-OR-MACHO`: ✔MEASURED — a DSS
// elf64 build of ordinary C carries ZERO `.eh_frame` sections where gcc on
// the same source emits one, so nothing can walk a DSS stack frame.
// ═══════════════════════════════════════════════════════════════════════
//
// ── WHY THIS LIVES IN `src/link/format/` ──
//
// Unwind ENCODING is per-format; the REPRESENTATION (`core/types/cfi.hpp`)
// is shared and carries no format identity. This file is the realization
// tier for the DWARF-flavoured encoding, shared by ELF and Mach-O because
// they genuinely share it — both consume the same CIE/FDE wire format —
// while PE's Win64 `UNWIND_INFO` is a different encoding of the same
// representation and lives in `pe.cpp`. Nothing here branches on which
// format is calling; the caller supplies the section VA and the function
// addresses, both of which are facts about a laid-out image.
//
// ── ★ THE REGISTER-NUMBERING HAZARD, AND WHY THIS REFUSES ──
//
// DWARF names registers by a psABI-assigned number that is a DIFFERENT
// PERMUTATION from the hardware encoding the instruction stream uses. On
// x86_64: `%rbp` is DWARF 6 but hardware 5; `%rsp` is DWARF 7 but hardware
// 4; `%rsi`/`%rdi` are DWARF 4/5 but hardware 6/7. The return address is
// column 16 — a synthetic column that is not a register at all — while on
// AArch64 it is x30, which is also an ordinary register.
//
// An encoder that reached for `hwEncoding` would produce a table that a
// debugger reads WITHOUT COMPLAINT and uses to walk the wrong frame: a
// plausible backtrace pointing at the wrong function. That is strictly
// worse than the current state of no table at all, which at least fails
// visibly. So the numbering is REQUIRED INPUT — a per-target psABI fact
// that belongs in `*.target.json` — and a caller that cannot supply it
// gets a refusal naming the register, never a guess.

namespace dss::link::format {

// The byte-append + diagnostic helpers every format walker in this
// directory already uses. Named here so this header reads the same way
// `elf.cpp` and `pe.cpp` do (bar item 1b -- reuse the existing verbs).
using detail::appendU16LE;
using detail::appendU32LE;
using detail::emit;

// ── DWARF CFA opcodes (DWARF 5 §6.4.2) ──────────────────────────────────
inline constexpr std::uint8_t kDwCfaNop              = 0x00;
inline constexpr std::uint8_t kDwCfaAdvanceLoc1      = 0x02;
inline constexpr std::uint8_t kDwCfaAdvanceLoc2      = 0x03;
inline constexpr std::uint8_t kDwCfaAdvanceLoc4      = 0x04;
inline constexpr std::uint8_t kDwCfaOffsetExtended   = 0x05;
inline constexpr std::uint8_t kDwCfaRestoreExtended  = 0x06;
inline constexpr std::uint8_t kDwCfaUndefined        = 0x07;
inline constexpr std::uint8_t kDwCfaSameValue        = 0x08;
inline constexpr std::uint8_t kDwCfaRegister         = 0x09;
inline constexpr std::uint8_t kDwCfaRememberState    = 0x0a;
inline constexpr std::uint8_t kDwCfaRestoreState     = 0x0b;
inline constexpr std::uint8_t kDwCfaDefCfa           = 0x0c;
inline constexpr std::uint8_t kDwCfaDefCfaRegister   = 0x0d;
inline constexpr std::uint8_t kDwCfaDefCfaOffset     = 0x0e;
inline constexpr std::uint8_t kDwCfaOffsetExtendedSf = 0x11;
inline constexpr std::uint8_t kDwCfaDefCfaOffsetSf   = 0x13;
inline constexpr std::uint8_t kDwCfaValOffset        = 0x14;
// High-2-bit primary opcodes: `DW_CFA_advance_loc` (0x40 | delta),
// `DW_CFA_offset` (0x80 | reg), `DW_CFA_restore` (0xc0 | reg).
inline constexpr std::uint8_t kDwCfaAdvanceLocHi     = 0x40;
inline constexpr std::uint8_t kDwCfaOffsetHi         = 0x80;
inline constexpr std::uint8_t kDwCfaRestoreHi        = 0xc0;

// Pointer encodings for the `z`-augmentation (LSB spec).
inline constexpr std::uint8_t kDwEhPeUData4 = 0x03;
inline constexpr std::uint8_t kDwEhPePcRel4 = 0x1b;  // pcrel | sdata4
inline constexpr std::uint8_t kDwEhPeDataRel4 = 0x3b; // datarel | sdata4

// ── The per-target DWARF register numbering ─────────────────────────────
//
// Supplied by the caller from `*.target.json`. Deliberately NOT read from
// a global here: the encoder must stay target-blind, and a lookup it
// performed itself would have to know which target it was encoding for.
// ★ THE NUMBERING IS READ OFF THE REGISTER ROW ITSELF, not off a parallel
//   array built beside it. The first cut of this struct carried a values
//   span plus a bit-parallel `hasDwarfNumber` mask, which meant (a) two
//   arrays that could differ in length — a condition `declared()` had to
//   re-check AT RUNTIME — and (b) a second home for a fact the register
//   table already owns, which is the drift shape bar item 1b names.
//   Reading `registers[ord].dwarfNumber` makes both unrepresentable, and
//   it lets a refusal print the register's NAME rather than only a number.
struct DwarfRegisterMapping {
    // The target's register table, indexed by the same physical ordinal
    // `LirReg::id` and `CfiRegRef::ordinal` already carry. A row whose
    // `dwarfNumber` is EMPTY has no DWARF identity — a narrow view
    // (`eax` / `w0`), or AArch64's `xzr` (✔MEASURED 2026-08-13:
    // `aarch64-linux-gnu-as` rejects `.cfi_offset xzr` outright) — and
    // naming one in a rule is a refusal, never a fallback to the parent.
    std::span<TargetRegisterInfo const> registers;
    // The CIE's `return_address_register`. x86_64 SysV = 16 (a synthetic
    // column); AArch64 = 30 (x30/LR). No derivation exists for the
    // synthetic case, so it is declared.
    std::optional<std::uint16_t>        returnAddressColumn;
    // Name of the target, for the refusal message.
    std::string_view                    targetName;

    // Both halves of the psABI table must be present. The target loader
    // already rejects a document carrying one without the other; this
    // does not INFER the numbering from the column, because a mapping can
    // also be hand-built (tests, a future in-memory target) and "the
    // loader guarantees it" is exactly the kind of remote invariant that
    // stops holding without anyone noticing.
    [[nodiscard]] bool declared() const noexcept {
        if (!returnAddressColumn.has_value()) return false;
        for (auto const& r : registers) {
            if (r.dwarfNumber.has_value()) return true;
        }
        return false;
    }
};

// The ONE place a `TargetSchema` becomes a `DwarfRegisterMapping`. Every
// format writer calls this instead of assembling the struct itself: two
// writers hand-building the same projection is how one of them ends up
// reaching for `hwEncoding`.
[[nodiscard]] inline DwarfRegisterMapping
dwarfRegisterMappingOf(TargetSchema const& target) noexcept {
    return DwarfRegisterMapping{target.registers(),
                                target.dwarfReturnAddressColumn(),
                                target.name()};
}

// ── The FDE `initial_location` relocation, for RELOCATABLE output ───────
//
// D-UNWIND-NO-EH-FRAME-IN-RELOCATABLE-OBJECTS. In a linked image the
// `initial_location` field is a pcrel patch this file writes itself
// (`applyEhFramePcRel`) because both addresses are known. In a `.o` the
// function has no address at all, so the field ships ZERO and a real
// RELOCATION carries the reference — against the code section's symbol,
// with the function's offset within that section as the addend.
// ✔MEASURED 2026-08-13, `gcc -c` + `aarch64-linux-gnu-gcc -c` + `readelf -r`:
//   x86_64  → `R_X86_64_PC32    .text + 0 / +19 / +48`
//   aarch64 → `R_AARCH64_PREL32 .text + 0 / +24 / +56`
// both against the SECTION symbol, both with the stored field zeroed.
//
// ★ THE RELOCATION IS FOUND BY WHAT IT DOES, NOT BY WHO IS ASKING.
//   A `switch (machine)` here would be a second owner for a fact the
//   target's own relocation table already states in full: `formulaKind`,
//   `pcRelative`, `addendBias` and `widthBytes` together say "32-bit
//   PC-relative, no implicit bias" precisely. So this DERIVES the row from
//   those fields — a target that grows a third architecture inherits the
//   lookup by declaring the row, and never by editing this file.
//   Ambiguity is a REFUSAL, not a first-match: two rows answering the same
//   description means the schema no longer identifies one relocation, and
//   silently picking either writes a table that is wrong for half the
//   reasons the other row exists.
[[nodiscard]] inline TargetRelocationInfo const*
fdePointerRelocationOf(TargetSchema const& target, std::string& errorOut) {
    TargetRelocationInfo const* found = nullptr;
    for (auto const& r : target.relocations()) {
        // `tls` rows are excluded explicitly: a thread-pointer offset is not
        // an address, and a table built from one would describe frames in a
        // per-thread coordinate space no unwinder uses.
        if (r.formulaKind != RelocFormulaKind::Linear || !r.pcRelative
            || r.addendBias != 0 || r.widthBytes != 4 || r.tls) {
            continue;
        }
        if (found != nullptr) {
            errorOut = "target '" + std::string(target.name())
                     + "' declares TWO relocations matching the DWARF FDE "
                       "pointer's description (32-bit PC-relative, no implicit "
                       "addend bias): '" + found->name + "' and '" + r.name
                     + "'. One of them is wrong for this field and nothing "
                       "here can tell which, so no `.eh_frame` relocation is "
                       "emitted rather than a coin-flip an unwinder would "
                       "follow into the wrong function";
            return nullptr;
        }
        found = &r;
    }
    if (found == nullptr) {
        errorOut = "target '" + std::string(target.name())
                 + "' declares no 32-bit PC-relative relocation with a zero "
                   "addend bias, so the FDE `initial_location` in a "
                   "relocatable object cannot be relocated. This is a "
                   "per-target psABI row (x86_64: R_X86_64_PC32; aarch64: "
                   "R_AARCH64_PREL32) and it belongs in that target's "
                   "`*.target.json`. Reusing the call-site rel32 row instead "
                   "would bake its -4 instruction-end bias into a DATA field "
                   "and point every FDE 4 bytes past its function "
                   "(D-UNWIND-NO-EH-FRAME-IN-RELOCATABLE-OBJECTS)";
    }
    return found;
}

// One FDE's `initial_location` field, recorded for the post-layout patch.
// The field holds a PC-relative sdata4 to the function's address, which is
// unknowable until the section has a VA — the same shape the ELF writer's
// `.plt` already uses (sized before layout, filled after).
struct EhFramePcRelPatch {
    std::uint32_t byteOffset = 0;  // offset within `.eh_frame` of the 4-byte field
    std::size_t   funcIndex  = 0;  // index into the caller's function array
};

struct EhFrameSection {
    std::vector<std::uint8_t>      bytes;
    std::vector<EhFramePcRelPatch> patches;
    // Byte offset within `.eh_frame` of each emitted FDE's first byte,
    // parallel with `patches`. `.eh_frame_hdr`'s search table needs it.
    std::vector<std::uint32_t>     fdeOffsets;
};

namespace detail {

inline void appendULeb(std::vector<std::uint8_t>& out, std::uint64_t v) {
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(v & 0x7Fu);
        v >>= 7;
        if (v != 0) byte |= 0x80u;
        out.push_back(byte);
    } while (v != 0);
}

inline void appendSLeb(std::vector<std::uint8_t>& out, std::int64_t v) {
    bool more = true;
    while (more) {
        std::uint8_t byte = static_cast<std::uint8_t>(v & 0x7F);
        v >>= 7;                              // arithmetic shift (sign-propagating)
        bool const signBit = (byte & 0x40u) != 0;
        if ((v == 0 && !signBit) || (v == -1 && signBit)) more = false;
        else byte |= 0x80u;
        out.push_back(byte);
    }
}

// Advance the current row to `delta` bytes further along. Picks the
// narrowest form; `delta == 0` emits nothing (two rules at one PC).
inline void appendAdvance(std::vector<std::uint8_t>& out, std::uint32_t delta) {
    if (delta == 0) return;
    if (delta < 0x40u) {
        out.push_back(static_cast<std::uint8_t>(kDwCfaAdvanceLocHi | delta));
    } else if (delta <= 0xFFu) {
        out.push_back(kDwCfaAdvanceLoc1);
        out.push_back(static_cast<std::uint8_t>(delta));
    } else if (delta <= 0xFFFFu) {
        out.push_back(kDwCfaAdvanceLoc2);
        appendU16LE(out, static_cast<std::uint16_t>(delta));
    } else {
        out.push_back(kDwCfaAdvanceLoc4);
        appendU32LE(out, delta);
    }
}

inline void padToMultiple(std::vector<std::uint8_t>& out, std::size_t from,
                          std::size_t multiple) {
    while ((out.size() - from) % multiple != 0) out.push_back(kDwCfaNop);
}

} // namespace detail

// ── The CIE's data-alignment factor ─────────────────────────────────────
//
// DWARF divides every register-save offset by this before encoding it.
// ★ DSS uses -1 — the identity — DELIBERATELY. The conventional -8 is a
//   size optimisation that only holds while every offset happens to be a
//   multiple of 8, and the failure mode when one is not is a truncating
//   integer division that emits a WRONG offset rather than a diagnostic.
//   The saving is a byte or two per rule (a factored 2 and an unfactored
//   16 are both one ULEB byte below 128). Correctness that cannot be
//   accidentally lost is worth more than that.
inline constexpr std::int64_t kDataAlignmentFactor = -1;
inline constexpr std::uint64_t kCodeAlignmentFactor = 1;

// Encode one function's rule stream into CFA instructions.
//
// Returns an empty string on success, or the reason this function cannot
// be described in DWARF — always naming the rule and the register.
[[nodiscard]] inline std::string
encodeCfiInstructions(CfiFunction const& fn, DwarfRegisterMapping const& regs,
                      std::vector<std::uint8_t>& out) {
    auto dwarfOf = [&](CfiRegRef r,
                       std::uint64_t& dst) -> std::string {
        if (r.isReturnAddress) {
            dst = *regs.returnAddressColumn;
            return {};
        }
        if (r.ordinal >= regs.registers.size()
            || !regs.registers[r.ordinal].dwarfNumber.has_value()) {
            std::string const named =
                r.ordinal < regs.registers.size()
                    ? " ('" + regs.registers[r.ordinal].name + "')"
                    : " (past the end of a "
                      + std::to_string(regs.registers.size())
                      + "-register table)";
            return "target '" + std::string(regs.targetName)
                 + "' declares no DWARF register number for physical register "
                   "ordinal " + std::to_string(r.ordinal) + named
                 + "; emitting the hardware encoding instead would name a "
                   "DIFFERENT register and produce an unwind table a debugger "
                   "follows into the wrong frame";
        }
        dst = *regs.registers[r.ordinal].dwarfNumber;
        return {};
    };

    std::uint32_t curPc = 0;
    for (auto const& op : fn.ops) {
        detail::appendAdvance(out, op.pcOffset - curPc);
        curPc = op.pcOffset;
        std::uint64_t reg = 0;
        switch (op.kind) {
        case CfiOpKind::DefCfa:
            if (auto e = dwarfOf(op.reg, reg); !e.empty()) return e;
            out.push_back(kDwCfaDefCfa);
            detail::appendULeb(out, reg);
            if (op.offset < 0) {
                return "def_cfa with a negative offset ("
                     + std::to_string(op.offset)
                     + ") — the canonical frame address is above the stack "
                       "pointer by construction";
            }
            detail::appendULeb(out, static_cast<std::uint64_t>(op.offset));
            break;
        case CfiOpKind::DefCfaRegister:
            if (auto e = dwarfOf(op.reg, reg); !e.empty()) return e;
            out.push_back(kDwCfaDefCfaRegister);
            detail::appendULeb(out, reg);
            break;
        case CfiOpKind::DefCfaOffset:
            if (op.offset < 0) {
                out.push_back(kDwCfaDefCfaOffsetSf);
                detail::appendSLeb(out, op.offset / kDataAlignmentFactor);
            } else {
                out.push_back(kDwCfaDefCfaOffset);
                detail::appendULeb(out, static_cast<std::uint64_t>(op.offset));
            }
            break;
        case CfiOpKind::AdjustCfaOffset:
            // DWARF has no "adjust" opcode — gas resolves `.cfi_adjust_cfa_
            // offset` against its own running state and emits an absolute
            // `def_cfa_offset`. Doing that here would require this encoder to
            // fold the stream, which is the producer's job; a producer that
            // emits Adjust must resolve it first.
            return "adjust_cfa_offset reached the DWARF encoder unresolved — "
                   "DWARF encodes only ABSOLUTE CFA offsets, so the producer "
                   "must fold the running state before emitting";
        case CfiOpKind::RegAtCfaOffset: {
            if (auto e = dwarfOf(op.reg, reg); !e.empty()) return e;
            std::int64_t const factored = op.offset / kDataAlignmentFactor;
            if (factored >= 0 && reg < 0x40u) {
                out.push_back(static_cast<std::uint8_t>(kDwCfaOffsetHi | reg));
                detail::appendULeb(out, static_cast<std::uint64_t>(factored));
            } else if (factored >= 0) {
                out.push_back(kDwCfaOffsetExtended);
                detail::appendULeb(out, reg);
                detail::appendULeb(out, static_cast<std::uint64_t>(factored));
            } else {
                out.push_back(kDwCfaOffsetExtendedSf);
                detail::appendULeb(out, reg);
                detail::appendSLeb(out, factored);
            }
            break;
        }
        case CfiOpKind::RegValIsCfaOffset:
            if (auto e = dwarfOf(op.reg, reg); !e.empty()) return e;
            out.push_back(kDwCfaValOffset);
            detail::appendULeb(out, reg);
            detail::appendULeb(
                out, static_cast<std::uint64_t>(op.offset / kDataAlignmentFactor));
            break;
        case CfiOpKind::RegInRegister: {
            std::uint64_t src = 0;
            if (auto e = dwarfOf(op.reg, reg); !e.empty()) return e;
            if (auto e = dwarfOf(op.srcReg, src); !e.empty()) return e;
            out.push_back(kDwCfaRegister);
            detail::appendULeb(out, reg);
            detail::appendULeb(out, src);
            break;
        }
        case CfiOpKind::RegSameValue:
            if (auto e = dwarfOf(op.reg, reg); !e.empty()) return e;
            out.push_back(kDwCfaSameValue);
            detail::appendULeb(out, reg);
            break;
        case CfiOpKind::RegUndefined:
            if (auto e = dwarfOf(op.reg, reg); !e.empty()) return e;
            out.push_back(kDwCfaUndefined);
            detail::appendULeb(out, reg);
            break;
        case CfiOpKind::RegRestoreInitial:
            if (auto e = dwarfOf(op.reg, reg); !e.empty()) return e;
            if (reg < 0x40u) {
                out.push_back(static_cast<std::uint8_t>(kDwCfaRestoreHi | reg));
            } else {
                out.push_back(kDwCfaRestoreExtended);
                detail::appendULeb(out, reg);
            }
            break;
        case CfiOpKind::RememberState: out.push_back(kDwCfaRememberState); break;
        case CfiOpKind::RestoreState:  out.push_back(kDwCfaRestoreState);  break;
        }
    }
    return {};
}

// Build the whole `.eh_frame` section: one CIE followed by one FDE per
// function that has call-frame information.
//
// `codeLengths[i]` / `hasCfi` come from the caller's function array;
// `addressSize` is the target pointer width (CIE/FDE records are padded to
// it). Returns nullopt after reporting a refusal.
//
// ★ REFUSES, never degrades: a function whose rules cannot be encoded
//   fails the whole section rather than being skipped. A `.eh_frame` that
//   silently covers 90% of an image is worse than none — the unwinder
//   trusts it, finds no FDE for the missing frame, and stops the walk with
//   no indication that a table was supposed to exist.
[[nodiscard]] inline std::optional<EhFrameSection>
buildEhFrame(std::span<std::optional<CfiFunction> const> perFunc,
             DwarfRegisterMapping const& regs, std::uint32_t addressSize,
             DiagnosticReporter& reporter) {
    auto fail = [&](std::string const& msg) -> std::optional<EhFrameSection> {
        emit(reporter, DiagnosticCode::K_UnwindRuleUnrepresentable,
             "dwarf_cfi::buildEhFrame: " + msg);
        return std::nullopt;
    };
    // ★ NOTHING TO DESCRIBE COMES FIRST, AND THE ORDER IS THE WHOLE POINT.
    //   The numbering check below is a REFUSAL, and a refusal must fire only
    //   when information would actually be LOST. A module in which no
    //   function carries frame information loses nothing on a target that
    //   declares no numbering — so asking about the numbering first turns a
    //   complete, correct link into a hard failure.
    //   ✔MEASURED 2026-08-13: it did exactly that. Wiring this encoder into
    //   the ELF writer reddened `CrossCuLinkFormats.
    //   Abs64GateFiresOnlyOnTheIndirectSlotArm`, whose synthetic
    //   `x86_64-no-abs64` target declares no registers at all and whose
    //   hand-built functions carry no CFI: an unrelated test lost its image
    //   to a refusal about a table it had no use for. Same shape as the
    //   Win64 `.pdata` gate — the trigger is the INFORMATION, not the target.
    CfiInitialState const* initial = nullptr;
    for (auto const& f : perFunc) {
        if (f.has_value()) { initial = &f->initial; break; }
    }
    EhFrameSection sec;
    if (initial == nullptr) return sec;   // empty ⇒ the caller emits no section

    if (!regs.declared()) {
        return fail(
            "target '" + std::string(regs.targetName) + "' declares no DWARF "
            "register numbering, so no `.eh_frame` register rule can be "
            "written. This is a per-target psABI table (the DWARF register "
            "number of each physical register, plus the CIE's "
            "return-address column) and it belongs in that target's "
            "`*.target.json`. Emitting the hardware encoding instead would "
            "produce a table a debugger follows into the wrong frame "
            "(D-UNWIND-NO-EH-FRAME-ANY-LANGUAGE-ON-ELF-OR-MACHO)");
    }

    // ── CIE ──
    std::size_t const cieStart = sec.bytes.size();
    appendU32LE(sec.bytes, 0);                       // length — back-patched
    std::size_t const cieBody = sec.bytes.size();
    appendU32LE(sec.bytes, 0);                       // CIE_id (0 in .eh_frame)
    sec.bytes.push_back(1);                          // version
    sec.bytes.push_back('z');                        // augmentation "zR"
    sec.bytes.push_back('R');
    sec.bytes.push_back(0);
    detail::appendULeb(sec.bytes, kCodeAlignmentFactor);
    detail::appendSLeb(sec.bytes, kDataAlignmentFactor);
    detail::appendULeb(sec.bytes, *regs.returnAddressColumn);
    detail::appendULeb(sec.bytes, 1);                // augmentation data length
    sec.bytes.push_back(kDwEhPePcRel4);              // FDE pointer encoding

    // Initial instructions: the entry state every function starts in.
    // `initial` was resolved above (it is what decides whether there is
    // anything to describe at all); they are all the same by construction —
    // one calling convention per module — and the CIE is DWARF's own
    // recognition of that fact.

    // ★ The entry state must be uniform. If two functions disagree, one of
    //   them would be described by a CIE that is not theirs — a table that
    //   is confidently wrong for half the image.
    for (auto const& f : perFunc) {
        if (f.has_value() && !(f->initial == *initial)) {
            return fail("two functions in one module declare different frame "
                        "entry states; a single shared CIE cannot describe "
                        "both, and emitting one anyway would misdescribe every "
                        "function that does not match it");
        }
    }
    {
        CfiFunction entry;
        entry.codeLength = 0;
        entry.initial    = *initial;
        entry.ops.push_back(CfiOp{0, CfiOpKind::DefCfa,
                                  CfiRegRef::physical(initial->cfaRegister),
                                  CfiRegRef{}, initial->cfaOffset});
        if (initial->returnAddressAtCfaOffset.has_value()) {
            entry.ops.push_back(CfiOp{0, CfiOpKind::RegAtCfaOffset,
                                      CfiRegRef::returnAddress(), CfiRegRef{},
                                      *initial->returnAddressAtCfaOffset});
        } else if (initial->returnAddressRegister.has_value()) {
            entry.ops.push_back(
                CfiOp{0, CfiOpKind::RegInRegister, CfiRegRef::returnAddress(),
                      CfiRegRef::physical(*initial->returnAddressRegister), 0});
        }
        if (auto e = encodeCfiInstructions(entry, regs, sec.bytes); !e.empty()) {
            return fail("the shared frame entry state cannot be encoded: " + e);
        }
    }
    // Pad so the WHOLE RECORD (its 4-byte length field included) is a
    // multiple of the address size -- padding only the body leaves every
    // following record 4 bytes off the alignment the readers expect.
    detail::padToMultiple(sec.bytes, cieStart, addressSize);
    {
        std::uint32_t const len =
            static_cast<std::uint32_t>(sec.bytes.size() - cieBody);
        sec.bytes[cieStart + 0] = static_cast<std::uint8_t>(len & 0xFFu);
        sec.bytes[cieStart + 1] = static_cast<std::uint8_t>((len >> 8) & 0xFFu);
        sec.bytes[cieStart + 2] = static_cast<std::uint8_t>((len >> 16) & 0xFFu);
        sec.bytes[cieStart + 3] = static_cast<std::uint8_t>((len >> 24) & 0xFFu);
    }

    // ── FDEs ──
    for (std::size_t fi = 0; fi < perFunc.size(); ++fi) {
        if (!perFunc[fi].has_value()) continue;
        CfiFunction const& fn = *perFunc[fi];
        std::size_t const fdeStart = sec.bytes.size();
        sec.fdeOffsets.push_back(static_cast<std::uint32_t>(fdeStart));
        appendU32LE(sec.bytes, 0);                   // length — back-patched
        std::size_t const fdeBody = sec.bytes.size();
        // CIE pointer: the DISTANCE BACK to the CIE from this field (this is
        // `.eh_frame`, not `.debug_frame`, where the same field is an absolute
        // section offset — encoding one as the other yields a record every
        // reader accepts and mis-associates).
        appendU32LE(sec.bytes,
                    static_cast<std::uint32_t>(fdeBody - cieStart));
        sec.patches.push_back(EhFramePcRelPatch{
            static_cast<std::uint32_t>(sec.bytes.size()), fi});
        appendU32LE(sec.bytes, 0);                   // initial_location — patched
        appendU32LE(sec.bytes, fn.codeLength);       // address_range
        detail::appendULeb(sec.bytes, 0);            // augmentation data length
        if (auto e = encodeCfiInstructions(fn, regs, sec.bytes); !e.empty()) {
            return fail("function #" + std::to_string(fi) + ": " + e);
        }
        detail::padToMultiple(sec.bytes, fdeStart, addressSize);
        std::uint32_t const len =
            static_cast<std::uint32_t>(sec.bytes.size() - fdeBody);
        sec.bytes[fdeStart + 0] = static_cast<std::uint8_t>(len & 0xFFu);
        sec.bytes[fdeStart + 1] = static_cast<std::uint8_t>((len >> 8) & 0xFFu);
        sec.bytes[fdeStart + 2] = static_cast<std::uint8_t>((len >> 16) & 0xFFu);
        sec.bytes[fdeStart + 3] = static_cast<std::uint8_t>((len >> 24) & 0xFFu);
    }
    // Terminator: a zero-length record ends the `.eh_frame` chain.
    appendU32LE(sec.bytes, 0);
    return sec;
}

// Fill every FDE's `initial_location` once the section and the functions
// have addresses. `funcVa(i)` returns the runtime address of function `i`.
template <typename FuncVaFn>
inline void applyEhFramePcRel(EhFrameSection& sec, std::uint64_t ehFrameVa,
                              FuncVaFn const& funcVa) {
    for (auto const& p : sec.patches) {
        std::int64_t const rel =
            static_cast<std::int64_t>(funcVa(p.funcIndex))
            - static_cast<std::int64_t>(ehFrameVa + p.byteOffset);
        auto const v = static_cast<std::uint32_t>(static_cast<std::int32_t>(rel));
        sec.bytes[p.byteOffset + 0] = static_cast<std::uint8_t>(v & 0xFFu);
        sec.bytes[p.byteOffset + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
        sec.bytes[p.byteOffset + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
        sec.bytes[p.byteOffset + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
    }
}

// Build `.eh_frame_hdr` — the sorted binary-search index a runtime unwinder
// finds through `PT_GNU_EH_FRAME` so it does not have to scan every FDE.
// `funcVa(i)` must agree with the one passed to `applyEhFramePcRel`.
template <typename FuncVaFn>
[[nodiscard]] inline std::vector<std::uint8_t>
buildEhFrameHdr(EhFrameSection const& sec, std::uint64_t hdrVa,
                std::uint64_t ehFrameVa, FuncVaFn const& funcVa) {
    std::vector<std::uint8_t> out;
    out.push_back(1);                 // version
    out.push_back(kDwEhPePcRel4);     // eh_frame_ptr encoding
    out.push_back(kDwEhPeUData4);     // fde_count encoding
    out.push_back(kDwEhPeDataRel4);   // table encoding (relative to THIS header)
    std::int64_t const ehPtrRel =
        static_cast<std::int64_t>(ehFrameVa)
        - static_cast<std::int64_t>(hdrVa + out.size());
    appendU32LE(out, static_cast<std::uint32_t>(
                         static_cast<std::int32_t>(ehPtrRel)));
    appendU32LE(out, static_cast<std::uint32_t>(sec.patches.size()));
    // The table MUST be sorted by initial_location: the unwinder binary-
    // searches it, and an unsorted table silently returns the wrong FDE for
    // some addresses rather than failing.
    struct Row { std::int32_t pc; std::int32_t fde; };
    std::vector<Row> rows;
    rows.reserve(sec.patches.size());
    for (std::size_t k = 0; k < sec.patches.size(); ++k) {
        rows.push_back(Row{
            static_cast<std::int32_t>(
                static_cast<std::int64_t>(funcVa(sec.patches[k].funcIndex))
                - static_cast<std::int64_t>(hdrVa)),
            static_cast<std::int32_t>(
                static_cast<std::int64_t>(ehFrameVa + sec.fdeOffsets[k])
                - static_cast<std::int64_t>(hdrVa))});
    }
    std::sort(rows.begin(), rows.end(),
              [](Row const& a, Row const& b) { return a.pc < b.pc; });
    for (auto const& r : rows) {
        appendU32LE(out, static_cast<std::uint32_t>(r.pc));
        appendU32LE(out, static_cast<std::uint32_t>(r.fde));
    }
    return out;
}

} // namespace dss::link::format
