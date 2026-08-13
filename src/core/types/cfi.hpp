#pragma once

#include "core/export.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════
// Call Frame Information — the UNIVERSAL, PC-KEYED unwind representation.
// Plan 15 §2.5 ("CFI / unwind in v1"), closing the representation half of
// `D-UNWIND-NO-EH-FRAME-ANY-LANGUAGE-ON-ELF-OR-MACHO` and
// `D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED`.
// ═══════════════════════════════════════════════════════════════════════
//
// ── WHY THIS EXISTS AND WHY `FrameUnwindInfo` COULD NOT BE IT ──
//
// `asm.hpp`'s `FrameUnwindInfo` (c114) is a projection of ONE canonical
// prologue shape: `sub sp, N` followed by one `store [sp+off], reg` per
// callee-save — "no push, no frame pointer". It has **no PC dimension at
// all**. Its consumer (the pe64 `.pdata`/`.xdata` builder) therefore had
// to RE-DERIVE every prologue byte offset by assuming DSS's own
// instruction encodings (`sub rsp,imm32` is 7 bytes; a GPR spill store is
// 8; an xmm8-15 spill is 9), guarded by an opcode check on the FIRST
// instruction only.
//
// CFI is not a frame SHAPE — it is a **per-PC state machine**. At every
// byte offset in a function there is an answer to "where is the CFA, and
// where is each callee-saved register?", and that answer CHANGES as the
// prologue executes and again as the epilogue tears the frame down. A
// representation with no PC dimension cannot express:
//   * the epilogue (after `add sp, N` the CFA rule is different — unwinding
//     from the last two instructions of a function is WRONG without it);
//   * a frame-pointer capture partway through a prologue;
//   * hand-written assembly, whose `.cfi_*` directives are, literally, a
//     PC-keyed op stream and nothing else.
//
// So the model here is the op stream: **(pcOffset, rule-change)**. That is
// simultaneously
//   (a) exactly what a `.s` file's `.cfi_*` directives say,
//   (b) exactly what a `FrameLayout` + the assembler's MEASURED per-
//       instruction byte offsets produce, and
//   (c) exactly what DWARF `.eh_frame` FDEs encode
//       (`DW_CFA_advance_loc` + a rule opcode).
// Folding an op stream into per-PC state is a forward scan (`foldCfiOps`
// below); recovering an op stream from per-PC state would require diffing.
// The op stream is therefore the primitive and the state is derived.
//
// ── WHAT THIS TYPE DELIBERATELY DOES NOT CARRY ──
//
// **No format identity.** Nothing here knows about ELF, Mach-O, PE, DWARF
// CIE/FDE layout, or Win64 `UNWIND_INFO`. The ENCODING of an unwind table
// is per-format and lives in `src/link/format/` (the sanctioned
// realization tier — the same place the `.pdata`/`.xdata` encoder already
// lives). A format that cannot express a rule REFUSES and names it; see
// `cfiOpKindName` below, which exists so a refusal can print the offending
// rule rather than a number.
//
// **No DWARF register numbers.** A register is identified by its DSS
// PHYSICAL ORDINAL — the same `std::uint16_t` that indexes
// `TargetSchema::registerInfo(ordinal)` and that `LirReg::id` already
// carries. This is bar item 1b (reuse the pipeline's existing vocabulary):
// the ordinal is the register identity every other tier already speaks.
//
// ★ The DWARF number is a DIFFERENT PERMUTATION of the same registers and
//   getting it wrong produces an unwind table that is CONFIDENTLY WRONG:
//   on x86_64, `%rbp` is DWARF register 6 but hardware encoding 5, and
//   `%rsp` is DWARF 7 but hardware encoding 4. A debugger handed the
//   hardware number walks the wrong frame and reports a plausible-looking
//   backtrace. The ordinal→DWARF mapping is a per-target psABI table and
//   belongs in `*.target.json`; the `.eh_frame` writer performs the lookup
//   and REFUSES (never guesses) when the target declares none.
//
// ── PC SEMANTICS (the one thing that must not be ambiguous) ──
//
// `CfiOp::pcOffset` is the byte offset, WITHIN the owning function, at
// which the new rule IS IN EFFECT — i.e. the offset of the first byte PAST
// the instruction that established it. This is deliberately the same
// convention as both consumers:
//   * DWARF: `DW_CFA_advance_loc` moves the current row to the address
//     where the following rules apply, which is after the establishing
//     instruction retires;
//   * Win64: `UNWIND_CODE::CodeOffset` is documented as "the offset from
//     the beginning of the prologue of the end of the instruction that
//     performs this operation".
// They agree because they are describing the same physical fact. An
// implementation that used "the offset OF the instruction" instead would
// be off by one instruction length in both directions and would still
// look plausible in a dump.

