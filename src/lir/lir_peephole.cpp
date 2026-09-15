#include "lir/lir_peephole.hpp"

#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"
#include "lir/lir_reg.hpp"
#include "lir/lir_rewrite.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace dss {

namespace {

// ── RULE R1 — REDUNDANT-COPY ELIMINATION ────────────────────────────────
//
// True iff `inst` is the declared class MOVE copying one physical register
// into ITSELF at that register's full declared width, carries no
// per-instruction side data, and has no declared side effect. See the
// header for why each clause is load-bearing; the short version is that
// on the shipped x86_64 target THREE opcodes (`mov`, `trunc`, `zext`)
// disassemble as `mov` and only the first is a no-op when its source and
// destination coincide.
//
// ★★★ THE PREDICATE ITSELF LIVES IN `lir_pass_util`, AND THAT IS THE POINT.
// It has a second consumer — `censusIdentityClassMoves`, the stage-boundary
// instrument that attributes this population per PASS
// (D-LIR-PEEPHOLE-CALLCONV-IDENTITY-COPY-CLAIM-HAS-NO-INSTRUMENT). A census
// that re-implemented the rule would measure a population the rule does not
// act on, which is exactly the failure that left the header's callconv claim
// unfalsifiable for a cycle. One owner; the census reports the verdict this
// function acts on, by construction.
[[nodiscard]] bool
isRedundantCopy(Lir const& src, LirInstId inst, TargetSchema const& schema,
                lir_pass_util::ClassMoveOpcodeCache& movCache) {
    return lir_pass_util::classifyIdentityClassMove(src, inst, schema,
                                                    movCache)
           == lir_pass_util::IdentityClassMoveVerdict::Deletable;
}

// ── WHY THE WIDTH CLAUSE INSIDE THAT PREDICATE IS NOT VACUOUS ───────────
//
// ⓘ A REGISTER WIDER THAN 64 BITS IS NOW SAYABLE, AND THIS RULE NEEDED
// NO EDIT TO GAIN IT. `lirInstWidthBits` was a three-flag field over
// {8,16,32,64} until 2026-08-26, so 128 was UNSAYABLE and a full-width copy
// of an x86-64 `xmm` (16 bytes) could never satisfy this equality -- a
// MEASURED 121 of 6079 corpus identity copies were skipped for that reason
// alone. That was an LIR expressiveness defect, not a peephole residue, so
// it was fixed at the substrate (`kLirInstFlagWidth128`) rather than
// special-cased here.
//
// ★★ THE 121 COPIES ARE NOW DELETED, AND NOT BY THIS RULE — THE ALLOCATOR
// STOPPED EMITTING THEM:
// D-LIR-COPY-COALESCING-ASKS-THE-REGISTERS-WIDTH-NOT-THE-VALUES.
// ⚠ THE PARAGRAPH THAT STOOD HERE PREDICTED THE WRONG FIX
// AND IS WORTH KEEPING AS A CORRECTION: it said the residue was "an honest
// statement about the LOWERING", that nothing stamps 128, and that "the day
// one does, this comparison starts matching them with no change here". A
// class move NEVER states 128 and never should — `movaps` and `fmov`
// declare their register-to-register variants with no width guard, so a
// stated width would be vocabulary with no purpose (and on arm64 there IS
// no 128-bit `fmov`: the 128-bit SIMD move is ORR, different bytes). The
// residue was never waiting on the lowering; it was this comparison asking
// about the REGISTER when the answer depends on the VALUE, and the value is
// knowable only while the ends are still virtual. ✔MEASURED corpus-wide:
// 185 identity `movaps` (x86_64) and 150 identity `fmov` (arm64) reached
// the emitted stream, every one of them refused HERE.
//
// ⇒ THIS TEST STAYS EXACTLY AS IT IS, and it is not vacuous: the copies
// that still reach this pass are the ones the linear scan made identity by
// COINCIDENCE, carrying no proof about the value, plus the author-pinned
// physical ones. For those, "does it write the whole register" is the
// only sound question, and it is the right one.
//
// *** AND THE GUARD MUST STAY EVEN THEN, because the lowering is not the
// only producer of a class move: the inline-asm path builds instructions
// from a dialect template and can mint a NARROW one (`movl %eax,%eax`
// writes 32 bits and zeroes the upper half of a 64-bit register). Inferring
// "writes the whole register" from the ABSENCE of a width guard on the
// variant would admit exactly that instruction, and is unsound for the same
// reason on any target whose only class-move form is a narrowing one.
//
// ✔MEASURED 2026-09-02 over `examples/c/**` at `--config=release`, by
// `censusIdentityClassMoves` reading R1's OWN verdict at four stage
// boundaries: this clause is what the entire post-peephole residue consists
// of — 37 of 37 (arm64) and 39 of 39 (x86_64) surviving identity class moves
// are refused by the width test and by nothing else, and ZERO deletable ones
// survive to the encoder on either target. See the header's ATTRIBUTION
// section (D-LIR-PEEPHOLE-CALLCONV-IDENTITY-COPY-CLAIM-HAS-NO-INSTRUMENT).

// ── RULE R2 — FALLTHROUGH-BRANCH ELISION (D-OPT-JCC-FALLTHROUGH) ────────
//
// True iff `inst` terminates `blk` with a branch whose LAST BlockRef operand
// names `nextBlock` — the block laid out immediately after `blk` — and the
// target declares the shorter, fallthrough-form encoding for it.
//
// ★ THE OPERAND LIST MUST ALREADY AGREE WITH THE SUCCESSOR LIST EXACTLY:
// same count, same blocks, same order. That is not a formality, it is what
// makes "drop the LAST OPERAND" mean "stop materializing the LAST SUCCESSOR"
// instead of "stop materializing whatever happens to sit at the end of the
// list". It is also `lir_verifier`'s Rule 1b stated as a precondition, so an
// instruction that already failed that rule is left exactly as it was for the
// verifier to report rather than quietly rewritten into a second shape.
//
// ⚠ AND IT MAKES THE RULE IDEMPOTENT FOR FREE: an ALREADY-elided branch has
// one fewer operand than successors, fails the count test, and is not elided
// twice.
[[nodiscard]] bool
isElidableFallthroughBranch(Lir const& src, LirBlockId blk, LirInstId inst,
                            std::optional<LirBlockId> nextBlock,
                            TargetSchema const& schema) {
    // The LAST block of a function has no next-laid-out block, so its
    // fallthrough edge has nowhere to fall to and must stay materialized.
    if (!nextBlock.has_value()) return false;
    auto const* info = schema.opcodeInfo(src.instOpcode(inst));
    if (info == nullptr || !info->isTerminator()) return false;

    auto const ops   = src.instOperands(inst);
    auto const succs = src.blockSuccessors(blk);
    if (ops.empty() || ops.size() != succs.size()) return false;
    for (std::size_t k = 0; k < ops.size(); ++k) {
        if (ops[k].kind != LirOperandKind::BlockRef) return false;
        if (ops[k].blockSlot != succs[k].v) return false;
    }
    if (succs.back().v != nextBlock->v) return false;

    // ★★★ AND ONLY IF THE TARGET SAYS SO. One owner for that question, shared
    // with the verifier arm that has to bless the result.
    return lir_pass_util::declaresFallthroughBranchForm(
        schema, src.instOpcode(inst), ops.size());
}

// Rewrite ONE function into `b`, dropping the instructions R1 proves
// redundant.
//
// Two failure channels, byte-for-byte the split `lir_2addr_legalize`
// documents: the HARD channel is this function's `bool` return (the
// destination builder has been left with an unterminated open block and
// must not be touched again); there is no SOFT channel, because a
// peephole that cannot prove a rewrite simply does not perform it.
[[nodiscard]] bool
peepholeOneFunc(Lir const& src, LirFuncId srcFn, TargetSchema const& schema,
                lir_pass_util::ClassMoveOpcodeCache& movCache,
                LirBuilder& b, std::size_t& removed,
                std::size_t& elided, DiagnosticReporter& reporter) {
    (void)b.addFunction(src.funcSymbol(srcFn));

    std::unordered_map<std::uint32_t, LirBlockId> srcToDst;
    std::uint32_t const blockCount = src.funcBlockCount(srcFn);
    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        LirBlockId const dst = b.createBlock();
        srcToDst[src.funcBlockAt(srcFn, bi).v] = dst;
    }
    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        LirBlockId const srcBlk = src.funcBlockAt(srcFn, bi);
        // R2's layout question, asked of the module being REBUILT. The
        // rebuild recreates blocks in source order (the loop above, and the
        // `beginBlock` order here), so "next in the source" IS "next in the
        // output" — the elision cannot be invalidated by this pass's own
        // rewrite. Nothing weaker than that identity would do: an elision is
        // a statement about the FINAL layout.
        std::optional<LirBlockId> const srcNext =
            (bi + 1 < blockCount)
                ? std::optional<LirBlockId>{src.funcBlockAt(srcFn, bi + 1)}
                : std::nullopt;
        b.beginBlock(srcToDst[srcBlk.v]);

