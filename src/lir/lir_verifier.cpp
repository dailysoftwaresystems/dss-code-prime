#include "lir/lir_verifier.hpp"

#include "core/types/parse_diagnostic.hpp"

#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// Spell an operand KIND for a diagnostic. ⚠ A TOTAL SWITCH WITH NO
// `default:` — a new `LirOperandKind` is a COMPILE error here rather than a
// silently unnamed operand in the one message a reader has to reason from,
// the same totality discipline `checkTerminatorBlockRefsMatchSuccessors`
// applies to `TargetTerminatorKind`.
[[nodiscard]] std::string_view lirOperandKindName(LirOperandKind k) {
    switch (k) {
        case LirOperandKind::None:            return "None";
        case LirOperandKind::Reg:             return "Reg";
        case LirOperandKind::ImmInt:          return "ImmInt";
        case LirOperandKind::BlockRef:        return "BlockRef";
        case LirOperandKind::SymbolRef:       return "SymbolRef";
        case LirOperandKind::MemBase:         return "MemBase";
        case LirOperandKind::MemOffset:       return "MemOffset";
        case LirOperandKind::LiteralIndex:    return "LiteralIndex";
        case LirOperandKind::ByValueStackAgg: return "ByValueStackAgg";
        case LirOperandKind::SpillSlotRef:    return "SpillSlotRef";
    }
    return "<unknown>";
}

// The operand list as a kind sequence, e.g. "[Reg, MemBase, MemOffset]".
// ★ THE DIAGNOSTIC PRINTS WHAT IT GOT, NOT ONLY WHAT IT WANTED. The rule
// this belongs to was FALSE about the shipped lowering for its whole
// lifetime (D-LIR-VERIFY-MEM-OPERAND-PAIRING-RULE-IS-FALSE) and a message
// naming only the expectation is precisely the message that cannot tell a
// reader the rule — rather than the producer — is the thing that is wrong.
[[nodiscard]] std::string operandKindSequence(std::span<LirOperand const> ops) {
    std::string s = "[";
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (i != 0) s += ", ";
        s += lirOperandKindName(ops[i].kind);
    }
    s += "]";
    return s;
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

// Rule 1: every Load/Store/Lea inst must carry exactly ONE well-formed
// ADDRESSING MODE. Walks LIR only; no MIR cross-reference needed. Operates
// per-block; safe across function boundaries.
//
// ★★★ THERE ARE **TWO** ADDRESSING MODES, AND FOR ITS WHOLE LIFETIME THIS
// RULE KNEW ONLY ONE (D-LIR-VERIFY-MEM-OPERAND-PAIRING-RULE-IS-FALSE). It
// demanded that every operand list END with `MemBase` then `MemOffset`.
// ✔MEASURED across every producer under `src/`: the shipped lowering also
// emits a SYMBOL-ADDRESSED form carrying no base/displacement pair at all —
// `lea r, [@sym]` for every `&global` (`mir_to_lir.cpp` GlobalAddr), for the
// ELF/PE/macho TLS address blocks, for a jump-table base, and the matching
// folded `[@sym]` load. So the rule was FALSE about the LIR this compiler
// actually builds, and enabling it reddened the examples corpus wholesale
// while the compiler was correct.
//
// ★★ WHY IT SURVIVED, WHICH IS THE TRANSFERABLE PART: it had only ever run
// on HAND-BUILT test modules. A rule whose only subjects are synthesised by
// the same author who wrote the rule cannot discover what the shipped path
// emits — it can only re-state its author's belief. That is why the
// accept-arm pin for this rule lowers real c source through the real
// `lowerToLir` instead of assembling a module out of arenas.
//
// ⚠ THE TWO FORMS ARE ASSERTED DISJOINT, WHICH IS WHAT KEEPS THIS FROM
// DEGENERATING INTO "ACCEPT ANYTHING". ✔MEASURED: no producer anywhere under
// `src/` puts a `SymbolRef` in the same operand list as a `MemBase` or a
// `MemOffset` — the two modes are strictly disjoint, so a list mixing them
// is a half-rewritten address and is REPORTED, in both directions.
//
// ⚠ AND THE SYMBOL FORM IS NOT "ENDS WITH A SymbolRef" — that spelling is
// the obvious one and it is WRONG. ✔MEASURED: computed-goto's `&&label`
// lowers to `lea r, [@sym ^block]` (`[SymbolRef, BlockRef]`, the BlockRef
// binding the synthetic symbol to the block's byte offset), so the SymbolRef
// is not last. Membership, not position, is the property that holds.
//
// ⚠ SCOPE, stated because the comment must not read wider than the code:
// this resolves the mnemonics `load` / `store` / `lea` only. The
// class-routed memory ops (`movsd_load`, `fldur`, `fstur_q`, the arm64
// scaled `load_u`/`store_u` twins) and the ordered/atomic ops
// (`load_acquire`, `store_release`, `cmpxchg`) carry the SAME two
// addressing modes and are NOT checked here.
enum class MemAddressForm : std::uint8_t {
    BaseDisplacement,  // [..., MemBase(scale), MemOffset(disp)], no SymbolRef
    SymbolAddressed,   // contains a SymbolRef, no MemBase and no MemOffset
    Malformed,         // neither — or both, which is a half-rewritten address
};

