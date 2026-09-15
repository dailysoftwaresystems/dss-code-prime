#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_reg.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

// Shared substrate for LIR transformation passes (rewrite, callconv,
// future inlining/optimization passes). Folds the cycle-3b / ML7
// duplication identified by the simplifier as D-ML7-1.1: every pass
// that walks an input `Lir` and builds a fresh one re-implements the
// same diagnostic-emission, block-ref remapping, and terminator
// dispatch — all of which are tier-invariant (target-blind, source-
// blind, transformation-blind). Hoist once.

namespace dss::lir_pass_util {

// Note: the `report(reporter, code, severity, msg)` shim previously
// lived here. Hoisted to `core/types/diagnostic_reporter.hpp` at LK10
// cycle 3 post-fold #2 (D-LK10-8 closure) — it's tier-agnostic and
// driver-tier callers (program/input_resolver) were dragging in LIR
// headers for it. Consumers now `using dss::report;` (or call
// `dss::report` qualified) without a LIR dependency.

// Translate a NON-vreg operand: BlockRef gets remapped to dest-side
// block ids; everything else passes through. Vreg/spill resolution
// is pass-specific (rewrite handles vreg→phys+scratch; callconv
// doesn't touch vregs since they're already physical) and is NOT
// included here.
[[nodiscard]] DSS_EXPORT LirOperand
remapBlockRef(LirOperand const& op,
              std::unordered_map<std::uint32_t, LirBlockId> const& srcToDst);

// Emit the rewritten terminator. The per-inst loop in each pass
// translates `newOps`; this routes to the matching `LirBuilder`
// entrypoint via `info->terminatorKind` (single source of truth,
// shared with the `.dsslir` parser dispatch — replaces the
// successor-count-counting heuristic that earlier draft used).
//
// `passName` is the prefix used in any error diagnostic (e.g.
// "rewrite", "callconv") so the reporter caller is identifiable.
//
// Returns false on Switch (reserved — future LIR Switch lowering will
// add the dispatch) or `terminatorKind == None` (substrate invariant
// violation — `info->isTerminator()` should have already filtered the
// call site).
[[nodiscard]] DSS_EXPORT bool
emitTerminator(LirBuilder& b, std::uint16_t op,
               TargetOpcodeInfo const* info,
               std::span<LirBlockId const> succs,
               std::span<LirOperand const> newOps,
               std::uint32_t payload,
               std::uint8_t  flags,
               std::unordered_map<std::uint32_t, LirBlockId> const& srcToDst,
               std::string_view passName,
               DiagnosticReporter& reporter);

// ── D-OPT-JCC-FALLTHROUGH — DOES THIS TARGET SPELL A FALLTHROUGH BRANCH? ──
//
// A branch terminator materializes EVERY edge it owns as BYTES. The shipped
// `jcc` rows encode operand[0] as the taken displacement AND operand[1] as a
// TRAILING UNCONDITIONAL JUMP (x86 `0F 8x rel32; E9 rel32`; arm64 `B.cond` +
// `B`), precisely so LIR block layout never has to guarantee fallthrough
// order. When that fallthrough successor IS the next-laid-out block the
// trailing jump is a jump to +0 — correct, and pure waste.
//
// ★★★ ELIDING IT IS A CHANGE OF ENCODING, AND ENCODING IS TARGET VOCABULARY.
// Whether a machine HAS fallthrough semantics at all, and how the shorter
// form spells its opcode, is not something a transform may assume: it is
// declared in `.target.json`. So this predicate asks the SCHEMA whether the
// target declares the shorter form, and a target that declares none simply
// gets no elision — from this pass or any other — with no arm anywhere naming
// a CPU, a mnemonic or a byte.
//
// True iff BOTH variants exist on `opcode`:
//   * a LONG variant whose guard tuple has exactly `opCount` entries and
//     whose LAST entry is `BlockRef` (the form an un-elided branch selects),
//     and
//   * a SHORT variant whose guard tuple is that same tuple MINUS its last
//     entry, element-for-element, agreeing on EVERY OTHER ROUTING AXIS
//     (width, immediate range, sign, memory-destination). A variant that
//     differs on another axis is a different instruction that happens to be
//     shorter, never this one's fallthrough form, and dropping an operand to
//     reach it would silently encode something else.
//
// ⚠ THE PREDICATE HAS EXACTLY ONE OWNER ON PURPOSE. `lir_peephole` reads it
// to decide whether it MAY drop the trailing BlockRef, and `lir_verifier`
// reads the SAME function to decide whether a dropped one is LEGAL. Two
// copies of this rule that drifted apart would be a verifier that blesses an
// elision the encoder cannot spell — which is a wrong branch, not a build
// error.
[[nodiscard]] DSS_EXPORT bool
declaresFallthroughBranchForm(TargetSchema const& schema,
                              std::uint16_t       opcode,
                              std::size_t         opCount) noexcept;

// Copy EVERY module-level SIDE STRUCTURE from the source module into the
// destination builder, PRESERVING indices. Today that is two pools:
//
//   * the wide-literal pool (D-CSUBSET-BITFIELD-WIDE-UNIT) — every pass
//     that walks an input `Lir` and builds a fresh one copies
//     `LiteralIndex` OPERANDS verbatim (the index is an opaque module-
//     level reference), so the new builder's pool MUST hold the same
//     entries at the same indices or those operands dangle
//     (`LirLiteralPool::at` out-of-range at encode time). Before FC8 no
//     real value rode `LiteralIndex` to the encoder — strings/floats
//     never reached it — so this latent rebuild gap was invisible; the
//     `mov r64, imm64` carrier exposed it.
//   * the per-instruction register-constraint pool
//     (D-LIR-PER-INST-REG-CONSTRAINTS) — same by-index reference
//     discipline, referenced from `detail::LirInst::regConstraints`
//     instead of from an operand.
//
// The destination builder MUST be freshly constructed (both pools empty)
// so appending entries 0..N-1 in order reproduces the source indices
// exactly. Call once, right after `LirBuilder b{schema}`, before any
// `addInst`.
//
// ★★★ THIS IS ONE FUNCTION ON PURPOSE, AND THAT IS THE WHOLE MECHANISM.
// FOUR passes rebuild a module into a fresh builder (✔MEASURED
// 2026-08-14, `grep -rn "copyModuleSideStructures" src/` —
// `lir_2addr_legalize.cpp`, `lir_callconv.cpp`, `lir_rewrite.cpp` and
// `lir_wide_call_args.cpp`; note FOUR, not the three that two separate
// documents claimed. The grep is cited instead of line numbers because
// the line numbers this comment used to carry went stale the first time
// anyone edited those passes). A side structure whose carry-across is
// written out per-pass is a side structure that the FIFTH pass forgets,
// and forgetting it is not a crash — it is a dangling index or a vanished
// clobber set, i.e. a silent miscompile. So there is exactly one place to
// add a new side structure and zero per-pass copy code to keep in sync.
// ⚠ If you add a third pool, add it HERE and add its rules to
// `checkSideStructureIntegrity` + `verifyLirRebuild` (`lir_verifier.cpp`);
// do NOT add a second copy call anywhere.
DSS_EXPORT void
copyModuleSideStructures(Lir const& src, LirBuilder& dst);

// Carry the per-INSTRUCTION side data of one source instruction onto the
// instruction a pass has just appended to `dst`.
//
// ★★ THIS IS THE ONE CALL A REBUILDING PASS MUST MAKE PER INSTRUCTION,
// AND IT IS UNAVOIDABLE — a per-instruction field cannot survive a
// rebuild "by construction" the way `flags` does. `flags` survives
// because every pass already threads it through `addInst(op, res, ops,
// payload, flags)`; `regConstraints` rides the POD, and the rebuild
// re-CREATES the instruction, so only the pass knows which new
// instruction corresponds to which old one. Adding a 7th defaulted
// `addInst` parameter would not fix that: the default IS the drop.
//
// The two-argument overload targets `dst.lastInst()`, which is what a
// terminator needs — the shared `emitTerminator` dispatch returns a bool
// (it routes to one of five builder entrypoints), so its callers have no
// id for what it just emitted.
//
// ⚠⚠ PREFER THE FOUR-ARGUMENT OVERLOAD, AND PASS THE ID `addInst`
// RETURNED. "The rebuilt instruction" is NOT "the last instruction the
// pass appended" — ✔MEASURED 2026-08-14 across the four rebuild passes,
// TWO of them append AFTER it:
//
//   * `lir_rewrite` emits `frame_load` reloads BEFORE the instruction and
//     a `frame_store` AFTER it when the result is spilled, so the
//     correspondent is in the MIDDLE of a 1→(k+2) expansion;
//   * `lir_callconv`'s call arm emits the arg-setup moves before the
//     `call` and the return-value capture move AFTER it.
//
// ★ THE PRECISE CLAIM, because the loose one is false and was measured to
// be false: at the exact statement that FOLLOWS the emit, `lastInst()` is
// still correct in both passes, and substituting it there is green. What
// the id buys is that the binding survives the carry being MOVED — to the
// end of the loop body, past the spill store or past the capture move,
// which is where a reader would naturally put "one call per rebuilt
// instruction". Under that move the constraint set lands on a compiler-
// synthesized store/move: a WRONG instruction rather than a missing one,
// which the verifier's reference census cannot see (the count is still 1).
// Only the ordering pins in `tests/lir/test_lir_pass_util.cpp` catch it.
//
// The two-argument overload is for the terminator path, where
// `emitTerminator` returns no id and is by construction the last thing the
// arm appends. ⚠ Gate it on that emit having SUCCEEDED: a failed
// `emitTerminator` appends nothing, and then `lastInst()` is the previous
// instruction.
//
// Precondition: `copyModuleSideStructures` has already run on `dst`, so
// the source handle's pool index is valid in the destination.
DSS_EXPORT void
carryInstSideData(Lir const& src, LirInstId srcInst,
                  LirBuilder& dst, LirInstId dstInst);
DSS_EXPORT void
carryInstSideData(Lir const& src, LirInstId srcInst, LirBuilder& dst);

// D-AS-REWRITE-SPILL-SCRATCH-INCOMING-ARG-CLOBBER: resolve the physical
// INCOMING argument register a register-machine `arg` op's parameter arrives
// in, or classify why there is none. ONE formula, shared by the two consumers
// that ask the SAME question ("which physical register still holds a live
// incoming param"):
//   (a) the regalloc position-aware occupied-arg exclusion
//       (collectArgRegisterOccupied, lir_regalloc.cpp) — keeps a vreg HOME off
//       a still-live arg register;
//   (b) the rewriter spill-scratch forbid (rewriteOneFunc, lir_rewrite.cpp) —
//       keeps a transient reload SCRATCH off it.
// Coupling them here is what makes them provably agree (a drift would re-open
// the incoming-param clobber this anchor closes).
//
// NOT the arg MATERIALIZATION path: lir_callconv's `h.arg` emits the actual
// `mov home, argReg` and needs the register NAME plus the slot-aligned pool
// size for its cursor-desync assert — a different output, deliberately left
// untouched. The register-resident test here mirrors the collector's existing
// per-class-pool `payload < pool.size()` (byte-identical on every shipped cc).
// A hypothetical slot-aligned cc with UNEQUAL arg pool sizes is the only shape
// where this per-class test would diverge from `h.arg`'s `max(g,f)` test; none
// ships, and both consumers here treat the divergent slot as "no register to
// protect" (safe — a stack-passed param has no incoming register to clobber).
// DOMAIN NOTE (pre-existing, unreached — preserved byte-for-byte from the
// pre-hoist collector): a non-FPR result class resolves through `argGprs`, so a
// VR-class (vector) param would protect a GPR ordinal rather than its v-register.
// Unreached by the c (no vector params; AAPCS64 F128 aliases the FPR
// d-view). If a vector-param ABI ever ships, add its arg-vector pool arm here.
enum class IncomingArgRegKind : std::uint8_t {
    Register,          // arrives in a physical register (ordinal below)
    StackPassed,       // payload past the arg-register pool → caller's stack
    UnresolvableName,  // the cc names a register absent from the target table
};
struct IncomingArgReg {
    IncomingArgRegKind kind{IncomingArgRegKind::StackPassed};
    std::uint16_t      ordinal{0};  // meaningful iff kind == Register
};

[[nodiscard]] DSS_EXPORT IncomingArgReg
incomingArgRegister(TargetSchema const&            schema,
                    TargetCallingConvention const& cc,
                    LirRegClass                    resultClass,
                    std::uint32_t                  payload);

// ── THE IDENTITY-CLASS-MOVE QUESTION, WITH ONE OWNER ────────────────────
// D-LIR-PEEPHOLE-CALLCONV-IDENTITY-COPY-CLAIM-HAS-NO-INSTRUMENT.
//
// "Is this instruction the declared register-to-register MOVE for its
// result's register class, copying a physical register into ITSELF?" is
// asked by TWO consumers that must never disagree:
//
//   (a) `lir_peephole`'s RULE R1, which DELETES the instruction; and
//   (b) `censusIdentityClassMoves` below, which COUNTS the population and
//       R1's refusals so a stage-to-stage claim about it can be re-measured
//       instead of re-quoted.
//
// ★★★ THE CENSUS EXISTS BECAUSE THE CLAIM IT CHECKS WAS ONCE UNFALSIFIABLE.
// `lir_peephole.hpp` asserted "`materializeCallingConvention` mints exactly
// ZERO identity copies" on the evidence "5575 at post-rewrite and 5575 at
// post-callconv". Those are the only two LIR dump stages that existed, and
// THREE passes sit between them (`legalizeTwoAddress`, `runLirPeephole`,
// `materializeCallingConvention`) — one of which deletes members of exactly
// this population. An equal count across that span is a NET, and a net of
// zero is not a per-pass zero. A second lane later measured a non-zero
// residue at post-callconv and could not tell which pass minted it, which is
// the whole cost of a claim whose instrument answers an adjacent question.
//
// ⚠ AND A MNEMONIC SWEEP OF THE TEXT DUMP CANNOT ANSWER IT EITHER. The dump
// prints `opcodeInfo(..)->mnemonic`, which carries no width and no register
// class, while R1's verdict turns on BOTH (a class move NARROWER than the
// register it names is a truncation with a zero-extending side effect, not a
// no-op). Counting the string "mov" therefore over-counts the population it
// is trying to attribute. This classifier is the schema-driven answer, and
// the pass that acts on it and the census that reports it are the same code.
enum class IdentityClassMoveVerdict : std::uint8_t {
    // Not a member of the population: not the class MOVE opcode, or not
    // physical-register-into-itself, or a terminator (never a copy, and
    // deleting one leaves the block unterminated).
    NotIdentityClassMove = 0,
    // In the population, and R1 deletes it.
    Deletable,
    // In the population; R1 refuses because the opcode declares a side
    // effect or an implicit register read/clobber — an observable this rule
    // cannot reason about from the operands.
    RefusedSideEffects,
    // In the population; R1 refuses because the target declares no width for
    // the named register (a schema the pass will not guess about).
    RefusedUndeclaredRegisterWidth,
    // In the population; R1 refuses because the copy is NARROWER than the
    // register it names, so it writes bits it did not read.
    RefusedNarrowerThanRegister,
    // In the population; R1 refuses because the instruction is the only
    // namer of a per-instruction register-constraint pool entry, and
    // deleting it would orphan the entry (`L_SideStructureReferenceLost`).
    RefusedNamesConstraintPoolEntry,
};

// Per-pass cache of "which opcode is this register class's declared
// register-to-register MOVE". Lazily resolved, and ABSENCE IS A VALUE: a
// class with no declared `move` resolves to an empty inner optional and is
// re-asked never. Held by the caller so a whole-module walk resolves each
// class once.
//
// ⚠ ABSENCE IS SILENT HERE, AND THAT IS THE CORRECT ARM FOR BOTH CONSUMERS.
// A class with no declared `move` means no copy is recognized for it, so
// nothing is deleted and nothing is counted — the fail-safe.
// `lir_2addr_legalize` reports the same absence as an Error because it is
// trying to EMIT the instruction and cannot; a cleanup that finds nothing to
// clean, and a census that counts nothing, have nothing to report.
class DSS_EXPORT ClassMoveOpcodeCache {
  public:
    [[nodiscard]] std::optional<std::uint16_t>
    resolve(TargetSchema const& schema, LirRegClass cls);

