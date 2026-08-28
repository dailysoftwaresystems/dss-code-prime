#pragma once

#include "core/types/cfi.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "link/format/dwarf_cfi.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════
// DWARF Call Frame Information DECODER — `.eh_frame` (CIE + FDEs) back
// into the neutral, PC-keyed `CfiFunction` vocabulary.
//
// D-LK-MERGED-FOREIGN-FUNCTIONS-CARRY-NO-UNWIND-INFO-IN-THE-IMAGE.
// ═══════════════════════════════════════════════════════════════════════
//
// ── WHY A DECODER IS THE FORMAT-WIDE FIX, AND A PER-FORMAT CARRY IS NOT ──
//
// A function merged from a foreign object used to arrive in a DSS image
// with NO unwind description at all: a backtrace stops at it, a profiler
// stack ends there, and an exception thrown through it terminates. The
// information WAS present in the object and was dropped.
//
// The obvious repair — copy the foreign `.eh_frame` bytes into the image
// and re-relocate each FDE's `initial_location` — is a per-format carry,
// and it buys nothing anywhere else: Mach-O's own linker does not carry
// its input unwind section either — ✔MEASURED under
//   D-LK-MACHO-COMPACT-UNWIND-SECTION-REFUSED-BLOCKS-EVERY-STOCK-MACOS-ARCHIVE
// that ld64 CONSUMES
// `__LD,__compact_unwind` and SYNTHESIZES `__TEXT,__unwind_info`. And PE
// states the same facts in a completely different encoding.
//
// This project already has the shape that answers all of them at once.
// `CfiFunction` is the neutral, PC-keyed unwind representation, EVERY
// format writer already encodes its table from it (`.eh_frame` here,
// `.pdata`/`.xdata` in `pe.cpp`), and `AssembledFunction::cfi` is the slot
// a reconstructed function already carries. So the gap is exactly ONE
// missing tier: nothing INVERTS a foreign encoding back into the neutral
// vocabulary. Fill that in, and every format's existing writer emits the
// merged function's table with no new emit path anywhere — which is why
// this is a READ-side header and why it is shared rather than per-format.
//
// ── THE ROUND TRIP IS THE POINT, AND IT IS EXACT ──
//
// This file is the INVERSE of `encodeCfiInstructions` in `dwarf_cfi.hpp`,
// opcode for opcode, and it deliberately reuses that header's opcode
// constants and its `DwarfRegisterMapping` rather than restating either.
// Two tables of DWARF opcodes in one tree is precisely the drift shape
// this project's bar names: one of them gets a new row and the other does
// not, and nothing fails.
//
// ── ★ WHAT IT REFUSES, AND WHY EACH REFUSAL IS LOAD-BEARING ──
//
// A decoder that "does its best" here is the original defect wearing a
// hat: an unwind table that is confidently wrong is worse than one that is
// absent, because the unwinder trusts it. Every construct below is
// therefore refused BY NAME rather than approximated:
//
//   * a `P` (personality) or `L` (LSDA) augmentation, and any non-empty
//     FDE augmentation data. `CfiFunction` carries neither pointer, so
//     accepting them would silently zero a language-specific handler and
//     turn a catch into a terminate. The row that anchors this file states
//     the constraint outright: personality / LSDA must FAIL LOUD until
//     they are carried, never be zeroed.
//   * an `S` (signal-frame) augmentation. `CfiFunction::signalFrame`
//     exists but `buildEhFrame` does not emit it (it is a CIE-level
//     property and DSS emits ONE shared CIE per module — the same
//     structural reason `.cfi_signal_frame` is refused by the assembly
//     producer), so accepting it would drop it.
//   * the DWARF EXPRESSION family (`DW_CFA_def_cfa_expression`,
//     `DW_CFA_expression`, `DW_CFA_val_expression`). These carry a DWARF
//     bytecode program, which the `CfiOpKind` vocabulary has no
//     enumerator for by construction.
//   * `DW_CFA_GNU_args_size` and every other vendor opcode. Unknown is
//     refused rather than skipped: the length of a vendor opcode's
//     operands is not derivable, so "skip it" cannot even find the next
//     opcode, and a decoder that guessed would resynchronize onto operand
//     bytes and emit a plausible-looking table of noise.
//   * a CIE whose initial instructions say more than `CfiInitialState` can
//     hold. The entry state is a fixed shape (CFA base + offset + where
//     the return address is); a CIE that also spills a callee-saved
//     register at entry is describing something this representation
//     cannot carry, and truncating it silently would misdescribe every
//     function sharing that CIE.
//
// ── ★ THE RETURN-ADDRESS COLUMN IS RESOLVED, NOT GUESSED ──
//
// DWARF names the return address by the CIE's `return_address_register`
// column. On x86_64 that is 16, a synthetic column no physical register
// maps to. On AArch64 it is 30 — which IS x30's ordinary DWARF number, so
// the same number means both things depending on where it appears.
// A decoded rule whose DWARF number equals the CIE's column becomes
// `CfiRegRef::returnAddress()`, which is what the column MEANS and what
// re-encodes back to the same number. Mapping it to the physical ordinal
// instead would round-trip through the encoder to the identical byte on
// AArch64 and to a DIFFERENT byte on x86_64 — a divergence that shows up
// only on one port, which is the failure mode this project has already
// paid for once (`D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED`'s M4 mutant).
//
// ── THE POINTER ENCODING IS READ FROM THE AUGMENTATION, NEVER ASSUMED ──
//
// An FDE's `initial_location` is encoded per the CIE's `R` augmentation
// byte. This decoder does not resolve it to an address — in a relocatable
// object there are no addresses, and the field is zero with a RELOCATION
// carrying the reference (✔MEASURED, `gcc -c` x86_64: `R_X86_64_PC32
// .text + 0 / +19 / +60`; aarch64: `R_AARCH64_PREL32 .text + 0 / +24 /
// +72`). So it reports the field's BYTE OFFSET within the section and the
// raw stored delta, and the CALLER — which is the only tier that knows
// whether this object states the reference as a relocation or as a
// section-relative pcrel value — binds it to a function. A decoder that
// picked one convention would be right for exactly one format.