namespace dss {

// ── A register named in a CFI rule ──────────────────────────────────────
//
// Either a physical register (by DSS ordinal — see the header note above)
// or the RETURN-ADDRESS COLUMN, which on some targets is not a register at
// all. On x86_64 the return address lives in a synthetic DWARF column (16)
// that no physical register maps to; on AArch64 it is x30, a real register
// that ALSO has an ordinary DWARF number. Modelling "the return address"
// as its own thing rather than as some register lets both targets be
// described without the representation knowing which case it is in — the
// per-target resolution happens once, in the writer.
struct DSS_EXPORT CfiRegRef {
    std::uint16_t ordinal        = 0;      // physical-register ordinal (registerInfo key)
    bool          isReturnAddress = false; // true ⇒ `ordinal` is meaningless; this is the RA column

    [[nodiscard]] static constexpr CfiRegRef physical(std::uint16_t ord) noexcept {
        return CfiRegRef{ord, false};
    }
    [[nodiscard]] static constexpr CfiRegRef returnAddress() noexcept {
        return CfiRegRef{0, true};
    }
    [[nodiscard]] constexpr bool operator==(CfiRegRef const& o) const noexcept {
        return isReturnAddress == o.isReturnAddress
            && (isReturnAddress || ordinal == o.ordinal);
    }
};

// ── The rule vocabulary ─────────────────────────────────────────────────
//
// One enumerator per thing a producer can SAY. The set is fixed by the
// union of what the two producers can express:
//
//   * the GNU `.cfi_*` directive family, which is what a hand-written or
//     compiler-emitted `.s` carries (18 spellings are currently declared
//     as `ignoredAnnotation` in the shipped AT&T dialect — that accept-and-
//     drop is `D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED`); and
//   * a callconv `FrameLayout`, which says only "the CFA moved by N" and
//     "register R is saved at CFA+K".
//
// The FrameLayout producer uses a strict subset. Nothing here is invented
// for a hypothetical future producer: every enumerator has a `.cfi_*`
// spelling that a real assembler emits, named in its comment.
enum class CfiOpKind : std::uint8_t {
    // CFA (Canonical Frame Address) rules — where the caller's stack
    // pointer was at the moment this function was entered.
    DefCfa            = 0,  // .cfi_def_cfa reg, off        — CFA = reg + off
    DefCfaRegister    = 1,  // .cfi_def_cfa_register reg    — CFA = reg + (unchanged off)
    DefCfaOffset      = 2,  // .cfi_def_cfa_offset off      — CFA = (unchanged reg) + off
    AdjustCfaOffset   = 3,  // .cfi_adjust_cfa_offset delta — CFA offset += delta

    // Per-register save rules — where a callee-saved register's ENTRY
    // value can be found once execution has reached this PC.
    RegAtCfaOffset    = 4,  // .cfi_offset reg, off      — value is AT [CFA + off]
    RegValIsCfaOffset = 5,  // .cfi_val_offset reg, off  — value IS (CFA + off), not a load
    RegInRegister     = 6,  // .cfi_register reg, srcReg — value is in another register
    RegSameValue      = 7,  // .cfi_same_value reg       — value is unchanged; do not restore
    RegUndefined      = 8,  // .cfi_undefined reg        — value is not recoverable here
    RegRestoreInitial = 9,  // .cfi_restore reg          — revert to this function's ENTRY rule