[[nodiscard]] MemAddressForm
classifyMemAddressForm(std::span<LirOperand const> ops) {
    bool hasSymbol = false;
    bool hasBaseOrOffset = false;
    for (auto const& o : ops) {
        if (o.kind == LirOperandKind::SymbolRef) hasSymbol = true;
        if (o.kind == LirOperandKind::MemBase
            || o.kind == LirOperandKind::MemOffset) {
            hasBaseOrOffset = true;
        }
    }
    if (hasSymbol && hasBaseOrOffset) return MemAddressForm::Malformed;
    if (hasSymbol) return MemAddressForm::SymbolAddressed;
    // The base+displacement form must be TERMINAL and ORDERED: the encoder
    // reads the addressing mode off the tail, so a `MemBase` that is not
    // second-to-last, or a pair the wrong way round, is not an address it
    // can turn into a ModR/M byte.
    if (ops.size() >= 2
        && ops[ops.size() - 2].kind == LirOperandKind::MemBase
        && ops[ops.size() - 1].kind == LirOperandKind::MemOffset) {
        return MemAddressForm::BaseDisplacement;
    }
    return MemAddressForm::Malformed;
}

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
                if (classifyMemAddressForm(ops) != MemAddressForm::Malformed) {
                    continue;
                }
                auto const* info = sch.opcodeInfo(op);
                report(reporter, std::format(
                    "LirVerifier: memory inst {} ('{}') carries no well-formed "
                    "addressing mode — its operands are {}. A LIR "
                    "load/store/lea must carry EITHER a base+displacement "
                    "address, whose operand list ENDS with MemBase then "
                    "MemOffset and names no symbol, OR a symbol-addressed one, "
                    "which names a SymbolRef and carries no MemBase/MemOffset "
                    "at all. Carrying both is a half-rewritten address and is "
                    "rejected the same way as carrying neither",
                    inst.v, info != nullptr ? info->mnemonic : "?",
                    operandKindSequence(ops)),
                    DiagnosticCode::L_MemOperandMalformed);
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

