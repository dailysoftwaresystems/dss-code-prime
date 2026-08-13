#include "lir/lir_verifier.hpp"

#include "core/types/parse_diagnostic.hpp"

#include <format>
#include <string>
#include <utility>

namespace dss {

namespace {

void report(DiagnosticReporter& reporter, std::string actual,
            DiagnosticCode code = DiagnosticCode::L_UnsupportedLoweringForOpcode) {
    ParseDiagnostic d;
    d.code     = code;
    d.severity = DiagnosticSeverity::Error;
    d.actual   = std::move(actual);
    reporter.report(std::move(d));
}

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
// MemBase followed by a MemOffset operand. Walks LIR only; no MIR
// cross-reference needed. Operates per-block; safe across function
// boundaries.
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
                        inst.v),
                        DiagnosticCode::L_MemOperandMalformed);
                }
            }
        }
    }
}

// Rule 1b (D-LIR-TEXT-CONDBR-BLOCKREF-OPERANDS-DROPPED): a terminator writes
// its CFG edges down TWICE and nothing cross-checked the two channels.
//
// ★★★ WHY TWO CHANNELS EXIST AT ALL, because "then delete one" is the obvious
// wrong answer. The block's recorded successor list (`blockSuccessors`) is the
// CFG: liveness, `simplifyCfg`, the `.dsslir` terminator dispatch and every
// walk read it, and it must stay a first-class edge set that survives a pass
// rebuilding the instruction. The BlockRef OPERANDS are the ENCODER's input:
// `x86_variable.cpp` takes a branch's displacement from `srcOp.blockSlot`, and
// a `cond-br` additionally uses operand[1] to emit the trailing unconditional
// jump, so the operand list is what turns into bytes. Both are load-bearing;
// what was missing is the rule that they say the SAME THING.
//
// ★★ AND THE RULE ASSERTS PRESENCE, NOT MERELY AGREEMENT. The defect that minted
// it was a SILENT DROP — the `.dsslir` reader filtered every BlockRef out of a
// `cond-br`'s operand list, so a lowered `jcc` came back with two successors and
// ZERO operands. An agreement-only rule ("if it has refs they must match") is
// vacuously satisfied by exactly that state, which is how the drop survived a
// verifier, two round-trip tests and three comments. So for the kinds whose
// successors RIDE their operands, an absence is a violation.
//
// ⚠ THE CLASSIFICATION IS A TOTAL SWITCH ON `TargetTerminatorKind` WITH NO
// `default:` — a new enumerator is a COMPILE error here rather than a silently
// unchecked terminator shape, the same totality discipline `parseInst`'s
// dispatch uses. It is schema VOCABULARY (a closed enum every `.target.json`
// declares into), never an arch / format / language identity: no arm asks which
// CPU or which object format this is.
void checkTerminatorBlockRefsMatchSuccessors(Lir const& lir,
                                             TargetSchema const& sch,
                                             DiagnosticReporter& reporter) {
    std::size_t const fnCount = lir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < fnCount; ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        std::uint32_t const blockCount = lir.funcBlockCount(fn);
        for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
            LirBlockId const bb = lir.funcBlockAt(fn, bi);
            std::uint32_t const n = lir.blockInstCount(bb);
            for (std::uint32_t i = 0; i < n; ++i) {
                LirInstId const inst = lir.blockInstAt(bb, i);
                auto const* info = sch.opcodeInfo(lir.instOpcode(inst));
                if (info == nullptr || !info->isTerminator()) continue;

                // Does this terminator kind carry its successor list on its own
                // operands? `Br`/`CondBr` do (the encoder reads them). `Return`
                // and `Unreachable` have NO successors, so "carries them" is the
                // assertion that they carry no BlockRef at all — the same rule,
                // not an exemption. `Switch` is reserved-unbuilt and
                // `IndirectBr` carries only its address register (its variadic
                // address-taken edges ride the successor list alone), so for
                // those the operands are OPTIONAL and only their CONTENT is
                // checked.
                bool refsRequired = false;
                switch (info->terminatorKind) {
                    case TargetTerminatorKind::Br:
                    case TargetTerminatorKind::CondBr:
                    case TargetTerminatorKind::Return:
                    case TargetTerminatorKind::Unreachable:
                        refsRequired = true;
                        break;
                    case TargetTerminatorKind::Switch:
                    case TargetTerminatorKind::IndirectBr:
                        refsRequired = false;
                        break;
                    case TargetTerminatorKind::None:
                        // Unreachable by construction: `isTerminator()` IS
                        // `terminatorKind != None`, and the `continue` above
                        // already took every non-terminator. Enumerated so the
                        // switch stays total.
                        continue;
                }

                std::vector<std::uint32_t> refs;
                for (auto const& o : lir.instOperands(inst)) {
                    if (o.kind == LirOperandKind::BlockRef) {
                        refs.push_back(o.blockSlot);
                    }
                }
                auto const succs = lir.blockSuccessors(bb);
                if (refs.empty() && !refsRequired) continue;

                bool same = (refs.size() == succs.size());
                for (std::size_t k = 0; same && k < refs.size(); ++k) {
                    same = (refs[k] == succs[k].v);
                }
                if (same) continue;

                std::string refText;
                for (std::uint32_t r : refs) {
                    if (!refText.empty()) refText += ", ";
                    refText += std::format("^{}", r);
                }
                std::string succText;
                for (LirBlockId s : succs) {
                    if (!succText.empty()) succText += ", ";
                    succText += std::format("^{}", s.v);
                }
                report(reporter, std::format(
                    "LirVerifier: terminator inst {} ('{}', {}) declares "
                    "successors [{}] but its BlockRef operands are [{}] — the "
                    "CFG edge list and the operand list the ENCODER turns into "
                    "branch displacements must name the same blocks in the same "
                    "order{}",
                    inst.v, info->mnemonic,
                    targetTerminatorKindName(info->terminatorKind),
                    succText.empty() ? "" : succText,
                    refText.empty() ? "" : refText,
                    refs.empty()
                        ? ". The operand list is EMPTY: the branch has no target "
                          "to encode, which is what a dropped-operand round trip "
                          "looks like"
                        : ""),
                    DiagnosticCode::L_TerminatorSuccessorMismatch);
            }
        }
    }
}