    // The state stack. `.cfi_remember_state` / `.cfi_restore_state` are how
    // real compiler output describes a shared epilogue reached from several
    // paths: push the whole rule set, mutate it, pop it back.
    RememberState     = 10, // .cfi_remember_state
    RestoreState      = 11, // .cfi_restore_state
};

inline constexpr std::size_t kCfiOpKindCount = 12;

// Named so a REFUSAL can print the rule it cannot encode. A format walker
// that prints "unsupported CFI opcode 5" has told the reader nothing; the
// whole subject of `D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED` is unwind
// information that vanishes without saying so, and a numeric refusal is
// only marginally better than a silent one.
[[nodiscard]] constexpr std::string_view cfiOpKindName(CfiOpKind k) noexcept {
    switch (k) {
        case CfiOpKind::DefCfa:            return "def_cfa";
        case CfiOpKind::DefCfaRegister:    return "def_cfa_register";
        case CfiOpKind::DefCfaOffset:      return "def_cfa_offset";
        case CfiOpKind::AdjustCfaOffset:   return "adjust_cfa_offset";
        case CfiOpKind::RegAtCfaOffset:    return "offset";
        case CfiOpKind::RegValIsCfaOffset: return "val_offset";
        case CfiOpKind::RegInRegister:     return "register";
        case CfiOpKind::RegSameValue:      return "same_value";
        case CfiOpKind::RegUndefined:      return "undefined";
        case CfiOpKind::RegRestoreInitial: return "restore";
        case CfiOpKind::RememberState:     return "remember_state";
        case CfiOpKind::RestoreState:      return "restore_state";
    }
    return "<unknown-cfi-op>";
}

// True for the four ops that change the CFA rule; false for the six
// per-register rules and the two state-stack ops. Exists so a consumer
// never has to re-enumerate the enum to ask this (the "define the
// complement, never enumerate the members" discipline).
[[nodiscard]] constexpr bool cfiOpTouchesCfa(CfiOpKind k) noexcept {
    return k == CfiOpKind::DefCfa || k == CfiOpKind::DefCfaRegister
        || k == CfiOpKind::DefCfaOffset || k == CfiOpKind::AdjustCfaOffset;
}

// True for the six ops that carry a `reg` operand naming a register whose
// save rule changes. `DefCfaRegister`/`DefCfa` also carry a register, but
// it is the CFA BASE, not a saved register — a consumer that conflates the
// two writes a table describing the wrong thing.
[[nodiscard]] constexpr bool cfiOpTouchesRegRule(CfiOpKind k) noexcept {
    return k == CfiOpKind::RegAtCfaOffset || k == CfiOpKind::RegValIsCfaOffset
        || k == CfiOpKind::RegInRegister  || k == CfiOpKind::RegSameValue
        || k == CfiOpKind::RegUndefined   || k == CfiOpKind::RegRestoreInitial;
}

// ── One rule change at one PC ───────────────────────────────────────────
struct DSS_EXPORT CfiOp {
    // Byte offset within the owning function at which this rule takes
    // effect. See the PC-SEMANTICS note in the header docblock: this is
    // the offset PAST the establishing instruction, matching both DWARF's
    // `DW_CFA_advance_loc` target and Win64's `UNWIND_CODE::CodeOffset`.
    std::uint32_t pcOffset = 0;
    CfiOpKind     kind     = CfiOpKind::DefCfa;
    // The register this rule is about: the CFA base for `DefCfa` /
    // `DefCfaRegister`, otherwise the saved register. Unused (and left
    // default) by the offset-only and state-stack ops.
    CfiRegRef     reg{};
    // `RegInRegister` only — the register currently HOLDING `reg`'s
    // entry value.
    CfiRegRef     srcReg{};
    // Byte offset or delta, in bytes and UNFACTORED. DWARF stores these
    // divided by the CIE's data-alignment factor; that division is an
    // ENCODING concern and happens in the DWARF writer, not here — a
    // representation carrying pre-factored values would be unusable by
    // Win64, whose scaling factor is different (8).
    std::int64_t  offset   = 0;
};

// ── The state at function entry ─────────────────────────────────────────
//
// Every function of a given calling convention starts in the same state,
// which is why DWARF hoists it into the shared CIE. All of it is DERIVED
// FROM THE CALLING CONVENTION the target already declares — no new config:
//
//   * `cfaRegister`   ← `cc.stackPointer` (the SP ordinal)
//   * `cfaOffset`     ← `cc.callPushBytes` (bytes the CALL instruction
//                        itself pushed: 8 on x86_64, 0 on AArch64)
//   * `returnAddressAtCfaOffset` — present iff the call pushed the return
//                        address, in which case it sits at CFA minus what
//                        was pushed. Absent ⇒ the return address is in
//                        `returnAddressRegister` (a link-register ABI).
//   * `returnAddressRegister` ← `cc.linkRegister` when it has one.
//
// ★ `callPushBytes` doing double duty here is not a coincidence: it is
//   defined as the SP delta at call entry, which is precisely the CFA
//   offset at function entry. The frame-alignment rule and the unwind rule
//   are two consequences of the same physical fact, so they must read the
//   same field — deriving one of them independently is how they drift.
struct DSS_EXPORT CfiInitialState {
    std::uint16_t                cfaRegister = 0;
    std::int64_t                 cfaOffset   = 0;
    std::optional<std::int64_t>  returnAddressAtCfaOffset;
    std::optional<std::uint16_t> returnAddressRegister;

    [[nodiscard]] constexpr bool operator==(CfiInitialState const&) const = default;
};

// ── One function's complete unwind description ──────────────────────────
struct DSS_EXPORT CfiFunction {
    // Total length of the function's machine code, in bytes. Bounds every
    // `pcOffset`; becomes the FDE's address_range and the RUNTIME_FUNCTION
    // extent.
    std::uint32_t codeLength = 0;