// ── Rule 1c: module SIDE-STRUCTURE integrity ─────────────────────────
//
// D-LIR-PER-INST-REG-CONSTRAINTS. A `Lir` module carries two
// by-index side structures beside the instruction stream: the
// wide-literal pool (referenced by `LiteralIndex` OPERANDS) and the
// per-instruction register-constraint pool (referenced by
// `detail::LirInst::regConstraints`). FOUR passes rebuild the stream into
// a fresh builder and must carry both across.
//
// ★★★ WHY A VERIFIER RULE AND NOT JUST THE SHARED COPY HELPER. The helper
// (`lir_pass_util::copyModuleSideStructures`) makes the POOLS survive by
// construction — there is one function to call and no per-pass copy code
// to forget. It cannot make the per-INSTRUCTION handle survive: a rebuild
// re-CREATES each instruction, so only the pass knows the correspondence,
// and a dropped handle reads as the perfectly legal
// `kLirNoRegConstraints`. Nothing dangles, nothing shrinks, the module
// verifies, and the allocator reuses a register the instruction destroys.
//
// ★★ WHAT MAKES THAT DETECTABLE FROM ONE MODULE. Because the helper
// carries the POOL unconditionally, a dropped handle leaves a pool entry
// that NO instruction references. So "every constraint-pool entry is
// referenced" is exactly the negation of the silent drop, needs no
// before/after pair, and runs on every module the verifier already sees.
// (`verifyLirRebuild` adds the checks that genuinely need the pair.)
//
// ⚠ The rule is NOT symmetric across the two pools. An unreferenced
// LITERAL entry is not asserted here: literal references ride OPERANDS,
// which every rebuilding pass copies verbatim, so the literal pool has no
// analogous drop — and MIR→LIR legitimately interns a literal on a path
// that then declines to emit its instruction. Asserting a property the
// producer does not hold would trade a real net for a false red. The
// literal pool's real exposure — a dangling `litIndex`, and a pool that
// shrank across a rebuild — IS covered, here and in `verifyLirRebuild`.
void checkSideStructureIntegrity(Lir const& lir, DiagnosticReporter& reporter) {
    // Which constraint-pool entries did we actually see referenced?
    std::vector<bool> referenced(lir.regConstraintPool().size(), false);

    std::size_t const fnCount = lir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < fnCount; ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        std::uint32_t const blockCount = lir.funcBlockCount(fn);
        for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
            LirBlockId const bb = lir.funcBlockAt(fn, bi);
            std::uint32_t const n = lir.blockInstCount(bb);
            for (std::uint32_t i = 0; i < n; ++i) {
                LirInstId const inst = lir.blockInstAt(bb, i);
                for (auto const& o : lir.instOperands(inst)) {
                    if (o.kind != LirOperandKind::LiteralIndex) continue;
                    if (o.litIndex >= lir.literalPool().size()) {
                        report(reporter, std::format(
                            "LirVerifier: inst {} references literal pool entry "
                            "lit#{} but the module's literal pool holds {} "
                            "entries — the index outlived the pool it names "
                            "(a rebuild that did not carry the pool across)",
                            inst.v, o.litIndex, lir.literalPool().size()),
                            DiagnosticCode::L_SideStructureIndexDangling);
                    }
                }
                std::uint32_t const h = lir.instRegConstraintHandle(inst);
                if (h == kLirNoRegConstraints) continue;
                std::uint32_t const idx = lirRegConstraintIndexForHandle(h);
                // ⚠ THE BOOKKEEPING WRITE IS GUARDED BY ITS OWN POSITIVE
                // IN-RANGE TEST, not by the error arm's `continue`. Mutation
                // testing found the difference: with the reporting arm's
                // condition weakened, an `else`-shaped version wrote past the
                // end of `referenced` and the test SEGFAULTED instead of
                // failing its assertion — so the rule's memory safety was
                // riding on the rule's diagnostic. Two responsibilities, two
                // tests; the report can now be changed or removed without the
                // verifier corrupting its own state.
                if (idx < lir.regConstraintPool().size()) {
                    referenced[idx] = true;
                    continue;
                }
                report(reporter, std::format(
                    "LirVerifier: inst {} carries register-constraint "
                    "handle {} (pool index {}) but the module's "
                    "register-constraint pool holds {} entries — the "
                    "handle outlived the pool it names (a rebuild that "
                    "did not carry the pool across)",
                    inst.v, h, idx, lir.regConstraintPool().size()),
                    DiagnosticCode::L_SideStructureIndexDangling);
            }
        }
    }

    for (std::size_t i = 0; i < referenced.size(); ++i) {
        if (referenced[i]) continue;
        report(reporter, std::format(
            "LirVerifier: register-constraint pool entry rc#{} is referenced "
            "by NO instruction ({} entries in the pool). A constraint set "
            "exists only because some instruction declared it, so an "
            "unreferenced entry means an instruction LOST its handle — the "
            "silent shape of a rebuild that carried the pool but not the "
            "per-instruction `regConstraints` field (call "
            "`lir_pass_util::carryInstSideData` for every rebuilt "
            "instruction), or of a pass that deleted an instruction which "
            "declared registers destroyed",
            i, lir.regConstraintPool().size()),
            DiagnosticCode::L_SideStructureReferenceLost);
    }
}

