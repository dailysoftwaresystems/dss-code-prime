#include "lir/lir_verifier.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_asm_region.hpp"
#include "lir/lir_pass_util.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dss {

namespace {

// ★★ THE VERDICT OF ONE PUBLIC VERIFY CALL (P68, lane `ht` — the `HirVerifier` /
// `MirVerifier` template, for free functions). Every rule reports through
// `report(verdict, …)`, which COUNTS the finding before the reporter decides
// anything — whether `report` keeps it, drops it as a recent duplicate or past a
// cap, is the reporter's business and must not become the verdict's. What counts
// is the POLICY's answer (`effectiveSeverity`, the one owner the reporter's own
// `report` asks), so a finding the operator suppressed does not count. Every code
// this verifier emits is a `kUnsuppressableCodes` member today, and members
// bypass dedup and the caps — so the count changes no verdict now; it is what
// keeps the verdict right for the NEXT code added without joining that table,
// which is how nine `HirVerifier` codes and five `MirVerifier` codes went missing.
struct LirVerdict {
    DiagnosticReporter& reporter;
    std::size_t         errorsFound = 0;

    // The call's verdict: nothing the policy makes an Error was found, AND the
    // reporter's error count did not move.
    [[nodiscard]] bool clean(std::size_t baseline) const {
        return errorsFound == 0 && reporter.errorCount() == baseline;
    }
};

void report(LirVerdict& verdict, std::string actual,
            DiagnosticCode code = DiagnosticCode::L_UnsupportedLoweringForOpcode) {
    if (verdict.reporter.effectiveSeverity(code, DiagnosticSeverity::Error)
        == DiagnosticSeverity::Error) {
        ++verdict.errorsFound;
    }
    ParseDiagnostic d;
    d.code     = code;
    d.severity = DiagnosticSeverity::Error;
    d.actual   = std::move(actual);
    verdict.reporter.report(std::move(d));
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
        case LirOperandKind::MemSymbolOffset: return "MemSymbolOffset";
        case LirOperandKind::SymbolAddress:   return "SymbolAddress";
        case LirOperandKind::LocationCounter: return "LocationCounter";
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

// ★ A SYMBOLIC DISPLACEMENT (`MemSymbolOffset`, `msg+4(%rip)`) IS A
// DISPLACEMENT: it closes the base+displacement form exactly as a `MemOffset`
// does, and it is not a `SymbolRef` — its symbol rides a pool entry, so the
// two forms stay disjoint.
[[nodiscard]] bool isMemDisplacement(LirOperandKind k) noexcept {
    return k == LirOperandKind::MemOffset
        || k == LirOperandKind::MemSymbolOffset;
}

[[nodiscard]] MemAddressForm
classifyMemAddressForm(std::span<LirOperand const> ops) {
    bool hasSymbol = false;
    bool hasBaseOrOffset = false;
    for (auto const& o : ops) {
        if (o.kind == LirOperandKind::SymbolRef
            || o.kind == LirOperandKind::SymbolAddress) {
            hasSymbol = true;
        }
        if (o.kind == LirOperandKind::MemBase || isMemDisplacement(o.kind)) {
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
        && isMemDisplacement(ops[ops.size() - 1].kind)) {
        return MemAddressForm::BaseDisplacement;
    }
    return MemAddressForm::Malformed;
}

void checkMemOperandPairing(Lir const& lir, TargetSchema const& sch,
                            LirVerdict& verdict) {
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
                report(verdict, std::format(
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
//
// ── AND THE ONE LEGAL WAY TO NAME FEWER BLOCKS THAN YOU BRANCH TO ───────────
//
// ★★★ D-OPT-JCC-FALLTHROUGH — THE PIN. `lir_peephole`'s R2 drops a branch's
// TRAILING BlockRef operand when the successor it named is already the
// NEXT-LAID-OUT BLOCK, so the encoder stops emitting a jump to +0. The CFG is
// untouched (both successors stay); the operand list is one shorter.
//
// That is the only shape in which the two channels may legitimately differ,
// and it carries the project's worst failure mode: an elided jump whose
// successor is NOT the next block falls into the WRONG BLOCK, producing a
// program with no bad byte in it — nothing a disassembler, a linker or a
// crash could point at. So the elided shape is admitted ONLY when all four
// hold, and the fourth is the one that matters:
//
//   1. exactly ONE successor is unnamed, and it is the LAST one;
//   2. the operands that remain are the successor list's PREFIX, in order;
//   3. the TARGET declares the fallthrough form of this opcode
//      (`lir_pass_util::declaresFallthroughBranchForm` — the same predicate
//      R2 consults, one owner, so the verifier can never bless an elision the
//      encoder cannot spell); and
//   4. ★ the unnamed successor IS the block laid out immediately after this
//      one, RE-DERIVED FROM THE MODULE IN HAND rather than trusted from the
//      pass that did the eliding.
//
// (4) is why this rule now also runs from `verifyLirPostRegalloc`. Before R2
// existed it ran in `verifyLir` and `verifyLirText` only — both of which sit
// BEFORE the peephole, i.e. at the one point in the pipeline where an elision
// cannot yet be observed. A rule that runs only where the bug cannot occur is
// not a net; that sentence is already in this file, about this same rule, for
// the previous defect.
void checkTerminatorBlockRefsMatchSuccessors(Lir const& lir,
                                             TargetSchema const& sch,
                                             LirVerdict& verdict) {
    // ⓘ HOISTED, and only because this rule's DUTY CYCLE changed. It used to
    // run once per compile (`verifyLir`); since D-OPT-JCC-FALLTHROUGH it also
    // runs from `verifyLirPostRegalloc`, which the pipeline calls three times.
    // A fresh `std::vector` per TERMINATOR was invisible at 1×; at 4× over a
    // module the size of sqlite it is hundreds of thousands of tiny
    // allocations for a check that reads at most a handful of elements.
    // `clear()` keeps the capacity, so the steady state is zero allocations.
    std::vector<std::uint32_t> refs;
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
                    // P68 round 8 part 4: an `asm goto` bundle's operands are
                    // its slots; its edges ride the successor list alone.
                    case TargetTerminatorKind::AsmGoto:
                        refsRequired = false;
                        break;
                    case TargetTerminatorKind::None:
                        // Unreachable by construction: `isTerminator()` IS
                        // `terminatorKind != None`, and the `continue` above
                        // already took every non-terminator. Enumerated so the
                        // switch stays total.
                        continue;
                }

                auto const ops = lir.instOperands(inst);
                refs.clear();
                for (auto const& o : ops) {
                    if (o.kind == LirOperandKind::BlockRef) {
                        refs.push_back(o.blockSlot);
                    }
                }
                auto const succs = lir.blockSuccessors(bb);
                if (refs.empty() && !refsRequired) continue;

                bool const prefixMatches =
                    refs.size() <= succs.size()
                    && std::equal(refs.begin(), refs.end(), succs.begin(),
                                  [](std::uint32_t r, LirBlockId s) {
                                      return r == s.v;
                                  });
                if (prefixMatches && refs.size() == succs.size()) continue;

                // ★ D-OPT-JCC-FALLTHROUGH — the elided shape. Every clause is
                // load-bearing; see the four numbered conditions above. The
                // layout question is re-derived HERE, from this module, so a
                // later pass that reorders blocks turns a silent wrong-block
                // fall into a loud build failure.
                bool elidedFallthrough = false;
                if (prefixMatches && refs.size() + 1 == succs.size()
                    && refs.size() == ops.size()
                    && lir_pass_util::declaresFallthroughBranchForm(
                           sch, lir.instOpcode(inst), ops.size() + 1)) {
                    elidedFallthrough =
                        (bi + 1 < blockCount)
                        && (succs.back().v == lir.funcBlockAt(fn, bi + 1).v);
                }
                if (elidedFallthrough) continue;

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
                // ★ D-OPT-JCC-FALLTHROUGH: say WHICH of the four conditions a
                // near-miss elision failed, and say it with the two block ids
                // that decide it. "The operand list is one short" is the
                // shape; "^7 is not the next block, ^3 is" is the bug.
                std::string why;
                if (prefixMatches && refs.size() + 1 == succs.size()
                    && refs.size() == ops.size()) {
                    if (!lir_pass_util::declaresFallthroughBranchForm(
                            sch, lir.instOpcode(inst), ops.size() + 1)) {
                        why = std::format(
                            ". The trailing edge ^{} is unnamed, which is the "
                            "shape of a D-OPT-JCC-FALLTHROUGH elision — but "
                            "this target declares NO fallthrough encoding "
                            "form for '{}' (no variant matching its guard "
                            "tuple minus the trailing blockref), so there is "
                            "no way to encode the branch without it",
                            succs.back().v, info->mnemonic);
                    } else if (bi + 1 >= blockCount) {
                        why = std::format(
                            ". The trailing edge ^{} is unnamed (a "
                            "D-OPT-JCC-FALLTHROUGH elision) but this is the "
                            "LAST block laid out in the function — there is "
                            "nothing after it to fall through TO, so control "
                            "would leave the function's bytes",
                            succs.back().v);
                    } else {
                        why = std::format(
                            ". The trailing edge ^{} is unnamed (a "
                            "D-OPT-JCC-FALLTHROUGH elision), which is legal "
                            "ONLY when that successor is the NEXT-LAID-OUT "
                            "block — and the block laid out after ^{} is ^{}. "
                            "Falling through would enter the WRONG BLOCK with "
                            "no incorrect byte anywhere to show for it",
                            succs.back().v, bb.v,
                            lir.funcBlockAt(fn, bi + 1).v);
                    }
                } else if (refs.empty()) {
                    why = ". The operand list is EMPTY: the branch has no "
                          "target to encode, which is what a dropped-operand "
                          "round trip looks like";
                }
                report(verdict, std::format(
                    "LirVerifier: terminator inst {} ('{}', {}) declares "
                    "successors [{}] but its BlockRef operands are [{}] — the "
                    "CFG edge list and the operand list the ENCODER turns into "
                    "branch displacements must name the same blocks in the same "
                    "order{}",
                    inst.v, info->mnemonic,
                    targetTerminatorKindName(info->terminatorKind),
                    succText.empty() ? "" : succText,
                    refText.empty() ? "" : refText,
                    why),
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
void checkSideStructureIntegrity(Lir const& lir, LirVerdict& verdict) {
    // Which constraint-pool entries did we actually see referenced?
    std::vector<bool> referenced(lir.regConstraintPool().size(), false);
    // P68 round 8 part 4: and which asm-region entries — the FOURTH side
    // structure, referenced by index from `detail::LirInst::asmRegion` exactly
    // as the constraint pool is from `regConstraints`, so it gets the same two
    // rules (a dangling handle; an entry nothing references), plus the
    // bundle's own bookkeeping, which only this pairing can see.
    std::vector<bool> referencedRegions(lir.asmRegionPool().size(), false);

    std::size_t const fnCount = lir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < fnCount; ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        std::uint32_t const blockCount = lir.funcBlockCount(fn);
        for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
            LirBlockId const bb = lir.funcBlockAt(fn, bi);
            std::uint32_t const n = lir.blockInstCount(bb);
            for (std::uint32_t i = 0; i < n; ++i) {
                LirInstId const inst = lir.blockInstAt(bb, i);
                if (std::uint32_t const rh = lir.instAsmRegionHandle(inst);
                    rh != kLirNoAsmRegion) {
                    std::uint32_t const ridx = lirAsmRegionIndexForHandle(rh);
                    if (ridx < lir.asmRegionPool().size()) {
                        referencedRegions[ridx] = true;
                        // The roles are read BY POSITION, so every operand
                        // must have one, and every role-bearing operand must
                        // be a register — otherwise `lirForEachInstDef` would
                        // silently skip a write.
                        LirAsmRegion const& region = lir.asmRegionPool().at(ridx);
                        auto const ops = lir.instOperands(inst);
                        if (ops.size() != region.roles.size()) {
                            report(verdict, std::format(
                                "LirVerifier: asm-region bundle inst {} has {} "
                                "operand(s) but its region (ar#{}) declares {} "
                                "slot(s) — a role is read by position, so an "
                                "operand past the slot list has none and its "
                                "write would be invisible to register "
                                "allocation",
                                inst.v, ops.size(), ridx, region.roles.size()),
                                DiagnosticCode::L_AsmRegionMalformed);
                        } else {
                            for (std::size_t k = 0; k < ops.size(); ++k) {
                                if (ops[k].kind == LirOperandKind::Reg
                                    && ops[k].reg.valid()) {
                                    continue;
                                }
                                report(verdict, std::format(
                                    "LirVerifier: asm-region bundle inst {} "
                                    "operand {} ({} slot) is not a register — "
                                    "every bundle operand stands for a register "
                                    "the template names",
                                    inst.v, k, lirAsmRoleName(region.roles[k])),
                                    DiagnosticCode::L_AsmRegionMalformed);
                            }
                        }
                    } else {
                        report(verdict, std::format(
                            "LirVerifier: inst {} carries asm-region handle {} "
                            "(pool index {}) but the module's asm-region pool "
                            "holds {} entries — the handle outlived the pool it "
                            "names (a rebuild that did not carry the pool "
                            "across)",
                            inst.v, rh, ridx, lir.asmRegionPool().size()),
                            DiagnosticCode::L_SideStructureIndexDangling);
                    }
                }
                for (auto const& o : lir.instOperands(inst)) {
                    if (o.kind != LirOperandKind::LiteralIndex
                        && o.kind != LirOperandKind::MemSymbolOffset
                        && o.kind != LirOperandKind::SymbolAddress) {
                        continue;
                    }
                    if (o.litIndex >= lir.literalPool().size()) {
                        report(verdict, std::format(
                            "LirVerifier: inst {} references literal pool entry "
                            "lit#{} but the module's literal pool holds {} "
                            "entries — the index outlived the pool it names "
                            "(a rebuild that did not carry the pool across)",
                            inst.v, o.litIndex, lir.literalPool().size()),
                            DiagnosticCode::L_SideStructureIndexDangling);
                        continue;
                    }
                    // ★ A SYMBOLIC DISPLACEMENT MUST NAME A SYMBOL ADDRESS, and
                    // a wide constant must NOT: the two operand kinds index one
                    // pool, so an index that lands on the other kind's arm is a
                    // value the encoder would read as something it is not.
                    bool const isSymbolAddress = std::holds_alternative<
                        LirSymbolAddress>(lir.literalValue(o.litIndex).value);
                    if (isSymbolAddress
                        != (o.kind == LirOperandKind::MemSymbolOffset
                            || o.kind == LirOperandKind::SymbolAddress)) {
                        report(verdict, std::format(
                            "LirVerifier: inst {} operand {} names literal pool "
                            "entry lit#{}, which {} a symbol address — a "
                            "MemSymbolOffset or SymbolAddress must name one and "
                            "a LiteralIndex must not",
                            inst.v, lirOperandKindName(o.kind), o.litIndex,
                            isSymbolAddress ? "is" : "is not"),
                            DiagnosticCode::L_MemOperandMalformed);
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
                report(verdict, std::format(
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
        report(verdict, std::format(
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

    for (std::size_t i = 0; i < referencedRegions.size(); ++i) {
        // Each entry's own shape — the one statement the builder aborts on,
        // REPORTED here for a module that reached the verifier some other way
        // (the `.dsslir` reader).
        if (auto const defect = lirAsmRegionShapeDefect(
                lir.asmRegionPool().at(static_cast<std::uint32_t>(i)))) {
            report(verdict, std::format(
                "LirVerifier: asm-region pool entry ar#{} is malformed: {}",
                i, *defect),
                DiagnosticCode::L_AsmRegionMalformed);
        }
        if (referencedRegions[i]) continue;
        report(verdict, std::format(
            "LirVerifier: asm-region pool entry ar#{} is referenced by NO "
            "instruction ({} entries in the pool). A region exists only because "
            "an inline-asm statement's bundle declared it, so an unreferenced "
            "entry means a bundle LOST its handle — a rebuild that carried the "
            "pool but not the per-instruction `asmRegion` field (call "
            "`lir_pass_util::carryInstSideData` for every rebuilt instruction), "
            "or a pass that deleted the statement",
            i, lir.asmRegionPool().size()),
            DiagnosticCode::L_SideStructureReferenceLost);
    }
}

// P68 round 8 part 4 — THE BUNDLE IS THE OPCODE AND THE HANDLE, TOGETHER. An
// instruction carrying a region handle must be the target's `asm_region` op,
// and that op must carry one: a handle on any other instruction would make
// `lirForEachInstDef` read its operands through roles they were never given,
// and a bundle without one is a statement with operands and no template.
void checkAsmRegionOpcodes(Lir const& lir, TargetSchema const& sch,
                           LirVerdict& verdict) {
    auto const bundleOp     = sch.opcodeByMnemonic(kLirAsmRegionMnemonic);
    auto const bundleGotoOp = sch.opcodeByMnemonic(kLirAsmRegionGotoMnemonic);
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
                bool const isBundleOp =
                    (bundleOp.has_value() && op == *bundleOp)
                    || (bundleGotoOp.has_value() && op == *bundleGotoOp);
                bool const hasRegion =
                    lir.instAsmRegionHandle(inst) != kLirNoAsmRegion;
                if (isBundleOp == hasRegion) continue;
                report(verdict, std::format(
                    "LirVerifier: inst {} {} — an asm-region bundle is a "
                    "'{}' or '{}' opcode AND a region handle, never one "
                    "without the other",
                    inst.v,
                    hasRegion ? "carries an asm-region handle but is not a "
                                "bundle opcode"
                              : "is a bundle opcode but carries no "
                                "asm-region handle",
                    kLirAsmRegionMnemonic, kLirAsmRegionGotoMnemonic),
                    DiagnosticCode::L_AsmRegionMalformed);
            }
        }
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
    std::size_t regionRefs     = 0;  // asm-region bundles (P68 round 8 part 4)
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
                    if (o.kind == LirOperandKind::LiteralIndex
                        || o.kind == LirOperandKind::MemSymbolOffset
                        || o.kind == LirOperandKind::SymbolAddress) {
                        ++c.literalRefs;
                    }
                }
                if (lir.instRegConstraintHandle(inst) != kLirNoRegConstraints) {
                    ++c.constraintRefs;
                }
                if (lir.instAsmRegionHandle(inst) != kLirNoAsmRegion) {
                    ++c.regionRefs;
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
    LirVerdict& verdict) {
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
                    report(verdict, std::format(
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
    std::span<MirInstId const> map, LirVerdict& verdict) {
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
                    report(verdict, std::format(
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
// result MUST be valid. Closes the cycle-3e D-PLAN12-INTRINSICCALL-VOID-NON-VOID-RESULT-VALIDITY-VERIFIER-RULE-2 deferral.
void checkIntrinsicCallResultValidity(Lir const& lir, Mir const& mir,
                                      TypeInterner const& interner,
                                      TargetSchema const& schema,
                                      std::span<MirInstId const> map,
                                      LirVerdict& verdict) {
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
                    report(verdict, std::format(
                        "LirVerifier: intrinsic_call LIR inst {} produced a "
                        "result reg but MIR inst %{} has Void type",
                        li.v, src.v));
                }
                if (!mirVoid && !lirHasResult) {
                    report(verdict, std::format(
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
    LirVerdict verdict{reporter};
    checkMemOperandPairing(lir, schema, verdict);
    checkTerminatorBlockRefsMatchSuccessors(lir, schema, verdict);
    checkSideStructureIntegrity(lir, verdict);
    checkAsmRegionOpcodes(lir, schema, verdict);
    checkStoreRegClassMatchesMirType(lir, mir, interner, schema, lirToMirMap, verdict);
    // ⚠ `checkVregClassMatchesMirType` (Rule 3) is ABSENT ON PURPOSE — it is
    // measured false about the shipped lowering (see the long note over it)
    // and reds 31 of 594 `examples/`. It is reachable through
    // `verifyLirVregClassesAgainstMir`; re-adding it here without first
    // publishing a one-to-one MIR→LIR value map re-breaks every float-bearing
    // compile. Rules 2 and 4 below use the SAME map and are measured CLEAN
    // across all 594, so the map is not uniformly unusable — only this rule's
    // one-to-one assumption is wrong.
    checkIntrinsicCallResultValidity(lir, mir, interner, schema, lirToMirMap, verdict);
    return {verdict.clean(baseline)};
}

bool verifyLirVregClassesAgainstMir(Lir const&                 lir,
                                    Mir const&                 mir,
                                    TypeInterner const&        interner,
                                    TargetSchema const&        schema,
                                    std::span<MirInstId const> lirToMirMap,
                                    DiagnosticReporter&        reporter) {
    auto const baseline = reporter.errorCount();
    LirVerdict verdict{reporter};
    checkVregClassMatchesMirType(lir, mir, interner, schema, lirToMirMap, verdict);
    return verdict.clean(baseline);
}

// ── post-regalloc verifier ─────────────────────────────────────────

bool verifyLirPostRegalloc(Lir const& lir, TargetSchema const& schema,
                           DiagnosticReporter& reporter) {
    auto const baseline = reporter.errorCount();
    LirVerdict verdict{reporter};
    auto const frameLoadOp  = schema.opcodeByMnemonic(schema.frameLoadMnemonic());
    auto const frameStoreOp = schema.opcodeByMnemonic(schema.frameStoreMnemonic());
    auto checkPhys = [&](LirReg r, char const* what, std::uint32_t instV) {
        if (r.valid() && r.isPhysical == 0) {
            report(verdict,
                   std::format("verifyLirPostRegalloc: inst {} has a "
                               "virtual {} reg (vreg id {})",
                               instV, what, static_cast<std::uint32_t>(r.id)),
                   DiagnosticCode::L_VirtualRegInPostRegalloc);
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
                    report(verdict,
                           std::format(
                               "verifyLirPostRegalloc: inst {} (frame_load/frame_store) "
                               "has payload 0 — invalid LirSpillSlot sentinel", inst.v),
                           DiagnosticCode::L_InvalidSpillSlotSentinel);
                }
            }
        }
    }
    // ★ The post-regalloc module is the output of the LARGEST rebuild in
    // the pipeline (`rewriteWithAllocation`), so this is the checkpoint
    // where a dropped side-structure reference is both most likely and
    // most expensive — everything downstream of here turns into bytes.
    checkSideStructureIntegrity(lir, verdict);
    checkAsmRegionOpcodes(lir, schema, verdict);
    // ★★★ D-OPT-JCC-FALLTHROUGH — THE PIN, AND THIS IS THE CHECKPOINT THAT
    // MATTERS FOR IT. `lir_peephole`'s R2 elides a branch's trailing
    // fallthrough operand on the strength of a LAYOUT claim, and this
    // verifier runs on the two modules downstream of that claim — the
    // peephole's own output AND `materializeCallingConvention`'s. Rule 1b
    // re-derives "is the unnamed successor really the next-laid-out block?"
    // from the module in hand, so callconv preserving block order 1:1 is
    // CHECKED rather than believed, and any future pass that reorders blocks
    // after the peephole fails the build instead of shipping a branch that
    // falls into the wrong one.
    checkTerminatorBlockRefsMatchSuccessors(lir, schema, verdict);
    return verdict.clean(baseline);
}

// ── text-load verifier (ML8 cycle 2) ─────────────────────────────────

bool verifyLirText(Lir const& lir, TargetSchema const& schema,
                   DiagnosticReporter& reporter) {
    auto const baseline = reporter.errorCount();
    LirVerdict verdict{reporter};
    // The LIR-only rules; future LIR-only rules added to `verifyLir` should
    // join here too. The text-load path has no MIR cross-reference (the source
    // MIR isn't part of `.dsslir`), so MIR-dependent rules (2–4) are
    // deliberately not invoked.
    // ★ Rule 1b matters MOST on this path (D-LIR-TEXT-CONDBR-BLOCKREF-OPERANDS-DROPPED):
    // the text reader is the one producer that ever built a
    // terminator whose two CFG channels disagreed, and it did so silently with
    // `ok == true`. A rule that runs only where the bug cannot occur is not a
    // net.
    checkMemOperandPairing(lir, schema, verdict);
    checkTerminatorBlockRefsMatchSuccessors(lir, schema, verdict);
    // ★ Rule 1c matters on this path for the same reason 1b does: the text
    // reader is a PRODUCER of `regConstraints` handles and of literal-pool
    // entries, and it writes the two halves from two different sections of
    // the file. A `reg_constraints` block whose entries no instruction
    // names is exactly what a hand-edited or truncated `.dsslir` looks
    // like.
    checkSideStructureIntegrity(lir, verdict);
    checkAsmRegionOpcodes(lir, schema, verdict);
    return verdict.clean(baseline);
}

// ── paired rebuild verifier ──────────────────────────────────────────

namespace {
// The asm-region pool's treatment in a paired check: CARRIED by every rebuild
// but one, CONSUMED by `expandAsmRegions`.
enum class AsmRegionCarry : std::uint8_t { Carried, Consumed };

void checkRebuildPair(Lir const& before, Lir const& after,
                      std::string_view passName, AsmRegionCarry regions,
                      LirVerdict& verdict) {

    // (1) A pool that SHRANK is a forgotten `copyModuleSideStructures`.
    // Checked first because it EXPLAINS the dangling references that
    // follow from it — reporting those first would bury the cause under
    // its own symptoms.
    auto poolShrank = [&](char const* what, std::size_t b, std::size_t a) {
        if (a >= b) return;
        report(verdict, std::format(
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
    // D-C-GNU-CONSTRUCTOR-ATTRIBUTE-IS-WARNED-AND-IGNORED-NOT-RUN: the third
    // side structure gets the same shrink guard, and it NEEDS one more than
    // the pools do. A dropped literal leaves a dangling INDEX that check (2)
    // catches downstream; a dropped schedule entry leaves nothing dangling at
    // all — the module verifies clean, links clean, runs, and simply never
    // calls the initializer. This is the only instrument that can see it.
    poolShrank("static-initializer schedule",
               before.staticInitSchedule().size(),
               after.staticInitSchedule().size());
    if (regions == AsmRegionCarry::Carried) {
        poolShrank("asm-region pool", before.asmRegionPool().size(),
                   after.asmRegionPool().size());
    } else if (after.asmRegionPool().size() != 0) {
        report(verdict, std::format(
            "verifyLirRebuild: pass '{}' consumes the asm-region pool, yet its "
            "output still holds {} region(s) — a consumed structure that "
            "survives is a body some later pass could still attach to an "
            "instruction",
            passName, after.asmRegionPool().size()),
            DiagnosticCode::L_AsmRegionMalformed);
    }

    // (2) The module-local rules on the OUTPUT — dangling indices, and
    // constraint entries nothing references.
    checkSideStructureIntegrity(after, verdict);

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
        report(verdict, std::format(
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
    if (regions == AsmRegionCarry::Carried) {
        refsLost("asm-region", b.regionRefs, a.regionRefs);
    } else if (a.regionRefs != 0) {
        report(verdict, std::format(
            "verifyLirRebuild: pass '{}' consumes the asm-region pool, yet {} "
            "instruction(s) of its output still carry a region handle — an "
            "inline-asm bundle survived its own expansion and would reach the "
            "encoder, which has no bytes for it",
            passName, a.regionRefs),
            DiagnosticCode::L_AsmRegionMalformed);
    }
}
} // namespace

bool verifyLirRebuild(Lir const& before, Lir const& after,
                      std::string_view passName,
                      DiagnosticReporter& reporter) {
    auto const baseline = reporter.errorCount();
    LirVerdict verdict{reporter};
    checkRebuildPair(before, after, passName, AsmRegionCarry::Carried, verdict);
    return verdict.clean(baseline);
}

bool verifyLirAsmRegionExpansion(Lir const& before, Lir const& after,
                                 TargetSchema const& schema,
                                 DiagnosticReporter& reporter) {
    auto const baseline = reporter.errorCount();
    LirVerdict verdict{reporter};
    checkRebuildPair(before, after, "asm-region-expansion",
                     AsmRegionCarry::Consumed, verdict);
    // No instruction of the output is the bundle opcode either (a bundle that
    // lost its handle on the way out would pass the handle count above).
    checkAsmRegionOpcodes(after, schema, verdict);
    return verdict.clean(baseline);
}

} // namespace dss