    // The PC at which the frame is fully established — the offset past the
    // last prologue instruction.
    //
    // ★ Present iff a producer could state it as a MEASURED fact. The
    //   `FrameLayout` producer can (it knows exactly which instructions it
    //   emitted and the assembler recorded where they landed). A `.s` file
    //   generally cannot: GNU `.cfi_*` has no "prologue ends here" verb.
    //   Win64 `UNWIND_INFO` has a mandatory `SizeOfProlog` field, so a
    //   format REQUIRING this must REFUSE a function that lacks it rather
    //   than picking a number — an invented `SizeOfProlog` makes the OS
    //   unwinder mis-classify PCs as mid-prologue.
    std::optional<std::uint32_t> prologueEndPc;

    // Entry state (above). One per function even though every function of
    // a calling convention shares it: the writer that hoists it into a
    // shared CIE is the one that gets to notice they are equal, and a
    // producer that had to reach for a module-level side-table to state a
    // per-function fact is how the two drift apart.
    CfiInitialState initial{};

    // Rule changes, REQUIRED to be sorted by non-decreasing `pcOffset`.
    // `validateCfiFunction` enforces it; DWARF's `advance_loc` is a
    // forward-only delta and Win64's unwind codes are a descending scan,
    // so an out-of-order stream silently produces a wrong table in both.
    std::vector<CfiOp> ops;

    // `.cfi_signal_frame` — this frame was entered by a signal/exception
    // dispatch rather than a CALL, so the unwinder must NOT back the
    // return address up by one byte when looking up the caller's FDE.
    // Carried because an assembly producer can say it; the `FrameLayout`
    // producer never sets it.
    bool signalFrame = false;

    [[nodiscard]] bool empty() const noexcept { return ops.empty(); }
};

// ── The per-PC state an op stream folds to ──────────────────────────────
//
// The derived view. A consumer asking "what is true at PC X" folds the ops
// with `pcOffset <= X`. Register rules are held as a flat vector rather
// than a map: a frame has a handful of saved registers, the vector is
// ordered by first mention (deterministic output — a map ordering would
// leak into emitted bytes and break the plan-15 §5 byte-identical-rebuild
// acceptance criterion).
struct DSS_EXPORT CfiRegRule {
    CfiRegRef reg{};
    CfiOpKind kind   = CfiOpKind::RegUndefined;
    CfiRegRef srcReg{};
    std::int64_t offset = 0;
};

struct DSS_EXPORT CfiFrameState {
    std::uint16_t           cfaRegister = 0;
    std::int64_t            cfaOffset   = 0;
    std::vector<CfiRegRule> regRules;