// Count the side-structure REFERENCES a module makes. Used only by the
// paired rebuild check: a count that dropped is a reference that was
// lost, which for the literal pool (whose references ride operands and
// therefore cannot be caught by the unreferenced-entry rule above) is the
// only detectable signature of a rebuild that dropped an instruction's
// wide constant.
struct SideStructureCensus {
    std::size_t literalRefs    = 0;  // `LiteralIndex` operands
    std::size_t constraintRefs = 0;  // insts with a non-zero handle
};

[[nodiscard]] SideStructureCensus censusSideStructures(Lir const& lir) {
    SideStructureCensus c;
    std::size_t const fnCount = lir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < fnCount; ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        std::uint32_t const blockCount = lir.funcBlockCount(fn);
        for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
            LirBlockId const bb = lir.funcBlockAt(fn, bi);
            std::uint32_t const n = lir.blockInstCount(bb);
            for (std::uint32_t i = 0; i < n; ++i) {
                LirInstId const inst = lir.blockInstAt(bb, i);
                for (auto const& o : lir.instOperands(inst)) {
                    if (o.kind == LirOperandKind::LiteralIndex) ++c.literalRefs;
                }
                if (lir.instRegConstraintHandle(inst) != kLirNoRegConstraints) {
                    ++c.constraintRefs;
                }
            }
        }
    }
    return c;
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
                TypeKind const valueKind = interner.kind(valueTy);
                // ⚠ THE WIDE FLOATS ARE MEMORY-RESIDENT AND THEIR VALUE
                // OPERAND IS A GPR **WORD**, BY DESIGN — not a violation.
                // ✔MEASURED 2026-08-15 (`examples/c/c_long_double`,
                // `…_constfold`, the last 2 of the 265 this rule set reddened):
                // `lowerF80Store` / `lowerF128Store`
                // (D-CSUBSET-LONG-DOUBLE-X87-ARITH / -IEEE128-ARITH) lower a
                // `long double` store to a memory→memory copy issued as GPR
                // word stores, while the MIR Store's value type is still F80 /
                // F128 — so `regClassForCoreType` says FPR and the LIR says
                // GPR, and the LIR is RIGHT. The rule knew one representation
                // for a float value and the lowering has two, which is Rule 1's
                // defect wearing different clothes.
                // ★ NARROW ON PURPOSE: exactly the two memory-resident kinds,
                // named individually rather than "any float", so an ordinary
                // F32/F64 store landing in a GPR — the hardcoded-GPR silent
                // corruption this rule exists to catch — still REPORTS.
                if (valueKind == TypeKind::F80 || valueKind == TypeKind::F128) {
                    continue;
                }
                LirRegClass const expected = static_cast<LirRegClass>(
                    regClassForCoreType(valueKind));
                LirRegClass const actual = lops[0].reg.regClass();
                if (expected != actual && actual != LirRegClass::None) {
                    auto const* linfo = sch.opcodeInfo(lir.instOpcode(li));
                    report(reporter, std::format(
                        "LirVerifier: store LIR inst {} ('{}') takes a {} value "
                        "operand but its source MIR Store %{} stores a value of "
                        "type kind {}, which belongs in a {} register — a store "
                        "issued against the wrong register FILE encodes to "
                        "valid-looking bytes that move the wrong 8 bytes",
                        li.v, linfo != nullptr ? linfo->mnemonic : "?",
                        lirRegClassName(actual), src.v,
                        static_cast<int>(valueKind), lirRegClassName(expected)));
                }
            }
        }
    }
}