        std::uint32_t const instCount = src.blockInstCount(srcBlk);
        for (std::uint32_t ii = 0; ii < instCount; ++ii) {
            LirInstId const inst = src.blockInstAt(srcBlk, ii);
            if (isRedundantCopy(src, inst, schema, movCache)) {
                ++removed;
                continue;
            }
            auto const  opcode = src.instOpcode(inst);
            auto const* info   = schema.opcodeInfo(opcode);
            auto const  ops    = src.instOperands(inst);

            std::vector<LirOperand> newOps;
            newOps.reserve(ops.size());
            for (auto const& op : ops) {
                newOps.push_back(lir_pass_util::remapBlockRef(op, srcToDst));
            }

            if (info != nullptr && info->isTerminator()) {
                std::vector<LirBlockId> succs;
                auto const srcSuccs = src.blockSuccessors(srcBlk);
                succs.reserve(srcSuccs.size());
                for (auto const& s : srcSuccs) {
                    auto it = srcToDst.find(s.v);
                    if (it != srcToDst.end()) succs.push_back(it->second);
                }
                // ★ R2 — and note what is NOT touched: `succs` is passed on
                // whole. The CFG edge survives; only the ENCODER's copy of it
                // goes away, because the layout already places it.
                if (isElidableFallthroughBranch(src, srcBlk, inst, srcNext,
                                                schema)) {
                    newOps.pop_back();
                    ++elided;
                }
                if (!lir_pass_util::emitTerminator(
                        b, opcode, info, succs, newOps,
                        src.instPayload(inst), src.instFlags(inst),
                        srcToDst, "lir-peephole", reporter)) {
                    return false;
                }
                lir_pass_util::carryInstSideData(src, inst, b);
            } else {
                LirInstId const dstInst =
                    b.addInst(opcode, src.instResult(inst), newOps,
                              src.instPayload(inst), src.instFlags(inst));
                lir_pass_util::carryInstSideData(src, inst, b, dstInst);
            }
        }
    }
    return true;
}

} // namespace