    [[nodiscard]] CfiRegRule const* rule(CfiRegRef r) const noexcept {
        for (auto const& e : regRules) { if (e.reg == r) return &e; }
        return nullptr;
    }
    void setRule(CfiRegRule const& nr) {
        for (auto& e : regRules) { if (e.reg == nr.reg) { e = nr; return; } }
        regRules.push_back(nr);
    }
};

// The entry state as a `CfiFrameState` — the row every fold starts from
// and the row `RegRestoreInitial` reverts a register to.
[[nodiscard]] inline CfiFrameState cfiEntryState(CfiInitialState const& init) {
    CfiFrameState s;
    s.cfaRegister = init.cfaRegister;
    s.cfaOffset   = init.cfaOffset;
    if (init.returnAddressAtCfaOffset.has_value()) {
        s.setRule(CfiRegRule{CfiRegRef::returnAddress(),
                             CfiOpKind::RegAtCfaOffset,
                             CfiRegRef{},
                             *init.returnAddressAtCfaOffset});
    } else if (init.returnAddressRegister.has_value()) {
        // A link-register ABI: at entry the return address is still in the
        // link register, i.e. "unchanged". `same_value` is exactly that,
        // and is what clang emits (by omission — same_value is the DWARF
        // default for the RA column) on AArch64.
        s.setRule(CfiRegRule{CfiRegRef::returnAddress(),
                             CfiOpKind::RegSameValue,
                             CfiRegRef::physical(*init.returnAddressRegister),
                             0});
    }
    return s;
}

// Fold every op with `pcOffset <= pc` and return the resulting state.
//
// Returns nullopt on a malformed stream — an unbalanced `restore_state`.
// That is a REFUSAL, not a repair: a `.cfi_restore_state` with nothing
// remembered means the producer's own model of the frame is wrong, and
// silently treating it as a no-op emits a table describing a frame that
// never existed. Callers pair this with `validateCfiFunction`, which
// reports the same condition with a message.
[[nodiscard]] inline std::optional<CfiFrameState>
foldCfiOps(CfiFunction const& fn, std::uint32_t pc) {
    CfiFrameState const entry = cfiEntryState(fn.initial);
    CfiFrameState state = entry;
    std::vector<CfiFrameState> stack;
    for (auto const& op : fn.ops) {
        if (op.pcOffset > pc) break;
        switch (op.kind) {
            case CfiOpKind::DefCfa:
                state.cfaRegister = op.reg.ordinal;
                state.cfaOffset   = op.offset;
                break;
            case CfiOpKind::DefCfaRegister:
                state.cfaRegister = op.reg.ordinal;
                break;
            case CfiOpKind::DefCfaOffset:
                state.cfaOffset = op.offset;
                break;
            case CfiOpKind::AdjustCfaOffset:
                state.cfaOffset += op.offset;
                break;
            case CfiOpKind::RegRestoreInitial: {
                auto const* init = entry.rule(op.reg);
                state.setRule(init != nullptr
                                  ? *init
                                  : CfiRegRule{op.reg, CfiOpKind::RegUndefined,
                                               CfiRegRef{}, 0});
                break;
            }
            case CfiOpKind::RegAtCfaOffset:
            case CfiOpKind::RegValIsCfaOffset:
            case CfiOpKind::RegInRegister:
            case CfiOpKind::RegSameValue:
            case CfiOpKind::RegUndefined:
                state.setRule(CfiRegRule{op.reg, op.kind, op.srcReg, op.offset});
                break;
            case CfiOpKind::RememberState:
                stack.push_back(state);
                break;
            case CfiOpKind::RestoreState:
                if (stack.empty()) return std::nullopt;
                state = stack.back();
                stack.pop_back();
                break;
        }
    }
    return state;
}

// ── Structural validation ───────────────────────────────────────────────
//
// Returns an empty string when the function is well-formed, otherwise a
// human-readable reason. Deliberately returns the MESSAGE rather than a
// bool so every caller's diagnostic names the specific defect — a
// `validate()` returning false forces each caller to invent its own
// wording, and the wordings then disagree.
//
// Header-only + `DiagnosticReporter`-free on purpose: this type lives
// below the tiers that own reporters (the assembler tags `AssembledFunction`
// with it, the linker's format walkers consume it), so it must not drag a
// reporting dependency downward.
[[nodiscard]] inline std::string validateCfiFunction(CfiFunction const& fn) {
    std::uint32_t last  = 0;
    std::int32_t  depth = 0;
    for (std::size_t i = 0; i < fn.ops.size(); ++i) {
        CfiOp const& op = fn.ops[i];
        if (op.pcOffset < last) {
            return "CFI op #" + std::to_string(i) + " ("
                 + std::string(cfiOpKindName(op.kind)) + ") is at pc+"
                 + std::to_string(op.pcOffset)
                 + " but the previous op is at pc+" + std::to_string(last)
                 + " — the op stream must be sorted by non-decreasing pc";
        }
        if (op.pcOffset > fn.codeLength) {
            return "CFI op #" + std::to_string(i) + " ("
                 + std::string(cfiOpKindName(op.kind)) + ") is at pc+"
                 + std::to_string(op.pcOffset)
                 + ", past the function's " + std::to_string(fn.codeLength)
                 + "-byte extent";
        }
        last = op.pcOffset;
        if (op.kind == CfiOpKind::RememberState) { ++depth; }
        if (op.kind == CfiOpKind::RestoreState) {
            if (depth == 0) {
                return "CFI op #" + std::to_string(i)
                     + " is a restore_state with no matching remember_state";
            }
            --depth;
        }
        // A rule about the CFA base register that names the RA column, or a
        // saved-register rule on a CFA op, is a producer bug that would
        // otherwise encode as a plausible-looking wrong table.
        if ((op.kind == CfiOpKind::DefCfa || op.kind == CfiOpKind::DefCfaRegister)
            && op.reg.isReturnAddress) {
            return "CFI op #" + std::to_string(i) + " ("
                 + std::string(cfiOpKindName(op.kind))
                 + ") names the return-address column as the CFA base register";
        }
    }
    if (fn.prologueEndPc.has_value() && *fn.prologueEndPc > fn.codeLength) {
        return "prologueEndPc " + std::to_string(*fn.prologueEndPc)
             + " is past the function's " + std::to_string(fn.codeLength)
             + "-byte extent";
    }
    return {};
}

} // namespace dss