// Rule 3: for every LIR inst with both a valid result vreg AND a
// recorded source MIR inst, the LirReg's class must match the MIR
// inst's result type's expected class. Same lirToMirMap-driven walk
// as rule 2 — robust to cycle-3b Switch lowering's extra LIR blocks.
//
// ⚠⚠⚠ THIS RULE IS **MEASURED FALSE** ABOUT THE SHIPPED LOWERING AND IS
// DELIBERATELY NOT PART OF `verifyLir`. It is reachable only through
// `verifyLirVregClassesAgainstMir`. Do not re-add the call until the root
// cause below is closed — doing so reds 31 of the 594 `examples/` tests.
//
// ✔MEASURED 2026-08-15, wiring `verifyLir` into `compile_pipeline.cpp`:
// 31 examples red here, every one of them float-bearing, and the shape is a
// SINGLE root cause rather than a family of special cases —
//
// ★★★ `lirToMir` IS MANY-TO-ONE AND THIS RULE ASSUMES ONE-TO-ONE. MIR→LIR
// expands one MIR instruction into SEVERAL LIR instructions and records the
// same `MirInstId` against all of them. Only ONE of them materializes the
// MIR value; the others are helpers, and a helper's register class answers a
// different question than the MIR result type asks. Measured instances:
//
//   * `lea_frame_slot` → gpr, source MIR `load` of an **F64** — the address
//     materializer for a memory-resident value, sharing its MIR inst with
//     the FPR load that actually produces the double. (⚠ F64, an ORDINARY
//     double — so "wide floats only" is NOT the predicate, and believing it
//     was cost one wrong hypothesis here.)
//   * `alloca` / `lea_frame_slot` → gpr, source MIR `arg`/`call` of F80/F128
//     — the same address-propagation model, where the LIR representation of
//     the value IS its address.
//   * `fldur_q` → vr, source MIR `fptosi` whose result type is **I32** — a
//     wide-float carrier mapped to the conversion it feeds.
//
// ⚠ WHY IT CANNOT BE FIXED FROM THIS FILE: the rule needs "the LIR inst that
// DEFINES this MIR inst's value", and no such channel exists —
// `MirToLirResult` exposes only the many-to-one `lirToMir`. The forward
// value map lives inside `mir_to_lir.cpp`'s `defineValue` and is not
// published. Closing this means publishing it (or recording a
// materializer flag), which is a change to the LOWERING, not the verifier.
// Guessing a proxy here ("the last LIR inst mapped to it") would be the same
// mistake Rule 1 made: a rule asserting its author's belief about a shape it
// has never measured.
//
// ★ Same family as D-LIR-VERIFY-MEM-OPERAND-PAIRING-RULE-IS-FALSE — and
// found the same way, by running it on real modules for the first time.
// Kept (not deleted) because the hazard it was minted for is real: cycle-3d
// found `lowerLoad` / `prepassAllocatePhis` / `emitPhiMovesForEdge`
// hardcoding GPR, which is a silent wrong-register-file encode.
void checkVregClassMatchesMirType(
    Lir const& lir, Mir const& mir, TypeInterner const& interner,
    TargetSchema const& sch,
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
                    auto const* linfo = sch.opcodeInfo(lir.instOpcode(li));
                    report(reporter, std::format(
                        "LirVerifier: LIR inst {} ('{}') produced a {} result "
                        "but its source MIR inst %{} ('{}', type kind {}) "
                        "expects class {}",
                        li.v, linfo != nullptr ? linfo->mnemonic : "?",
                        lirRegClassName(actual), src.v, mnemonic(mop),
                        static_cast<int>(interner.kind(mty)),
                        lirRegClassName(expected)));
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
    checkSideStructureIntegrity(lir, reporter);
    checkStoreRegClassMatchesMirType(lir, mir, interner, schema, lirToMirMap, reporter);
    // ⚠ `checkVregClassMatchesMirType` (Rule 3) is ABSENT ON PURPOSE — it is
    // measured false about the shipped lowering (see the long note over it)
    // and reds 31 of 594 `examples/`. It is reachable through
    // `verifyLirVregClassesAgainstMir`; re-adding it here without first
    // publishing a one-to-one MIR→LIR value map re-breaks every float-bearing
    // compile. Rules 2 and 4 below use the SAME map and are measured CLEAN
    // across all 594, so the map is not uniformly unusable — only this rule's
    // one-to-one assumption is wrong.
    checkIntrinsicCallResultValidity(lir, mir, interner, schema, lirToMirMap, reporter);
    return {reporter.errorCount() == baseline};
}