// Look up the source MIR inst for a LIR inst via the `lirToMirMap`.
// Returns `InvalidMirInst` (default-constructed) when:
//   - the LIR inst id is past the map's range (defensive; means the
//     lowerer didn't record this inst)
//   - the recorded entry is the default-constructed `InvalidMirInst`
[[nodiscard]] MirInstId sourceMirInst(std::span<MirInstId const> map, LirInstId li) {
    if (!li.valid()) return MirInstId{};
    if (li.v >= map.size()) return MirInstId{};
    return map[li.v];
}

// Rule 2: for each LIR `store` inst with a source MIR Store, cross-
// check the value-operand's vreg class against
// `regClassForCoreType(interner.kind(mir.instType(mirStoreValueOp)))`.
// Walks LIR by inst and uses `lirToMirMap` for source resolution —
// REPLACES the cycle-3e positional MIR-vs-LIR walk that silently
// skipped switch-bearing functions (architect HIGH + silent-failure
// CRITICAL findings).
void checkStoreRegClassMatchesMirType(
    Lir const& lir, Mir const& mir, TypeInterner const& interner,
    TargetSchema const& sch, std::span<MirInstId const> map,
    DiagnosticReporter& reporter) {
    auto const storeOp = sch.opcodeByMnemonic("store");
    if (!storeOp.has_value()) {
        // Schema lacks `store` — non-register-machine target. Skip
        // silently is acceptable here because the rule has nothing to
        // check; if any LIR `store` HAD been emitted on such a target,
        // it would have hit `reportMissingOpcode` at lowering time.
        return;
    }
    std::size_t const fnCount = lir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < fnCount; ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        std::uint32_t const blockCount = lir.funcBlockCount(fn);
        for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
            LirBlockId const bb = lir.funcBlockAt(fn, bi);
            std::uint32_t const n = lir.blockInstCount(bb);
            for (std::uint32_t i = 0; i < n; ++i) {
                LirInstId const li = lir.blockInstAt(bb, i);
                if (lir.instOpcode(li) != *storeOp) continue;
                MirInstId const src = sourceMirInst(map, li);
                if (!src.valid()) continue;
                if (mir.instOpcode(src) != MirOpcode::Store) continue;
                auto const lops = lir.instOperands(li);
                if (lops.empty() || lops[0].kind != LirOperandKind::Reg) continue;
                auto const mops = mir.instOperands(src);
                if (mops.empty()) continue;
                TypeId const valueTy = mir.instType(mops[0]);
                if (!valueTy.valid()) continue;
                LirRegClass const expected = static_cast<LirRegClass>(
                    regClassForCoreType(interner.kind(valueTy)));
                LirRegClass const actual = lops[0].reg.regClass();
                if (expected != actual && actual != LirRegClass::None) {
                    report(reporter, std::format(
                        "LirVerifier: Store value-operand reg class {} does "
                        "not match MIR pointee-type regClass {} (LIR inst {})",
                        static_cast<int>(actual),
                        static_cast<int>(expected), li.v));
                }
            }
        }
    }
}