LirPeepholeResult
runLirPeephole(Lir const&          src,
               TargetSchema const& schema,
               DiagnosticReporter& reporter) {
    LirPeepholeResult result;
    if (src.moduleFuncCount() == 0) {
        // Empty module (a declaration-only / all-preprocessed-out TU):
        // nothing to clean. 0 == 0 with `rebuilt` vacuously true, so
        // `ok()` reports SUCCESS — the same arm
        // `legalizeTwoAddress` takes (D-CSUBSET-TESTTU-SILENT-EXIT1).
        result.rebuilt = true;
        return result;
    }
    result.expectedFuncCount = src.moduleFuncCount();

    lir_pass_util::ClassMoveOpcodeCache movCache{};
    LirBuilder b{schema};
    // The wide-literal pool and the per-instruction register-constraint
    // pool, index-preserving, in ONE call — see `lir_pass_util.hpp`.
    lir_pass_util::copyModuleSideStructures(src, b);

    std::size_t const funcCount = src.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < funcCount; ++fi) {
        if (peepholeOneFunc(src, src.funcAt(fi), schema, movCache, b,
                            result.redundantCopiesRemoved,
                            result.fallthroughBranchesElided, reporter)) {
            continue;
        }
        // Mid-failure the builder holds a half-open function whose current
        // block never got its terminator; `finish()` would be a process
        // kill inside `closeFunction_`. Return WITHOUT touching `b` again
        // — `result.lir` stays empty and `ok()` is false on both clauses.
        result.rebuilt = false;
        return result;
    }

    result.lir     = std::move(b).finish();
    result.rebuilt = true;
    // ★ THE STAGE BOUNDARY THIS PASS DID NOT HAVE
    // (D-LIR-PEEPHOLE-CALLCONV-IDENTITY-COPY-CLAIM-HAS-NO-INSTRUMENT).
    // Env-gated and zero-cost when unset, like every other stage dump. Without
    // it the nearest boundaries either side of this pass were `post-rewrite`
    // and `post-callconv`, so ANY per-pass claim about the population R1 acts
    // on could only be stated as a net across three passes — which is how the
    // header's callconv claim came to rest on evidence that could not test it.
    dumpLirFuncs(result.lir, schema, "post-peephole");
    return result;
}

} // namespace dss