namespace dss::link::format {

// Opcodes the ENCODER never emits and so does not declare, but which a
// foreign producer legitimately does. Declared here beside their decode
// arms rather than added to the encoder's list: a constant with no
// consumer in its own file is a constant nothing keeps true.
inline constexpr std::uint8_t kDwCfaDefCfaSf      = 0x12;
inline constexpr std::uint8_t kDwCfaValOffsetSf   = 0x15;
// The expression family — refused by name (see the header note).
inline constexpr std::uint8_t kDwCfaDefCfaExpression = 0x0f;
inline constexpr std::uint8_t kDwCfaExpression       = 0x10;
inline constexpr std::uint8_t kDwCfaValExpression    = 0x16;
// Vendor range. Everything at or above this is producer-private and its
// operand shape is not derivable, so it is refused rather than skipped.
inline constexpr std::uint8_t kDwCfaLoUser           = 0x1c;

// Pointer encodings this decoder can resolve. Both halves matter: the
// FORMAT half (sdata4 vs absptr) says how many bytes the field is, and the
// APPLICATION half (pcrel vs absolute) says what they mean.
inline constexpr std::uint8_t kDwEhPeAbsPtr = 0x00;
// ★ APPLE'S THIRD SPELLING, AND A REFUTATION OF THIS FILE'S OWN "the two
//   encodings every measured producer emits". ✔MEASURED 2026-08-25 on real
//   Apple Silicon (macOS 26.5.2), `/usr/bin/cc -arch x86_64 -c` with no other
//   flag: the CIE's `R` augmentation byte is **0x10** — pcrel like gcc's, but
//   with the FORMAT half left at absptr, so the field is EIGHT bytes and not
//   four. It was refused outright, which is the fail-loud arm working: this
//   decoder read no field of the wrong width, it declined to read at all. The
//   fix is one row here, because the two halves were already separate concerns
//   — the 8-byte read arm below is the `absptr` arm unchanged, and `pcrel` is
//   the CALLER'S half (this file never resolves an address; see the header's
//   note on why the pointer's APPLICATION cannot be decided here).
inline constexpr std::uint8_t kDwEhPePcRel8 = 0x10;  // pcrel | absptr (8-byte)
inline constexpr std::uint8_t kDwEhPeOmit   = 0xff;

// Is `enc` a pointer encoding whose field is EIGHT bytes wide? The one place
// the format half is interpreted — the gate and the reader must agree about
// the width or the gate admits an encoding the reader mis-reads.
[[nodiscard]] constexpr bool dwEhPeIsEightByteField(std::uint8_t enc) noexcept {
    return enc == kDwEhPeAbsPtr || enc == kDwEhPePcRel8;
}

// Is the stored value a DELTA FROM ITS OWN FIELD'S ADDRESS (pcrel), rather
// than an address? The APPLICATION half of the encoding — the question the
// CALLER answers, because only the caller knows the field's address. Stated
// here beside the encodings themselves so a caller cannot reach for a bit
// test of its own and get the mask subtly wrong.
[[nodiscard]] constexpr bool dwEhPeIsPcRelative(std::uint8_t enc) noexcept {
    return enc == kDwEhPePcRel4 || enc == kDwEhPePcRel8;
}

// One decoded FDE: the function's unwind description plus everything the
// caller needs to bind it to a reconstructed function.
struct DecodedFde {
    // Byte offset WITHIN the decoded section of the FDE's
    // `initial_location` field. In a relocatable object this is the
    // `r_offset` of the relocation that carries the reference.
    std::uint32_t initialLocationFieldOffset = 0;
    // The value STORED in that field, sign-extended. Zero in a relocatable
    // object whose reference is a relocation; a pcrel delta in an object
    // that resolves the reference within its own flat address space.
    std::int64_t  storedInitialLocation = 0;
    // The CIE's `R` pointer encoding for that field, so a caller that
    // resolves the stored value knows what it is resolving.
    std::uint8_t  pointerEncoding = kDwEhPeOmit;
    // The function's unwind description. `codeLength` is the FDE's
    // `address_range`.
    CfiFunction   cfi;
};

struct DecodedEhFrame {
    std::vector<DecodedFde> fdes;
};

namespace detail {

// ── LEB128 readers, bounds-checked ──────────────────────────────────────
//
// Return false on a truncated or over-long encoding. An over-long ULEB
// (more than 10 continuation bytes for a 64-bit value) is a corrupt
// stream, not a large number: accepting it would let a crafted object
// shift every following opcode.
[[nodiscard]] inline bool readULeb(std::span<std::uint8_t const> b,
                                   std::size_t& pos, std::uint64_t& out) {
    out = 0;
    unsigned shift = 0;
    for (unsigned i = 0; i < 10u; ++i) {
        if (pos >= b.size()) return false;
        std::uint8_t const byte = b[pos++];
        out |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) return true;
        shift += 7;
    }
    return false;
}

[[nodiscard]] inline bool readSLeb(std::span<std::uint8_t const> b,
                                   std::size_t& pos, std::int64_t& out) {
    std::uint64_t result = 0;
    unsigned shift = 0;
    for (unsigned i = 0; i < 10u; ++i) {
        if (pos >= b.size()) return false;
        std::uint8_t const byte = b[pos++];
        result |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;
        shift += 7;
        if ((byte & 0x80u) == 0) {
            if (shift < 64u && (byte & 0x40u) != 0) {
                result |= ~std::uint64_t{0} << shift;   // sign-extend
            }
            out = static_cast<std::int64_t>(result);
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool readU32LE(std::span<std::uint8_t const> b,
                                    std::size_t& pos, std::uint32_t& out) {
    if (pos + 4u > b.size()) return false;
    out = static_cast<std::uint32_t>(b[pos])
        | (static_cast<std::uint32_t>(b[pos + 1]) << 8)
        | (static_cast<std::uint32_t>(b[pos + 2]) << 16)
        | (static_cast<std::uint32_t>(b[pos + 3]) << 24);
    pos += 4u;
    return true;
}

[[nodiscard]] inline bool readU64LE(std::span<std::uint8_t const> b,
                                    std::size_t& pos, std::uint64_t& out) {
    if (pos + 8u > b.size()) return false;
    out = 0;
    for (unsigned i = 0; i < 8u; ++i) {
        out |= static_cast<std::uint64_t>(b[pos + i]) << (8u * i);
    }
    pos += 8u;
    return true;
}

// The CIE fields an FDE needs in order to be decoded.
struct DecodedCie {
    std::uint64_t codeAlignmentFactor = 1;
    std::int64_t  dataAlignmentFactor = 1;
    std::uint64_t returnAddressColumn = 0;
    std::uint8_t  fdePointerEncoding  = kDwEhPeOmit;
    bool          hasZAugmentation    = false;
    CfiInitialState initial{};
};

} // namespace detail

// The DWARF-number → physical-ordinal reverse map.
//
// ★★ THE FORWARD MAP IS NOT INJECTIVE, AND THE FIRST CUT OF THIS CLASS
//    REFUSED ON THAT — WRONGLY. It rejected any DWARF number claimed by two
//    register rows, on the reasoning that "the document no longer identifies
//    a register". ✔MEASURED against the shipped documents and the reasoning
//    is false on one of the two targets that ship: `arm64.target.json`
//    declares DWARF 64..95 TWICE EACH — once for `dN` (class `fpr`,
//    widthBytes 8) and once for `vN` (class `vr`, widthBytes 16) — and that
//    is CORRECT psABI, because AArch64 numbers the REGISTER FILE while a DSS
//    ordinal names a WIDTH VIEW of a register. `x86_64.target.json` has no
//    such pair (33 numbers, 33 rows), so the refusal read green on the
//    target it was tried against and reddened a real `aarch64-linux-gnu-gcc`
//    object — the same one-port-masks-the-other shape this project already
//    paid for in `D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED`.
//
// ★ SO THE REFUSAL IS NARROWED TO THE CONDITION THAT IS ACTUALLY WRONG,
//   AND IT IS A MEASURED PREDICATE RATHER THAN A COUNT: two rows sharing a
//   DWARF number must also share an `hwEncoding` — that is what makes them
//   two views of ONE architectural register (✔MEASURED: `d8` and `v8` both
//   carry hwEncoding 8). Two rows sharing a number with DIFFERENT hardware
//   encodings really are two different registers wearing one number, and
//   THAT is unresolvable and still refuses.
//
// ★ WHICH VIEW WINS IS THE NARROWEST, and it is a statement about what a
//   save rule MEANS rather than a tiebreak: a CFI rule describes the bytes
//   the callee-save contract covers, and on AArch64 that is the low 64 bits
//   of v8–v15 — which is exactly `d8`–`d15`, and exactly what GNU `as`
//   translates `.cfi_offset d8` into. The choice is also inert to every
//   unwind encoder in this tree by construction: all candidates carry the
//   same `dwarfNumber`, so the forward map collapses them back to the byte
//   the object came in with, whichever one is chosen.
class DwarfRegisterReverseMap {
public:
    [[nodiscard]] static std::optional<DwarfRegisterReverseMap>
    build(DwarfRegisterMapping const& regs, std::string& errorOut) {
        DwarfRegisterReverseMap m;
        m.regs_ = &regs;
        for (std::size_t ord = 0; ord < regs.registers.size(); ++ord) {
            auto const& r = regs.registers[ord];
            if (!r.dwarfNumber.has_value()) continue;
            std::uint64_t const n = *r.dwarfNumber;
            bool replaced = false;
            for (auto& [have, haveOrd] : m.rows_) {
                if (have != n) continue;
                auto const& other = regs.registers[haveOrd];
                if (other.hwEncoding != r.hwEncoding) {
                    errorOut =
                        "target '" + std::string(regs.targetName)
                        + "' declares DWARF register number " + std::to_string(n)
                        + " for BOTH '" + other.name + "' (hardware encoding "
                        + std::to_string(other.hwEncoding) + ") and '" + r.name
                        + "' (hardware encoding " + std::to_string(r.hwEncoding)
                        + "). Two rows may share a DWARF number only when they "
                          "are width VIEWS of one architectural register, which "
                          "a shared hardware encoding is what states; these name "
                          "two different registers, so a decoded unwind rule "
                          "carrying that number identifies neither and picking "
                          "either would write a table a debugger follows into "
                          "the wrong frame";
                    return std::nullopt;
                }
                if (r.widthBytes < other.widthBytes) haveOrd = ord;  // narrowest wins
                replaced = true;
                break;
            }
            if (!replaced) m.rows_.push_back({n, ord});
        }
        return m;
    }

    // Resolve a DWARF register number to a `CfiRegRef`. The CIE's
    // return-address column wins over any physical row carrying the same
    // number — see the header note on AArch64's x30.
    [[nodiscard]] std::optional<CfiRegRef> resolve(std::uint64_t dwarfNum) const {
        if (regs_->returnAddressColumn.has_value()
            && dwarfNum == *regs_->returnAddressColumn) {
            return CfiRegRef::returnAddress();
        }
        for (auto const& [have, ord] : rows_) {
            if (have == dwarfNum) {
                return CfiRegRef::physical(static_cast<std::uint16_t>(ord));
            }
        }
        return std::nullopt;
    }

    // The physical ordinal for a DWARF number, IGNORING the return-address
    // column. Used only for the link-register case, where the CIE's column
    // names a real register whose ordinal the entry state must carry.
    [[nodiscard]] std::optional<std::uint16_t>
    physicalOnly(std::uint64_t dwarfNum) const {
        for (auto const& [have, ord] : rows_) {
            if (have == dwarfNum) return static_cast<std::uint16_t>(ord);
        }
        return std::nullopt;
    }

    [[nodiscard]] std::string describe(std::uint64_t dwarfNum) const {
        for (auto const& [have, ord] : rows_) {
            if (have == dwarfNum) return regs_->registers[ord].name;
        }
        return "<no register in target '" + std::string(regs_->targetName)
             + "' declares DWARF number " + std::to_string(dwarfNum) + ">";
    }

    // Name an ALREADY-RESOLVED reference. Distinct from `describe`, which
    // takes a DWARF number: a refusal about a rule the decoder has already
    // resolved holds a `CfiRegRef`, and passing its ordinal to `describe`
    // would look up the wrong register entirely (ordinals and DWARF numbers
    // are different permutations of the same file — the hazard
    // `dwarf_cfi.hpp` names in the forward direction).
    [[nodiscard]] std::string describeRef(CfiRegRef r) const {
        if (r.isReturnAddress) return "the return-address column";
        if (r.ordinal < regs_->registers.size()) {
            return "'" + regs_->registers[r.ordinal].name + "'";
        }
        return "physical ordinal " + std::to_string(r.ordinal);
    }

private:
    DwarfRegisterMapping const*                              regs_ = nullptr;
    std::vector<std::pair<std::uint64_t, std::size_t>>       rows_;
};

namespace detail {

// Decode one run of CFA instructions into `ops`, advancing `pc`.
// `initialPass` selects the CIE's stricter rules (see the header note on
// what `CfiInitialState` can hold): in the CIE a rule is folded into the
// entry state, in an FDE it becomes an op.
//
// Returns an empty string on success, otherwise the reason — always naming
// the opcode, so a refusal is actionable rather than a byte value.
[[nodiscard]] inline std::string
decodeCfaInstructions(std::span<std::uint8_t const> body, DecodedCie const& cie,
                      DwarfRegisterReverseMap const& rev, bool initialPass,
                      CfiInitialState& initial, std::vector<CfiOp>& ops) {
    std::size_t pos = 0;
    std::uint32_t pc = 0;
    bool sawDefCfa = false;

    auto regOf = [&](std::uint64_t n,
                     std::optional<CfiRegRef>& dst) -> std::string {
        dst = rev.resolve(n);
        if (!dst.has_value()) {
            return "the rule names DWARF register number " + std::to_string(n)
                 + ", which " + rev.describe(n)
                 + ". Guessing a physical register here would produce an "
                   "unwind table a debugger follows into the wrong frame";
        }
        return {};
    };
    auto push = [&](CfiOpKind kind, CfiRegRef reg, CfiRegRef src,
                    std::int64_t off) -> std::string {
        if (!initialPass) {
            ops.push_back(CfiOp{pc, kind, reg, src, off});
            return {};
        }
        // ── CIE initial instructions: fold into `CfiInitialState` ──
        if (pc != 0) {
            return "a CIE's initial instructions advance the program counter, "
                   "which makes the entry state PC-dependent; `CfiInitialState` "
                   "describes one entry row and cannot carry that";
        }
        switch (kind) {
        case CfiOpKind::DefCfa:
            if (reg.isReturnAddress) {
                return "the CIE names the return-address column as the CFA base "
                       "register";
            }
            initial.cfaRegister = reg.ordinal;
            initial.cfaOffset   = off;
            sawDefCfa = true;
            return {};
        case CfiOpKind::DefCfaRegister:
            if (reg.isReturnAddress) {
                return "the CIE names the return-address column as the CFA base "
                       "register";
            }
            initial.cfaRegister = reg.ordinal;
            sawDefCfa = true;
            return {};
        case CfiOpKind::DefCfaOffset:
            initial.cfaOffset = off;
            return {};
        case CfiOpKind::RegAtCfaOffset:
            if (!reg.isReturnAddress) {
                return "the CIE states an entry save rule ('offset') for "
                       "register " + rev.describeRef(reg)
                     + "; `CfiInitialState` carries the CFA rule and the return "
                       "address only, and silently dropping an entry-state "
                       "register rule would misdescribe every function sharing "
                       "this CIE";
            }
            initial.returnAddressAtCfaOffset = off;
            initial.returnAddressRegister.reset();
            return {};
        case CfiOpKind::RegInRegister:
        case CfiOpKind::RegSameValue:
            if (!reg.isReturnAddress) {
                return "the CIE states an entry rule ('"
                     + std::string(cfiOpKindName(kind))
                     + "') for a general register; `CfiInitialState` carries the "
                       "CFA rule and the return address only";
            }
            // The return address is still wherever it was at entry — a
            // link-register ABI. `srcReg` names the holding register for
            // `register`; for `same_value` the column IS the register.
            {
                CfiRegRef const holder =
                    kind == CfiOpKind::RegInRegister ? src : reg;
                if (holder.isReturnAddress) {
                    auto const ord =
                        rev.physicalOnly(cie.returnAddressColumn);
                    if (!ord.has_value()) {
                        return "the CIE says the return address is unchanged at "
                               "entry, but its return-address column ("
                             + std::to_string(cie.returnAddressColumn)
                             + ") names no physical register in this target, so "
                               "there is no register to say it lives in";
                    }
                    initial.returnAddressRegister = *ord;
                } else {
                    initial.returnAddressRegister = holder.ordinal;
                }
                initial.returnAddressAtCfaOffset.reset();
            }
            return {};
        default:
            return "the CIE states an entry rule ('"
                 + std::string(cfiOpKindName(kind))
                 + "') that `CfiInitialState` cannot carry";
        }
    };

    while (pos < body.size()) {
        std::uint8_t const op = body[pos++];
        std::uint8_t const hi = static_cast<std::uint8_t>(op & 0xC0u);
        std::uint8_t const lo = static_cast<std::uint8_t>(op & 0x3Fu);

        if (hi == kDwCfaAdvanceLocHi) {
            pc += static_cast<std::uint32_t>(lo * cie.codeAlignmentFactor);
            continue;
        }
        if (hi == kDwCfaOffsetHi) {                    // DW_CFA_offset
            std::uint64_t factored = 0;
            if (!readULeb(body, pos, factored)) return "truncated DW_CFA_offset";
            std::optional<CfiRegRef> r;
            if (auto e = regOf(lo, r); !e.empty()) return "DW_CFA_offset: " + e;
            if (auto e = push(CfiOpKind::RegAtCfaOffset, *r, CfiRegRef{},
                              static_cast<std::int64_t>(factored)
                                  * cie.dataAlignmentFactor);
                !e.empty()) {
                return "DW_CFA_offset: " + e;
            }
            continue;
        }
        if (hi == kDwCfaRestoreHi) {                   // DW_CFA_restore
            std::optional<CfiRegRef> r;
            if (auto e = regOf(lo, r); !e.empty()) return "DW_CFA_restore: " + e;
            if (auto e = push(CfiOpKind::RegRestoreInitial, *r, CfiRegRef{}, 0);
                !e.empty()) {
                return "DW_CFA_restore: " + e;
            }
            continue;
        }

        switch (op) {
        case kDwCfaNop:
            break;
        case kDwCfaAdvanceLoc1: {
            if (pos >= body.size()) return "truncated DW_CFA_advance_loc1";
            pc += static_cast<std::uint32_t>(body[pos++] * cie.codeAlignmentFactor);
            break;
        }
        case kDwCfaAdvanceLoc2: {
            if (pos + 2u > body.size()) return "truncated DW_CFA_advance_loc2";
            std::uint32_t const d = static_cast<std::uint32_t>(body[pos])
                                  | (static_cast<std::uint32_t>(body[pos + 1]) << 8);
            pos += 2u;
            pc += static_cast<std::uint32_t>(d * cie.codeAlignmentFactor);
            break;
        }
        case kDwCfaAdvanceLoc4: {
            std::uint32_t d = 0;
            if (!readU32LE(body, pos, d)) return "truncated DW_CFA_advance_loc4";
            pc += static_cast<std::uint32_t>(d * cie.codeAlignmentFactor);
            break;
        }
        case kDwCfaOffsetExtended:
        case kDwCfaOffsetExtendedSf:
        case kDwCfaValOffset:
        case kDwCfaValOffsetSf: {
            std::uint64_t reg = 0;
            if (!readULeb(body, pos, reg)) return "truncated register operand";
            std::int64_t factored = 0;
            bool const signedForm =
                (op == kDwCfaOffsetExtendedSf || op == kDwCfaValOffsetSf);
            if (signedForm) {
                if (!readSLeb(body, pos, factored)) return "truncated offset";
            } else {
                std::uint64_t u = 0;
                if (!readULeb(body, pos, u)) return "truncated offset";
                factored = static_cast<std::int64_t>(u);
            }
            std::optional<CfiRegRef> r;
            if (auto e = regOf(reg, r); !e.empty()) return e;
            CfiOpKind const kind =
                (op == kDwCfaValOffset || op == kDwCfaValOffsetSf)
                    ? CfiOpKind::RegValIsCfaOffset
                    : CfiOpKind::RegAtCfaOffset;
            if (auto e = push(kind, *r, CfiRegRef{},
                              factored * cie.dataAlignmentFactor);
                !e.empty()) {
                return e;
            }
            break;
        }
        case kDwCfaRestoreExtended: {
            std::uint64_t reg = 0;
            if (!readULeb(body, pos, reg))
                return "truncated DW_CFA_restore_extended";
            std::optional<CfiRegRef> r;
            if (auto e = regOf(reg, r); !e.empty())
                return "DW_CFA_restore_extended: " + e;
            if (auto e = push(CfiOpKind::RegRestoreInitial, *r, CfiRegRef{}, 0);
                !e.empty()) {
                return e;
            }
            break;
        }
        case kDwCfaUndefined:
        case kDwCfaSameValue: {
            std::uint64_t reg = 0;
            if (!readULeb(body, pos, reg)) return "truncated register operand";
            std::optional<CfiRegRef> r;
            if (auto e = regOf(reg, r); !e.empty()) return e;
            CfiOpKind const kind = op == kDwCfaUndefined
                                       ? CfiOpKind::RegUndefined
                                       : CfiOpKind::RegSameValue;
            if (auto e = push(kind, *r, *r, 0); !e.empty()) return e;
            break;
        }
        case kDwCfaRegister: {
            std::uint64_t reg = 0, src = 0;
            if (!readULeb(body, pos, reg) || !readULeb(body, pos, src))
                return "truncated DW_CFA_register";
            std::optional<CfiRegRef> r, s;
            if (auto e = regOf(reg, r); !e.empty()) return "DW_CFA_register: " + e;
            if (auto e = regOf(src, s); !e.empty()) return "DW_CFA_register: " + e;
            if (auto e = push(CfiOpKind::RegInRegister, *r, *s, 0); !e.empty())
                return e;
            break;
        }
        case kDwCfaRememberState:
        case kDwCfaRestoreState: {
            if (initialPass) {
                return "a CIE's initial instructions use the state stack "
                       "('remember_state'/'restore_state'), which describes a "
                       "sequence rather than the single entry row "
                       "`CfiInitialState` carries";
            }
            ops.push_back(CfiOp{pc,
                                op == kDwCfaRememberState
                                    ? CfiOpKind::RememberState
                                    : CfiOpKind::RestoreState,
                                CfiRegRef{}, CfiRegRef{}, 0});
            break;
        }
        case kDwCfaDefCfa:
        case kDwCfaDefCfaSf: {
            std::uint64_t reg = 0;
            if (!readULeb(body, pos, reg)) return "truncated DW_CFA_def_cfa";
            std::int64_t off = 0;
            if (op == kDwCfaDefCfaSf) {
                if (!readSLeb(body, pos, off)) return "truncated DW_CFA_def_cfa_sf";
                off *= cie.dataAlignmentFactor;
            } else {
                std::uint64_t u = 0;
                if (!readULeb(body, pos, u)) return "truncated DW_CFA_def_cfa";
                off = static_cast<std::int64_t>(u);
            }
            std::optional<CfiRegRef> r;
            if (auto e = regOf(reg, r); !e.empty()) return "DW_CFA_def_cfa: " + e;
            if (auto e = push(CfiOpKind::DefCfa, *r, CfiRegRef{}, off); !e.empty())
                return e;
            break;
        }
        case kDwCfaDefCfaRegister: {
            std::uint64_t reg = 0;
            if (!readULeb(body, pos, reg))
                return "truncated DW_CFA_def_cfa_register";
            std::optional<CfiRegRef> r;
            if (auto e = regOf(reg, r); !e.empty())
                return "DW_CFA_def_cfa_register: " + e;
            if (auto e = push(CfiOpKind::DefCfaRegister, *r, CfiRegRef{}, 0);
                !e.empty()) {
                return e;
            }
            break;
        }
        case kDwCfaDefCfaOffset:
        case kDwCfaDefCfaOffsetSf: {
            std::int64_t off = 0;
            if (op == kDwCfaDefCfaOffsetSf) {
                if (!readSLeb(body, pos, off))
                    return "truncated DW_CFA_def_cfa_offset_sf";
                off *= cie.dataAlignmentFactor;
            } else {
                std::uint64_t u = 0;
                if (!readULeb(body, pos, u))
                    return "truncated DW_CFA_def_cfa_offset";
                off = static_cast<std::int64_t>(u);
            }
            if (auto e = push(CfiOpKind::DefCfaOffset, CfiRegRef{}, CfiRegRef{},
                              off);
                !e.empty()) {
                return e;
            }
            break;
        }
        case kDwCfaDefCfaExpression:
        case kDwCfaExpression:
        case kDwCfaValExpression:
            return "the frame rule is a DWARF EXPRESSION (opcode 0x"
                 + std::string(1, "0123456789abcdef"[(op >> 4) & 0xFu])
                 + std::string(1, "0123456789abcdef"[op & 0xFu])
                 + "), a bytecode program the neutral `CfiFunction` vocabulary "
                   "has no enumerator for. Carrying it would need a DWARF "
                   "expression evaluator in every format's unwind writer, and "
                   "dropping it would leave the register it describes "
                   "unrecoverable with no indication a rule was lost";
        default:
            if (op >= kDwCfaLoUser) {
                return "the frame rule is vendor opcode 0x"
                     + std::string(1, "0123456789abcdef"[(op >> 4) & 0xFu])
                     + std::string(1, "0123456789abcdef"[op & 0xFu])
                     + " (DW_CFA_lo_user and above). Its operand shape is "
                       "producer-private, so this cannot even find the next "
                       "opcode past it — skipping would resynchronize onto "
                       "operand bytes and decode a table of noise";
            }
            return "unknown DWARF CFA opcode 0x"
                 + std::string(1, "0123456789abcdef"[(op >> 4) & 0xFu])
                 + std::string(1, "0123456789abcdef"[op & 0xFu]);
        }
    }
    if (initialPass && !sawDefCfa) {
        return "the CIE states no CFA rule at all, so nothing describes where "
               "the caller's stack pointer was on entry";
    }
    return {};
}

} // namespace detail

// Decode a whole `.eh_frame` section body into one `DecodedFde` per FDE.
//
// Returns nullopt after reporting exactly one refusal naming the specific
// construct. `contextLabel` prefixes the diagnostic with the reader that
// asked ("elf::readRelocatableObject"), so the user sees which tier
// refused and on which object.
[[nodiscard]] inline std::optional<DecodedEhFrame>
decodeEhFrame(std::span<std::uint8_t const> section,
              DwarfRegisterMapping const& regs, std::string_view contextLabel,
              DiagnosticReporter& reporter) {
    auto fail = [&](std::string const& msg) -> std::optional<DecodedEhFrame> {
        emit(reporter, DiagnosticCode::K_UnwindRuleUnrepresentable,
             std::string(contextLabel) + ": " + msg
                 + " (D-LK-MERGED-FOREIGN-FUNCTIONS-CARRY-NO-UNWIND-INFO-IN-THE-IMAGE)");
        return std::nullopt;
    };

    std::string revErr;
    auto revOpt = DwarfRegisterReverseMap::build(regs, revErr);
    if (!revOpt.has_value()) return fail(revErr);
    DwarfRegisterReverseMap const& rev = *revOpt;

    DecodedEhFrame out;
    // CIEs are keyed by their START OFFSET, which is what an FDE's
    // `cie_pointer` resolves to. A section may carry several (one per
    // augmentation shape); an FDE naming an offset that is not a CIE start
    // is a corrupt chain, not a recoverable condition.
    std::vector<std::pair<std::size_t, detail::DecodedCie>> cies;

    std::size_t pos = 0;
    while (pos + 4u <= section.size()) {
        std::size_t const recStart = pos;
        std::uint32_t length = 0;
        if (!detail::readU32LE(section, pos, length)) {
            return fail("truncated record length at byte offset "
                        + std::to_string(recStart) + " of the unwind section");
        }
        if (length == 0u) break;               // end-of-chain terminator
        if (length == 0xFFFFFFFFu) {
            return fail("the unwind section uses the 64-bit DWARF record "
                        "length form at byte offset " + std::to_string(recStart)
                        + ", which no producer this project has measured emits "
                          "and which this decoder does not implement rather "
                          "than mis-parse");
        }
        std::size_t const bodyStart = pos;
        if (bodyStart + length > section.size()) {
            return fail("record at byte offset " + std::to_string(recStart)
                        + " declares " + std::to_string(length)
                        + " body bytes, which runs past the end of the "
                          + std::to_string(section.size())
                        + "-byte unwind section");
        }
        std::size_t const recEnd = bodyStart + length;
        std::uint32_t idOrCiePtr = 0;
        if (!detail::readU32LE(section, pos, idOrCiePtr)) {
            return fail("truncated record at byte offset "
                        + std::to_string(recStart));
        }

        if (idOrCiePtr == 0u) {
            // ── CIE ──
            detail::DecodedCie cie;
            if (pos >= recEnd) return fail("truncated CIE version");
            std::uint8_t const version = section[pos++];
            if (version != 1u && version != 3u && version != 4u) {
                return fail("CIE at byte offset " + std::to_string(recStart)
                            + " declares version " + std::to_string(version)
                            + "; this decoder reads versions 1, 3 and 4 and "
                              "refuses rather than guess an unknown layout");
            }
            std::string aug;
            while (pos < recEnd && section[pos] != 0u) {
                aug.push_back(static_cast<char>(section[pos++]));
            }
            if (pos >= recEnd) return fail("unterminated CIE augmentation string");
            ++pos;                                   // the NUL
            std::uint64_t caf = 0;
            if (!detail::readULeb(section, pos, caf)) {
                return fail("truncated CIE code alignment factor");
            }
            if (caf == 0u) {
                return fail("CIE declares a code alignment factor of zero, so "
                            "every `advance_loc` would move the program counter "
                            "nowhere and every rule would land at offset 0");
            }
            cie.codeAlignmentFactor = caf;
            if (!detail::readSLeb(section, pos, cie.dataAlignmentFactor)) {
                return fail("truncated CIE data alignment factor");
            }
            if (cie.dataAlignmentFactor == 0) {
                return fail("CIE declares a data alignment factor of zero, "
                            "which makes every register save offset zero");
            }
            if (version == 1u) {
                // ★ VERSION 1 STORES THE COLUMN AS A SINGLE UBYTE, versions 3+
                //   as a ULEB128. They coincide for every column below 128 —
                //   which is every column this project has measured — so a
                //   decoder that read the wrong one would be right on every
                //   object it was tested against and wrong on the first target
                //   whose psABI numbers a column past 127.
                if (pos >= recEnd) return fail("truncated CIE return address column");
                cie.returnAddressColumn = section[pos++];
            } else if (!detail::readULeb(section, pos, cie.returnAddressColumn)) {
                return fail("truncated CIE return address column");
            }
            if (version == 4u) {
                // DWARF 4 inserted address_size + segment_selector_size.
                if (pos + 2u > recEnd) return fail("truncated CIE address size");
                pos += 2u;
            }

            // ── The augmentation string decides what follows ──
            if (!aug.empty() && aug[0] == 'z') {
                cie.hasZAugmentation = true;
                std::uint64_t augLen = 0;
                if (!detail::readULeb(section, pos, augLen)) {
                    return fail("truncated CIE augmentation data length");
                }
                std::size_t const augEnd = pos + static_cast<std::size_t>(augLen);
                if (augEnd > recEnd) {
                    return fail("CIE augmentation data runs past the record");
                }
                for (std::size_t ci = 1; ci < aug.size(); ++ci) {
                    switch (aug[ci]) {
                    case 'R':
                        if (pos >= augEnd) {
                            return fail("CIE augmentation 'R' has no encoding byte");
                        }
                        cie.fdePointerEncoding = section[pos++];
                        break;
                    case 'P':
                        return fail(
                            "CIE at byte offset " + std::to_string(recStart)
                            + " carries a 'P' (personality routine) augmentation. "
                              "The neutral call-frame representation this project "
                              "encodes every format's unwind table from carries no "
                              "personality pointer, so accepting it would drop the "
                              "language-specific handler and silently turn a caught "
                              "exception into a terminate. Refusing until the "
                              "pointer is carried");
                    case 'L':
                        return fail(
                            "CIE at byte offset " + std::to_string(recStart)
                            + " carries an 'L' (LSDA) augmentation. Each FDE then "
                              "points at a language-specific data area this project's "
                              "neutral call-frame representation cannot carry, and "
                              "zeroing it would leave every catch clause in the "
                              "merged function unreachable at run time");
                    case 'S':
                        return fail(
                            "CIE at byte offset " + std::to_string(recStart)
                            + " carries an 'S' (signal frame) augmentation. DSS emits "
                              "ONE shared CIE per module and its writer has no way to "
                              "vary this per function, so accepting it would drop the "
                              "flag and make the unwinder back the return address up "
                              "by one byte in a frame that was not entered by a call");
                    default:
                        return fail(
                            "CIE at byte offset " + std::to_string(recStart)
                            + " carries augmentation character '"
                            + std::string(1, aug[ci])
                            + "', which this decoder does not know. An unknown "
                              "augmentation character changes the layout of every "
                              "record that follows, so it is refused rather than "
                              "skipped");
                    }
                }
                pos = augEnd;                        // skip any unread padding
            } else if (!aug.empty()) {
                return fail("CIE at byte offset " + std::to_string(recStart)
                            + " carries a non-empty augmentation string '" + aug
                            + "' that does not begin with 'z', so its data has no "
                              "self-describing length and every record after it "
                              "would be parsed at the wrong offset");
            }
            if (cie.fdePointerEncoding == kDwEhPeOmit) {
                // No 'R' ⇒ the LSB default: an absolute, address-sized pointer.
                cie.fdePointerEncoding = kDwEhPeAbsPtr;
            }
            if (cie.fdePointerEncoding != kDwEhPePcRel4
                && !dwEhPeIsEightByteField(cie.fdePointerEncoding)) {
                return fail(
                    "CIE at byte offset " + std::to_string(recStart)
                    + " declares FDE pointer encoding 0x"
                    + std::string(1, "0123456789abcdef"[(cie.fdePointerEncoding >> 4) & 0xFu])
                    + std::string(1, "0123456789abcdef"[cie.fdePointerEncoding & 0xFu])
                    + ". This decoder resolves the three encodings every measured "
                      "producer emits — pcrel|sdata4 (0x1b, gcc on ELF), "
                      "pcrel|absptr (0x10, Apple clang on Mach-O) and absptr "
                      "(0x00) — and refuses the rest rather than read a field of "
                      "the wrong width and bind every FDE to the wrong function");
            }

            std::vector<CfiOp> unusedOps;
            std::span<std::uint8_t const> const initialBody =
                section.subspan(pos, recEnd - pos);
            if (auto e = detail::decodeCfaInstructions(initialBody, cie, rev,
                                                       /*initialPass=*/true,
                                                       cie.initial, unusedOps);
                !e.empty()) {
                return fail("CIE at byte offset " + std::to_string(recStart)
                            + ": " + e);
            }
            // ★★ A CIE THAT SAYS NOTHING ABOUT THE RETURN ADDRESS IS SAYING
            //    SOMETHING, AND OMITTING THIS COST A WHOLE PORT.
            //    DWARF's default rule for the return-address column is
            //    `same_value`: at function entry the return address is still
            //    wherever the column names, which on a link-register ABI is a
            //    real register. `aarch64-linux-gnu-gcc` relies on exactly that
            //    — ✔MEASURED, its CIE carries `DW_CFA_def_cfa r31 ofs 0` and
            //    NOTHING ELSE, while x86_64 gcc spells its `DW_CFA_offset r16
            //    at cfa-8` out. Leaving the field unset therefore produced an
            //    entry state that disagreed with the one DSS's own producer
            //    derives from `cc.linkRegister`, and `buildEhFrame` refused the
            //    whole image with "two functions in one module declare
            //    different frame entry states" — loudly, and only on the
            //    aarch64 leg. The x86_64 leg had already linked and RUN green.
            //    Deriving it here rather than letting the writer paper over a
            //    mismatch keeps that refusal meaningful for the case where two
            //    entry states really do differ.
            if (!cie.initial.returnAddressAtCfaOffset.has_value()
                && !cie.initial.returnAddressRegister.has_value()) {
                if (auto const ord = rev.physicalOnly(cie.returnAddressColumn);
                    ord.has_value()) {
                    cie.initial.returnAddressRegister = *ord;
                }
                // No physical register behind the column (x86_64's synthetic
                // 16) and no stated rule leaves BOTH empty, which is the
                // representation's way of saying the return address is not
                // described at entry — the honest reading of a silent CIE on
                // a target where the column is not a register.
            }
            cies.emplace_back(recStart, cie);
        } else {
            // ── FDE ──
            //
            // `cie_pointer` in `.eh_frame` is the DISTANCE BACK from this
            // field to the CIE's first byte — NOT an absolute section offset
            // (that is `.debug_frame`, whose identical field means the other
            // thing; reading one as the other yields a record every parser
            // accepts and mis-associates).
            std::size_t const ciePtrFieldOffset = bodyStart;
            if (idOrCiePtr > ciePtrFieldOffset) {
                return fail("FDE at byte offset " + std::to_string(recStart)
                            + " names a CIE " + std::to_string(idOrCiePtr)
                            + " bytes back from offset "
                            + std::to_string(ciePtrFieldOffset)
                            + ", which is before the start of the section");
            }
            std::size_t const cieStart = ciePtrFieldOffset - idOrCiePtr;
            detail::DecodedCie const* cie = nullptr;
            for (auto const& [start, c] : cies) {
                if (start == cieStart) { cie = &c; break; }
            }
            if (cie == nullptr) {
                return fail("FDE at byte offset " + std::to_string(recStart)
                            + " points at byte offset " + std::to_string(cieStart)
                            + ", where no CIE begins");
            }

            DecodedFde fde;
            fde.pointerEncoding = cie->fdePointerEncoding;
            fde.initialLocationFieldOffset = static_cast<std::uint32_t>(pos);
            std::uint64_t addressRange = 0;
            // ⚠ THE WIDTH IS DECIDED BY THE ONE PREDICATE, at this site AND at
            // the CIE gate above. Spelling the set twice is how the gate comes
            // to admit an encoding this arm then reads at the wrong width —
            // and a mis-read `initial_location` binds every FDE to the wrong
            // function while reporting success.
            if (dwEhPeIsEightByteField(cie->fdePointerEncoding)) {
                std::uint64_t abs64 = 0;
                if (!detail::readU64LE(section, pos, abs64)) {
                    return fail("truncated FDE initial_location");
                }
                fde.storedInitialLocation = static_cast<std::int64_t>(abs64);
                if (!detail::readU64LE(section, pos, addressRange)) {
                    return fail("truncated FDE address_range");
                }
            } else {
                std::uint32_t lo32 = 0;
                if (!detail::readU32LE(section, pos, lo32)) {
                    return fail("truncated FDE initial_location");
                }
                fde.storedInitialLocation =
                    static_cast<std::int64_t>(static_cast<std::int32_t>(lo32));
                std::uint32_t range = 0;
                if (!detail::readU32LE(section, pos, range)) {
                    return fail("truncated FDE address_range");
                }
                addressRange = range;
            }
            if (addressRange > 0xFFFFFFFFull) {
                return fail("FDE at byte offset " + std::to_string(recStart)
                            + " declares a " + std::to_string(addressRange)
                            + "-byte address range, past what a function extent "
                              "can be");
            }
            fde.cfi.codeLength = static_cast<std::uint32_t>(addressRange);
            fde.cfi.initial    = cie->initial;

            if (cie->hasZAugmentation) {
                std::uint64_t augLen = 0;
                if (!detail::readULeb(section, pos, augLen)) {
                    return fail("truncated FDE augmentation data length");
                }
                if (augLen != 0u) {
                    return fail(
                        "FDE at byte offset " + std::to_string(recStart)
                        + " carries " + std::to_string(augLen)
                        + " bytes of augmentation data (a language-specific data "
                          "area pointer). The neutral call-frame representation "
                          "carries no LSDA pointer, and dropping it would leave "
                          "the function's catch clauses unreachable at run time "
                          "while the table still looked complete");
                }
            }

            if (pos > recEnd) return fail("FDE header runs past its record");
            std::span<std::uint8_t const> const insns =
                section.subspan(pos, recEnd - pos);
            CfiInitialState unusedInitial = cie->initial;
            if (auto e = detail::decodeCfaInstructions(insns, *cie, rev,
                                                       /*initialPass=*/false,
                                                       unusedInitial,
                                                       fde.cfi.ops);
                !e.empty()) {
                return fail("FDE at byte offset " + std::to_string(recStart)
                            + ": " + e);
            }
            if (auto const why = validateCfiFunction(fde.cfi); !why.empty()) {
                return fail("FDE at byte offset " + std::to_string(recStart)
                            + " decoded to a malformed rule stream: " + why);
            }
            out.fdes.push_back(std::move(fde));
        }
        pos = recEnd;
    }
    return out;
}

} // namespace dss::link::format
