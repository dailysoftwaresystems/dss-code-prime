#include "lir/lir_verifier.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"  // regClassForCoreType

#include <format>
#include <string>
#include <utility>

namespace dss {

namespace {

void report(DiagnosticReporter& reporter, std::string actual) {
    ParseDiagnostic d;
    d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
    d.severity = DiagnosticSeverity::Error;
    d.actual   = std::move(actual);
    reporter.report(std::move(d));
}

// Resolve a mnemonic-by-name once. The verifier needs `load`/`store`/
// `lea`'s opcode numbers to find the memory-bearing insts; cycle 3e
// only checks register-machine-shaped targets, so a missing mnemonic
// returns nullopt and the rule silently skips (a non-register-machine
// target legitimately omits these).
struct MemOpcodeIds {
    std::optional<std::uint16_t> load;
    std::optional<std::uint16_t> store;
    std::optional<std::uint16_t> lea;
};

[[nodiscard]] MemOpcodeIds resolveMemOpcodes(TargetSchema const& sch) {
    return {
        sch.opcodeByMnemonic("load"),
        sch.opcodeByMnemonic("store"),
        sch.opcodeByMnemonic("lea"),
    };
}

// Rule 1: every Load/Store/Lea inst's operand list must END with a
// MemBase followed by a MemOffset operand. Cycle 3c/3d emit:
//   Load:  [Reg base, MemBase, MemOffset]
//   Store: [Reg value, Reg base, MemBase, MemOffset]
//   Lea:   [Reg base, MemBase, MemOffset] (3-operand) or
//          [Reg base, Reg index, MemBase, MemOffset] (4-operand)
// The shared invariant: the last two operands are MemBase, MemOffset.
void checkMemOperandPairing(Lir const& lir, TargetSchema const& sch,
                            DiagnosticReporter& reporter) {
    auto const mem = resolveMemOpcodes(sch);
    std::size_t const fnCount = lir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < fnCount; ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        std::uint32_t const blockCount = lir.funcBlockCount(fn);
        for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
            LirBlockId const bb = lir.funcBlockAt(fn, bi);
            std::uint32_t const n = lir.blockInstCount(bb);
            for (std::uint32_t i = 0; i < n; ++i) {
                LirInstId const inst = lir.blockInstAt(bb, i);
                std::uint16_t const op = lir.instOpcode(inst);
                bool const isMem = (mem.load.has_value()  && op == *mem.load)
                                || (mem.store.has_value() && op == *mem.store)
                                || (mem.lea.has_value()   && op == *mem.lea);
                if (!isMem) continue;
                auto const ops = lir.instOperands(inst);
                if (ops.size() < 2
                    || ops[ops.size() - 2].kind != LirOperandKind::MemBase
                    || ops[ops.size() - 1].kind != LirOperandKind::MemOffset) {
                    report(reporter, std::format(
                        "LirVerifier: memory inst {} has malformed addressing-"
                        "mode operand pair; expected last two ops to be "
                        "MemBase then MemOffset",
                        inst.v));
                }
            }
        }
    }
}

// Rule 2: every Store inst's value-operand register class must match
// the regClassForCoreType of the MIR pointee type. The lowerer routes
// MIR Store → LIR `store`; the verifier cross-references both modules.
// Cycle 3e's Mir-vs-Lir alignment is implicit (LIR funcs/blocks/insts
// are emitted in MIR order); we walk both in parallel.
void checkStoreRegClassMatchesMirType(Lir const& lir, Mir const& mir,
                                      TypeInterner const& interner,
                                      TargetSchema const& sch,
                                      DiagnosticReporter& reporter) {
    auto const storeOp = sch.opcodeByMnemonic("store");
    if (!storeOp.has_value()) return;  // non-register-machine
    if (lir.moduleFuncCount() != mir.moduleFuncCount()) return;  // alignment broken
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const lfn = lir.funcAt(fi);
        MirFuncId const mfn = mir.funcAt(fi);
        if (lir.funcBlockCount(lfn) != mir.funcBlockCount(mfn)) continue;
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(lfn); ++bi) {
            LirBlockId const lbb = lir.funcBlockAt(lfn, bi);
            MirBlockId const mbb = mir.funcBlockAt(mfn, bi);
            std::uint32_t const ln = lir.blockInstCount(lbb);
            std::uint32_t const mn = mir.blockInstCount(mbb);
            // For each MIR Store, find the corresponding LIR `store`.
            // Walk LIR ops once per MIR Store; the lowerer emits in
            // sequence so a forward scan finds the next match.
            std::uint32_t li = 0;
            for (std::uint32_t mi = 0; mi < mn; ++mi) {
                MirInstId const minst = mir.blockInstAt(mbb, mi);
                if (mir.instOpcode(minst) != MirOpcode::Store) continue;
                while (li < ln && lir.instOpcode(lir.blockInstAt(lbb, li)) != *storeOp) ++li;
                if (li >= ln) break;
                LirInstId const linst = lir.blockInstAt(lbb, li++);
                auto const lops = lir.instOperands(linst);
                if (lops.empty() || lops[0].kind != LirOperandKind::Reg) continue;
                // The MIR Store's value type drives the expected class.
                auto const mops = mir.instOperands(minst);
                if (mops.size() < 1) continue;
                TypeId const valueTy = mir.instType(mops[0]);
                if (!valueTy.valid()) continue;
                LirRegClass const expected =
                    static_cast<LirRegClass>(regClassForCoreType(interner.kind(valueTy)));
                LirRegClass const actual = lops[0].reg.regClass();
                if (expected != actual && actual != LirRegClass::None) {
                    report(reporter, std::format(
                        "LirVerifier: Store value-operand reg class {} does "
                        "not match MIR pointee-type regClass {} (LIR inst {})",
                        static_cast<int>(actual),
                        static_cast<int>(expected), linst.v));
                }
            }
        }
    }
}