// Rule 3: for every LIR inst with both a valid result vreg AND a
// recorded source MIR inst, the LirReg's class must match the MIR
// inst's result type's expected class. Same lirToMirMap-driven walk
// as rule 2 — robust to cycle-3b Switch lowering's extra LIR blocks.
void checkVregClassMatchesMirType(
    Lir const& lir, Mir const& mir, TypeInterner const& interner,
    std::span<MirInstId const> map, DiagnosticReporter& reporter) {
    std::size_t const fnCount = lir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < fnCount; ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        std::uint32_t const blockCount = lir.funcBlockCount(fn);
        for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
            LirBlockId const bb = lir.funcBlockAt(fn, bi);
            std::uint32_t const n = lir.blockInstCount(bb);
            for (std::uint32_t i = 0; i < n; ++i) {
                LirInstId const li = lir.blockInstAt(bb, i);
                LirReg const result = lir.instResult(li);
                if (!result.valid()) continue;
                MirInstId const src = sourceMirInst(map, li);
                if (!src.valid()) continue;
                MirOpcode const mop = mir.instOpcode(src);
                // Skip opcodes whose LIR vreg class is target-defined
                // rather than type-derived. Phi/Alloca/GlobalAddr
                // produce GPR pointers regardless of payload type.
                if (mop == MirOpcode::Phi)        continue;
                if (mop == MirOpcode::Alloca)     continue;
                if (mop == MirOpcode::GlobalAddr) continue;
                TypeId const mty = mir.instType(src);
                if (!mty.valid()) continue;
                if (interner.kind(mty) == TypeKind::Void) continue;
                LirRegClass const expected = static_cast<LirRegClass>(
                    regClassForCoreType(interner.kind(mty)));
                LirRegClass const actual = result.regClass();
                if (expected != actual && actual != LirRegClass::None) {
                    report(reporter, std::format(
                        "LirVerifier: LIR inst {} produced reg class {} but "
                        "MIR inst %{} has type kind expecting class {}",
                        li.v, static_cast<int>(actual), src.v,
                        static_cast<int>(expected)));
                }
            }
        }
    }
}

// Rule 4: IntrinsicCall result-validity. For every LIR `intrinsic_call`
// inst whose source MIR inst is a MIR `IntrinsicCall`, the LIR
// result-reg presence must match the MIR result type — Void MIR
// type → LIR result MUST be `InvalidLirReg`; non-Void MIR type → LIR
// result MUST be valid. Closes the cycle-3e D-3e.2 deferral.
void checkIntrinsicCallResultValidity(Lir const& lir, Mir const& mir,
                                      TypeInterner const& interner,
                                      TargetSchema const& schema,
                                      std::span<MirInstId const> map,
                                      DiagnosticReporter& reporter) {
    auto const icOp = schema.opcodeByMnemonic("intrinsic_call");
    if (!icOp.has_value()) return;
    std::size_t const fnCount = lir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < fnCount; ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        std::uint32_t const blockCount = lir.funcBlockCount(fn);
        for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
            LirBlockId const bb = lir.funcBlockAt(fn, bi);
            std::uint32_t const n = lir.blockInstCount(bb);
            for (std::uint32_t i = 0; i < n; ++i) {
                LirInstId const li = lir.blockInstAt(bb, i);
                if (lir.instOpcode(li) != *icOp) continue;
                MirInstId const src = sourceMirInst(map, li);
                if (!src.valid()) continue;
                if (mir.instOpcode(src) != MirOpcode::IntrinsicCall) continue;
                TypeId const mty = mir.instType(src);
                bool const mirVoid = !mty.valid()
                    || interner.kind(mty) == TypeKind::Void;
                bool const lirHasResult = lir.instResult(li).valid();
                if (mirVoid && lirHasResult) {
                    report(reporter, std::format(
                        "LirVerifier: intrinsic_call LIR inst {} produced a "
                        "result reg but MIR inst %{} has Void type",
                        li.v, src.v));
                }
                if (!mirVoid && !lirHasResult) {
                    report(reporter, std::format(
                        "LirVerifier: intrinsic_call LIR inst {} has no "
                        "result reg but MIR inst %{} has a non-Void type",
                        li.v, src.v));
                }
            }
        }
    }
}

} // namespace