  private:
    // One slot per `LirRegClass` envelope value (None, GPR, FPR, VR, Flags).
    std::array<std::optional<std::optional<std::uint16_t>>, 5> byClass_{};
};

[[nodiscard]] DSS_EXPORT IdentityClassMoveVerdict
classifyIdentityClassMove(Lir const& lir, LirInstId inst,
                          TargetSchema const&    schema,
                          ClassMoveOpcodeCache&  cache);

// Module-wide census of the identity-class-move population, split by R1's
// verdict. `population` is the sum of the five verdict buckets, so an
// attribution never silently loses a member.
//
// ★ READ IT AT A STAGE BOUNDARY AND SUBTRACT. `dumpLirFuncs` prints this on
// every STAGE header line, so `post-rewrite → post-legalize → post-peephole
// → post-callconv` attributes each pass's contribution to each bucket
// SEPARATELY. That is the instrument the "callconv mints zero" claim never
// had.
struct IdentityClassMoveCensus {
    std::size_t population                      = 0;
    std::size_t deletable                       = 0;
    std::size_t refusedSideEffects              = 0;
    std::size_t refusedUndeclaredRegisterWidth  = 0;
    std::size_t refusedNarrowerThanRegister     = 0;
    std::size_t refusedNamesConstraintPoolEntry = 0;
    // ★ THE SUPERSET R1 IS NOT ALLOWED TO BE. Every instruction whose SOLE
    // operand is a register equal to its result register, whatever its opcode
    // — `zext`, `sext`, `trunc`, `not`, `neg`, `shl`, the class move, and the
    // rest. A rule shaped "result == its only operand ⇒ delete" would take
    // this whole number; R1 takes only the `deletable` slice of it, and the
    // MARGIN between them is the correctness argument for asking the schema
    // which opcode is the class copy. `population` is always ≤ this.
    //
    // ⚠ IT IS COUNTED HERE FOR THE SAME REASON THE REST IS: the docblock's
    // statement of that margin was a pair of hand-carried numbers that went
    // stale the moment the coalescer changed the population, and nothing
    // re-derived them.
    std::size_t selfReferentialSingleOperand    = 0;
};

[[nodiscard]] DSS_EXPORT IdentityClassMoveCensus
censusIdentityClassMoves(Lir const& lir, TargetSchema const& schema);

} // namespace dss::lir_pass_util