// Rule 3: every LIR inst that produces a result and whose result reg
// is class-typed must match the regClassForCoreType of the
// corresponding MIR inst's type. Walks both modules in parallel; for
// each MIR value-producing inst, find the next LIR inst whose result
// is a valid vreg and cross-check. The check skips opcodes whose
// result class is target-defined rather than type-derived (Alloca's
// result is always a pointer / GPR regardless of the Mir typeId
// detail; future cycles add address-of opcodes the same way).
void checkVregClassMatchesMirType(Lir const& lir, Mir const& mir,
                                  TypeInterner const& interner,
                                  DiagnosticReporter& reporter) {
    if (lir.moduleFuncCount() != mir.moduleFuncCount()) return;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const lfn = lir.funcAt(fi);
        MirFuncId const mfn = mir.funcAt(fi);
        if (lir.funcBlockCount(lfn) != mir.funcBlockCount(mfn)) continue;
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(lfn); ++bi) {
            LirBlockId const lbb = lir.funcBlockAt(lfn, bi);
            MirBlockId const mbb = mir.funcBlockAt(mfn, bi);
            std::uint32_t const ln = lir.blockInstCount(lbb);
            std::uint32_t const mn = mir.blockInstCount(mbb);
            std::uint32_t li = 0;
            for (std::uint32_t mi = 0; mi < mn; ++mi) {
                MirInstId const minst = mir.blockInstAt(mbb, mi);
                TypeId const mty = mir.instType(minst);
                // Skip MIR insts that don't produce values or have an
                // invalid type (terminators, side-effect-only ops).
                if (!mty.valid()) continue;
                if (interner.kind(mty) == TypeKind::Void) continue;
                if (mir.instOpcode(minst) == MirOpcode::Phi) continue;
                if (mir.instOpcode(minst) == MirOpcode::Alloca) continue;
                if (mir.instOpcode(minst) == MirOpcode::GlobalAddr) continue;
                // Advance LIR cursor to the next value-producing inst.
                while (li < ln) {
                    LirInstId const cand = lir.blockInstAt(lbb, li);
                    if (lir.instResult(cand).valid()) break;
                    ++li;
                }
                if (li >= ln) break;
                LirInstId const linst = lir.blockInstAt(lbb, li++);
                LirRegClass const expected =
                    static_cast<LirRegClass>(regClassForCoreType(interner.kind(mty)));
                LirRegClass const actual = lir.instResult(linst).regClass();
                if (expected != actual && actual != LirRegClass::None) {
                    report(reporter, std::format(
                        "LirVerifier: LIR inst {} produced reg class {} but "
                        "the corresponding MIR inst %{} has type kind that "
                        "expects class {}",
                        linst.v, static_cast<int>(actual), minst.v,
                        static_cast<int>(expected)));
                }
            }
        }
    }
}

} // namespace

LirVerifyResult verifyLir(Lir const&          lir,
                          Mir const&          mir,
                          TypeInterner const& interner,
                          DiagnosticReporter& reporter) {
    auto schema = TargetSchema::loadShipped("x86_64");
    if (!schema) {
        // No target schema loadable — cycle 3e ships only the x86_64
        // path; future targets will pass the schema in directly.
        return {true};
    }
    auto const baseline = reporter.errorCount();
    checkMemOperandPairing(lir, **schema, reporter);
    checkStoreRegClassMatchesMirType(lir, mir, interner, **schema, reporter);
    checkVregClassMatchesMirType(lir, mir, interner, reporter);
    return {reporter.errorCount() == baseline};
}

} // namespace dss