bool verifyLirVregClassesAgainstMir(Lir const&                 lir,
                                    Mir const&                 mir,
                                    TypeInterner const&        interner,
                                    TargetSchema const&        schema,
                                    std::span<MirInstId const> lirToMirMap,
                                    DiagnosticReporter&        reporter) {
    auto const baseline = reporter.errorCount();
    checkVregClassMatchesMirType(lir, mir, interner, schema, lirToMirMap, reporter);
    return reporter.errorCount() == baseline;
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
    // ★ The post-regalloc module is the output of the LARGEST rebuild in
    // the pipeline (`rewriteWithAllocation`), so this is the checkpoint
    // where a dropped side-structure reference is both most likely and
    // most expensive — everything downstream of here turns into bytes.
    checkSideStructureIntegrity(lir, reporter);
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
    // ★ Rule 1b matters MOST on this path (D-LIR-TEXT-CONDBR-BLOCKREF-OPERANDS-DROPPED):
    // the text reader is the one producer that ever built a
    // terminator whose two CFG channels disagreed, and it did so silently with
    // `ok == true`. A rule that runs only where the bug cannot occur is not a
    // net.
    checkMemOperandPairing(lir, schema, reporter);
    checkTerminatorBlockRefsMatchSuccessors(lir, schema, reporter);
    // ★ Rule 1c matters on this path for the same reason 1b does: the text
    // reader is a PRODUCER of `regConstraints` handles and of literal-pool
    // entries, and it writes the two halves from two different sections of
    // the file. A `reg_constraints` block whose entries no instruction
    // names is exactly what a hand-edited or truncated `.dsslir` looks
    // like.
    checkSideStructureIntegrity(lir, reporter);
    return reporter.errorCount() == baseline;
}

// ── paired rebuild verifier ──────────────────────────────────────────

bool verifyLirRebuild(Lir const& before, Lir const& after,
                      std::string_view passName,
                      DiagnosticReporter& reporter) {
    auto const baseline = reporter.errorCount();

    // (1) A pool that SHRANK is a forgotten `copyModuleSideStructures`.
    // Checked first because it EXPLAINS the dangling references that
    // follow from it — reporting those first would bury the cause under
    // its own symptoms.
    auto poolShrank = [&](char const* what, std::size_t b, std::size_t a) {
        if (a >= b) return;
        report(reporter, std::format(
            "verifyLirRebuild: pass '{}' produced a module whose {} holds {} "
            "entries, down from {} — a rebuild must carry every module side "
            "structure across index-for-index (one call: "
            "`lir_pass_util::copyModuleSideStructures(src, b)`, right after "
            "the fresh `LirBuilder`)",
            passName, what, a, b),
            DiagnosticCode::L_SideStructurePoolShrank);
    };
    poolShrank("literal pool", before.literalPool().size(),
               after.literalPool().size());
    poolShrank("register-constraint pool", before.regConstraintPool().size(),
               after.regConstraintPool().size());

    // (2) The module-local rules on the OUTPUT — dangling indices, and
    // constraint entries nothing references.
    checkSideStructureIntegrity(after, reporter);

    // (3) References that vanished. ★ This is the only instrument that
    // catches a LITERAL reference dropped by a rebuild: a `LiteralIndex`
    // operand that a pass failed to copy leaves the pool intact and every
    // surviving index valid, so neither (1) nor (2) sees it. `>=` not `==`
    // on purpose — passes legitimately ADD instructions (2-address
    // legalize inserts movs, callconv inserts arg setup), and a pass that
    // duplicates a reference has not lost one.
    auto const b = censusSideStructures(before);
    auto const a = censusSideStructures(after);
    auto refsLost = [&](char const* what, std::size_t bn, std::size_t an) {
        if (an >= bn) return;
        report(reporter, std::format(
            "verifyLirRebuild: pass '{}' produced a module making {} {} "
            "references, down from {} — an instruction lost its reference to "
            "a module side structure. The pool still holds the entry and "
            "every surviving index still resolves, so this count is the only "
            "place the loss is visible",
            passName, an, what, bn),
            DiagnosticCode::L_SideStructureReferenceLost);
    };
    refsLost("literal-pool", b.literalRefs, a.literalRefs);
    refsLost("register-constraint", b.constraintRefs, a.constraintRefs);

    return reporter.errorCount() == baseline;
}

} // namespace dss