LirVerifyResult verifyLir(Lir const&                  lir,
                          Mir const&                  mir,
                          TypeInterner const&         interner,
                          TargetSchema const&         schema,
                          std::span<MirInstId const>  lirToMirMap,
                          DiagnosticReporter&         reporter) {
    auto const baseline = reporter.errorCount();
    checkMemOperandPairing(lir, schema, reporter);
    checkTerminatorBlockRefsMatchSuccessors(lir, schema, reporter);
    checkStoreRegClassMatchesMirType(lir, mir, interner, schema, lirToMirMap, reporter);
    checkVregClassMatchesMirType(lir, mir, interner, lirToMirMap, reporter);
    checkIntrinsicCallResultValidity(lir, mir, interner, schema, lirToMirMap, reporter);
    return {reporter.errorCount() == baseline};
}

// ── post-regalloc verifier ─────────────────────────────────────────

bool verifyLirPostRegalloc(Lir const& lir, TargetSchema const& schema,
                           DiagnosticReporter& reporter) {
    auto const baseline = reporter.errorCount();
    auto const frameLoadOp  = schema.opcodeByMnemonic(schema.frameLoadMnemonic());
    auto const frameStoreOp = schema.opcodeByMnemonic(schema.frameStoreMnemonic());
    auto checkPhys = [&](LirReg r, char const* what, std::uint32_t instV) {
        if (r.valid() && r.isPhysical == 0) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::L_VirtualRegInPostRegalloc;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format("verifyLirPostRegalloc: inst {} has a "
                                     "virtual {} reg (vreg id {})",
                                     instV, what,
                                     static_cast<std::uint32_t>(r.id));
            reporter.report(std::move(d));
        }
    };
    std::size_t const fnCount = lir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < fnCount; ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        std::uint32_t const blockCount = lir.funcBlockCount(fn);
        for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
            LirBlockId const b = lir.funcBlockAt(fn, bi);
            std::uint32_t const n = lir.blockInstCount(b);
            for (std::uint32_t i = 0; i < n; ++i) {
                LirInstId const inst = lir.blockInstAt(b, i);
                checkPhys(lir.instResult(inst), "result", inst.v);
                auto const ops = lir.instOperands(inst);
                for (auto const& op : ops) {
                    if (op.kind == LirOperandKind::Reg) {
                        checkPhys(op.reg, "operand", inst.v);
                    }
                }
                // Frame pseudo-op slot sentinel: payload must be a
                // valid `LirSpillSlot` (non-zero per the strong-id
                // sentinel convention).
                std::uint16_t const op = lir.instOpcode(inst);
                bool const isFrame =
                    (frameLoadOp.has_value()  && op == *frameLoadOp)
                 || (frameStoreOp.has_value() && op == *frameStoreOp);
                if (isFrame && lir.instPayload(inst) == 0) {
                    ParseDiagnostic d;
                    d.code     = DiagnosticCode::L_InvalidSpillSlotSentinel;
                    d.severity = DiagnosticSeverity::Error;
                    d.actual   = std::format(
                        "verifyLirPostRegalloc: inst {} (frame_load/frame_store) "
                        "has payload 0 — invalid LirSpillSlot sentinel", inst.v);
                    reporter.report(std::move(d));
                }
            }
        }
    }
    return reporter.errorCount() == baseline;
}

// ── text-load verifier (ML8 cycle 2) ─────────────────────────────────

bool verifyLirText(Lir const& lir, TargetSchema const& schema,
                   DiagnosticReporter& reporter) {
    auto const baseline = reporter.errorCount();
    // The LIR-only rules; future LIR-only rules added to `verifyLir` should
    // join here too. The text-load path has no MIR cross-reference (the source
    // MIR isn't part of `.dsslir`), so MIR-dependent rules (2–4) are
    // deliberately not invoked.
    // ★ Rule 1b matters MOST on this path (D-LIR-TEXT-CONDBR-BLOCKREF-OPERANDS-
    // DROPPED): the text reader is the one producer that ever built a
    // terminator whose two CFG channels disagreed, and it did so silently with
    // `ok == true`. A rule that runs only where the bug cannot occur is not a
    // net.
    checkMemOperandPairing(lir, schema, reporter);
    checkTerminatorBlockRefsMatchSuccessors(lir, schema, reporter);
    return reporter.errorCount() == baseline;
}

} // namespace dss
