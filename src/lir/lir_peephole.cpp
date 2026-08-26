#include "lir/lir_peephole.hpp"

#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"
#include "lir/lir_reg.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace dss {

namespace {

// Per-pass cache of "which opcode is this register class's declared
// register-to-register MOVE". Same lazily-resolved shape
// `lir_2addr_legalize`'s `PassState` uses to resolve the copy it
// SYNTHESIZES — deliberately, because the two passes must agree on which
// opcode a copy IS: legalize mints one, this pass may delete one, and a
// disagreement would let the minted copy survive as an unrecognized
// opcode or (far worse) let some OTHER opcode be mistaken for a copy.
//
// ⚠ ABSENCE IS SILENT HERE, AND THAT IS THE CORRECT ARM. A class with no
// declared `move` means this pass recognizes no copy for it and deletes
// nothing — the fail-safe. `lir_2addr_legalize` reports the same absence
// as an Error because it is trying to EMIT the instruction and cannot;
// a cleanup that finds nothing to clean has nothing to report.
struct PassState {
    TargetSchema const& schema;
    std::array<std::optional<std::optional<std::uint16_t>>, 5> movByClass{};

    [[nodiscard]] std::optional<std::uint16_t> resolveMov(LirRegClass cls) {
        auto const c = static_cast<std::size_t>(cls);
        if (c >= movByClass.size()) return std::nullopt;
        if (!movByClass[c].has_value()) {
            movByClass[c] = schema.regClassOpOpcode(
                static_cast<TargetRegClass>(c), RegClassOp::Move);
        }
        return *movByClass[c];
    }
};

// ── RULE R1 — REDUNDANT-COPY ELIMINATION ────────────────────────────────
//
// True iff `inst` is the declared class MOVE copying one physical register
// into ITSELF at that register's full declared width, carries no
// per-instruction side data, and has no declared side effect. See the
// header for why each clause is load-bearing; the short version is that
// on the shipped x86_64 target THREE opcodes (`mov`, `trunc`, `zext`)
// disassemble as `mov` and only the first is a no-op when its source and
// destination coincide.
[[nodiscard]] bool
isRedundantCopy(Lir const& src, LirInstId inst, TargetSchema const& schema,
                PassState& state) {
    auto const* info = schema.opcodeInfo(src.instOpcode(inst));
    if (info == nullptr) return false;
    // A terminator is never a copy, and deleting one would leave the block
    // unterminated — the builder's abort, not a diagnostic.
    if (info->isTerminator()) return false;
    // A declared side effect (or a declared implicit register read/clobber)
    // is an observable this pass cannot reason about from the operands.
    if (info->hasSideEffects) return false;
    if (info->implicitRegisters.has_value()) return false;

    LirReg const result = src.instResult(inst);
    if (!result.valid() || result.isPhysical == 0) return false;

    // ★ THE OPCODE IDENTITY TEST. Not a mnemonic, not an encoded byte —
    // the handle the SCHEMA names as this class's copy.
    auto const movOpcode = state.resolveMov(result.regClass());
    if (!movOpcode.has_value()) return false;
    if (src.instOpcode(inst) != *movOpcode) return false;

    // Exactly one operand, a physical register identical to the result.
    auto const ops = src.instOperands(inst);
    if (ops.size() != 1) return false;
    if (ops[0].kind != LirOperandKind::Reg) return false;
    if (ops[0].reg.isPhysical == 0) return false;
    if (!(ops[0].reg == result)) return false;

    // ★ THE WIDTH TEST — the second, independent guard on the partial-
    // register-write hazard. A copy NARROWER than the register it names
    // writes bits it did not read (x86-64's 32-bit GPR forms zero the
    // upper half), so it is NOT a no-op even when source and destination
    // are the same register.
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
    // The 121 copies are still not deleted, and that is now an honest
    // statement about the LOWERING rather than about this rule: no shipped
    // lowering STAMPS 128, because the FPR class moves on both shipped targets
    // (`movaps`, `fmov`) declare encoding variants with no width guard and so
    // have no reason to state a width. The day one does, this comparison starts
    // matching them with no change here.
    //
    // *** AND THE GUARD MUST STAY EVEN THEN, because the lowering is not the
    // only producer of a class move: the inline-asm path builds instructions
    // from a dialect template and can mint a NARROW one (`movl %eax,%eax`
    // writes 32 bits and zeroes the upper half of a 64-bit register). Inferring
    // "writes the whole register" from the ABSENCE of a width guard on the
    // variant would admit exactly that instruction, and is unsound for the same
    // reason on any target whose only class-move form is a narrowing one.
    auto const* regInfo = schema.registerInfo(
        static_cast<std::uint16_t>(result.id));
    if (regInfo == nullptr || regInfo->widthBytes == 0) return false;
    auto const regWidthBits =
        static_cast<std::uint32_t>(regInfo->widthBytes) * 8u;
    if (static_cast<std::uint32_t>(lirInstWidthBits(src.instFlags(inst)))
        != regWidthBits) {
        return false;
    }

    // ★ NEVER DELETE THE ONLY NAMER OF A SIDE-STRUCTURE ENTRY. The
    // per-instruction register-constraint pool is referenced by index from
    // the instruction stream and `verifyLirRebuild` counts the references
    // on both sides; orphaning an entry is `L_SideStructureReferenceLost`.
    // Keeping a redundant copy costs one instruction — the fail-safe arm.
    if (src.instRegConstraintHandle(inst) != kLirNoRegConstraints) {
        return false;
    }
    return true;
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
                PassState& state, LirBuilder& b, std::size_t& removed,
                DiagnosticReporter& reporter) {
    (void)b.addFunction(src.funcSymbol(srcFn));

    std::unordered_map<std::uint32_t, LirBlockId> srcToDst;
    std::uint32_t const blockCount = src.funcBlockCount(srcFn);
    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        LirBlockId const dst = b.createBlock();
        srcToDst[src.funcBlockAt(srcFn, bi).v] = dst;
    }
    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        LirBlockId const srcBlk = src.funcBlockAt(srcFn, bi);
        b.beginBlock(srcToDst[srcBlk.v]);

        std::uint32_t const instCount = src.blockInstCount(srcBlk);
        for (std::uint32_t ii = 0; ii < instCount; ++ii) {
            LirInstId const inst = src.blockInstAt(srcBlk, ii);
            if (isRedundantCopy(src, inst, schema, state)) {
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

    PassState state{schema};
    LirBuilder b{schema};
    // The wide-literal pool and the per-instruction register-constraint
    // pool, index-preserving, in ONE call — see `lir_pass_util.hpp`.
    lir_pass_util::copyModuleSideStructures(src, b);

    std::size_t const funcCount = src.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < funcCount; ++fi) {
        if (peepholeOneFunc(src, src.funcAt(fi), schema, state, b,
                            result.redundantCopiesRemoved, reporter)) {
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
    return result;
}

} // namespace dss
