#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"

#include <cstddef>

// LIR PEEPHOLE — plan 22 OPT8, the post-regalloc instruction-level cleanup.
//
// Runs between `legalizeTwoAddress` and `materializeCallingConvention`, and
// produces a fresh `Lir` module with the instructions its rules prove
// redundant DELETED. It is the only LIR pass that removes instructions;
// every other one is 1→(1 or more).
//
// ── RULE R1: REDUNDANT-COPY ELIMINATION ─────────────────────────────────
//
// An instruction is deleted iff it is the DECLARED register-to-register
// MOVE for its result's register class, copying a physical register into
// ITSELF at the register's FULL width. `mov %r13,%r13` writes the value it
// just read; nothing downstream can observe that it ran.
//
// ★★★ THE OPCODE TEST IS AN IDENTITY TEST AGAINST
// `registerClassOps[].move`, NEVER A MNEMONIC OR A BYTE PATTERN, AND THAT
// IS THE WHOLE CORRECTNESS ARGUMENT.
//
// ✔MEASURED on the shipped x86_64 target, 2026-08-25: THREE distinct LIR
// opcodes — `mov` (width 64, REX.W 0x8B), `trunc` (width 32, 0x8B) and
// `zext` (width 32, 0x8B) — encode to bytes that every disassembler prints
// as `mov`, and only ONE of them is a no-op when source and destination
// name the same register. `trunc %r14d,%r14d` and `zext %r15d,%r15d` CLEAR
// THE UPPER 32 BITS of a 64-bit register; deleting either is a wrong
// answer, not a missed optimization. A peephole that matched the mnemonic
// text, or the emitted opcode byte, would delete all three.
//
// So the rule asks the SCHEMA which opcode is this class's copy
// (`TargetSchema::regClassOpOpcode(cls, RegClassOp::Move)` — the same
// resolution `lir_2addr_legalize` uses to SYNTHESIZE a copy) and compares
// opcode handles. `trunc` and `zext` are different handles and never match,
// on any target, without this pass knowing they exist.
//
// ★ THE GUARD REJECTS MORE THAN IT ACCEPTS, AND THE MARGIN IS THE POINT.
// ✔MEASURED over all 585 dumping examples of `examples/c/**`, 2026-08-25:
// instructions whose sole operand is a register EQUAL to their result number
// **12021** at post-callconv, of which only **5575** are the class MOVE.
// The other 6446 are `zext` (3928), `sext` (1283), `trunc` (404), `not`
// (219), `neg` (219), `shl` (103), `shr_l` (102), `movaps` (121 -- the FPR
// class move, held back by the LOWERING rather than by this rule; see the
// width clause in the .cpp), `fpcvt`, `clz`, `popcount`, `ctz`, `bswap`,
// `shr_a` and three
// `call`s — every one of them a live computation that reads a register and
// writes a DIFFERENT value back to it. A rule shaped as "result == its only
// operand ⇒ delete" would silently delete all 6446.
//
// The width clause is the second, independent guard on the same hazard: a
// copy NARROWER than the register it names is a truncation with a
// zero-extending side effect on every machine that has partial-register
// writes, so R1 fires only when the instruction's operation width
// (`lirInstWidthBits`) equals the register's declared full width
// (`registers[].widthBytes * 8`). Both guards are target VOCABULARY; the
// pass contains no target name, no mnemonic string and no width constant.
//
// ── WHY *BEFORE* CALLCONV, WHICH IS NOT WHERE THIS PASS FIRST WENT ──────
//
// Redundant copies are minted by the ALLOCATOR — `rewriteWithAllocation`
// leaves one wherever the allocator handed a copy's source and destination
// the same physical register (✔MEASURED: `p15 = mov p15` in the
// post-rewrite dump of `examples/c/umulh_intrinsic`). The obvious
// placement is therefore LAST, after `materializeCallingConvention`, so the
// argument/return moves callconv mints are cleaned too.
//
// ✔MEASURED over all 585 dumping examples of `examples/c/**`, 2026-08-25:
// the identity class-move count is **5575 at post-rewrite and 5575 at
// post-callconv** — callconv mints exactly ZERO of them. The late
// placement buys NOTHING, and it costs something real:
// `LirCallconvResult::perFuncCfi` is keyed BY `LirInstId` and joined
// against the assembler's `sourceMap` in `compile_pipeline.cpp` to build
// `.eh_frame`. A rebuild AFTER callconv renumbers those instructions, so
// every CFI row would describe a DIFFERENT instruction than the one that
// moved the frame — an unwind table that loads clean and walks into the
// wrong frame, which the target register table's own docblock calls
// strictly worse than no table at all.
//
// So the pass runs where the copies actually are and where no by-index
// side channel is downstream of it. Deleting an instruction here cannot
// disturb anything callconv then computes: `LirAllocation` is keyed per
// FUNCTION (cross-checked on `originalSymbol`, which this rebuild
// preserves), LIR branch targets are block ids rather than offsets, and
// frame slots are frame-relative — no offset in the module is a function
// of the instruction count.
//
// ── WHAT THE PASS REFUSES TO TOUCH ─────────────────────────────────────
//
// An instruction naming a per-instruction register-constraint pool entry
// is NEVER deleted, even when R1 otherwise matches. The pool is referenced
// BY INDEX from the instruction stream, and `verifyLirRebuild` counts the
// references on both sides — deleting the only namer of an entry orphans it
// (`L_SideStructureReferenceLost`). Keeping the instruction is the
// fail-safe arm: a redundant copy that survives costs one instruction, an
// orphaned constraint set costs the build.
//
// The pass is target-blind, source-language-blind and format-blind. It
// never branches on `schema.name()`.

namespace dss {

struct DSS_EXPORT LirPeepholeResult {
    Lir         lir{};
    // Number of LIR functions the pass STARTED rewriting. Mirrors
    // `LirTwoAddrLegalizeResult::expectedFuncCount` — the parallel-index
    // discipline every LIR rebuild pass reports.
    std::size_t expectedFuncCount = 0;
    // Instructions R1 deleted, module-wide. Reported as an Info note
    // (`L_PeepholeSummary`) once per module when non-zero, and read by the
    // pass's differential tests.
    std::size_t redundantCopiesRemoved = 0;
    // False iff the rebuild left the module unusable (a terminator the
    // shared dispatch refused). Same HARD channel as the sibling passes.
    bool        rebuilt = false;

    // Shape-consistency is the success channel, exactly as
    // `LirTwoAddrLegalizeResult::ok()`: the pass must have rebuilt as many
    // functions as it started with. An EMPTY module (0 functions — a
    // declaration-only TU) is a VALID success.
    [[nodiscard]] bool ok() const noexcept {
        return rebuilt && lir.moduleFuncCount() == expectedFuncCount;
    }
};

[[nodiscard]] DSS_EXPORT LirPeepholeResult
runLirPeephole(Lir const&          src,
               TargetSchema const& schema,
               DiagnosticReporter& reporter);

} // namespace dss
