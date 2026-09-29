#include "lir/lir_asm_region.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_pass_util.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dss {

char const* lirAsmRoleName(LirAsmOperandRole r) noexcept {
    switch (r) {
        case LirAsmOperandRole::Use:      return "use";
        case LirAsmOperandRole::Def:      return "def";
        case LirAsmOperandRole::UseDef:   return "use-def";
        case LirAsmOperandRole::EarlyDef: return "early-def";
    }
    return "?";
}

namespace {

// The index of `blk` among the body function's blocks, or nullopt when it is
// not one of them.
[[nodiscard]] std::optional<std::uint32_t>
bodyBlockIndex(Lir const& body, LirBlockId blk) {
    if (body.moduleFuncCount() != 1) return std::nullopt;
    LirFuncId const fn = body.funcAt(0);
    std::uint32_t const n = body.funcBlockCount(fn);
    if (n == 0 || blk.arenaTag != body.id().v) return std::nullopt;
    std::uint32_t const first = body.funcBlockAt(fn, 0).v;
    if (blk.v < first || blk.v >= first + n) return std::nullopt;
    return blk.v - first;
}

// The slot whose body register is `r`, or nullopt.
[[nodiscard]] std::optional<std::size_t>
slotOf(LirAsmRegion const& region, LirReg r) {
    for (std::size_t k = 0; k < region.bodyRegs.size(); ++k) {
        if (region.bodyRegs[k] == r) return k;
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string> lirAsmRegionShapeDefect(LirAsmRegion const& r) {
    if (r.roles.size() != r.bodyRegs.size()) {
        return std::format("it declares {} role(s) but {} body register(s) — "
                           "one of each per slot", r.roles.size(),
                           r.bodyRegs.size());
    }
    for (std::size_t k = 0; k < r.bodyRegs.size(); ++k) {
        LirReg const reg = r.bodyRegs[k];
        if (!reg.valid() || reg.isPhysical != 0) {
            return std::format("slot {}'s body register is not a virtual "
                               "register — a slot stands for a register the "
                               "allocator assigns", k);
        }
        for (std::size_t j = 0; j < k; ++j) {
            if (r.bodyRegs[j] == reg) {
                return std::format("slots {} and {} name the same body register "
                                   "v{} — one register is one slot, whatever "
                                   "roles it plays", j, k,
                                   static_cast<std::uint32_t>(reg.id));
            }
        }
    }
    if (r.body.moduleFuncCount() != 1) {
        return std::format("its body holds {} function(s); a statement's body is "
                           "exactly one", r.body.moduleFuncCount());
    }
    if (r.body.literalPool().size() != 0 || r.body.regConstraintPool().size() != 0
        || !r.body.staticInitSchedule().empty() || r.body.asmRegionPool().size() != 0) {
        return std::string{"its body carries a module side structure of its own "
                           "(a literal, a constraint set, an initializer or a "
                           "region) — the expansion copies instructions, not "
                           "side structures, so it would dangle"};
    }
    LirFuncId const fn = r.body.funcAt(0);
    std::uint32_t const nb = r.body.funcBlockCount(fn);
    if (r.syntheticFallthrough.size() != nb) {
        return std::format("it marks {} block(s) for fall-through but its body "
                           "has {}", r.syntheticFallthrough.size(), nb);
    }
    auto const exitIdx = bodyBlockIndex(r.body, r.exit);
    if (!exitIdx.has_value()) {
        return std::string{"its exit is not a block of its body"};
    }
    if (*exitIdx == 0) {
        return std::string{"its exit is the body's entry — the template would "
                           "have no first line"};
    }
    std::vector<std::uint32_t> stubIdx;
    for (std::size_t j = 0; j < r.gotoTargets.size(); ++j) {
        auto const idx = bodyBlockIndex(r.body, r.gotoTargets[j]);
        if (!idx.has_value()) {
            return std::format("its `asm goto` target {} is not a block of its "
                               "body", j);
        }
        if (*idx == 0 || *idx == *exitIdx
            || std::find(stubIdx.begin(), stubIdx.end(), *idx) != stubIdx.end()) {
            return std::format("its `asm goto` target {} is the entry, the exit "
                               "or another target — each edge out of the "
                               "statement is its own block", j);
        }
        stubIdx.push_back(*idx);
    }
    for (std::uint32_t i = 0; i < nb; ++i) {
        LirBlockId const blk = r.body.funcBlockAt(fn, i);
        if (r.syntheticFallthrough[i] == 0) continue;
        auto const succs = r.body.blockSuccessors(blk);
        auto const ops   = r.body.instOperands(r.body.blockTerminator(blk));
        if (succs.size() != 1 || ops.size() != 1
            || ops[0].kind != LirOperandKind::BlockRef
            || ops[0].blockSlot != succs[0].v) {
            return std::format("body block {} is marked as falling into the next "
                               "line, but its terminator is not a branch to "
                               "exactly one block", i);
        }
    }
    // Every VIRTUAL register the body names is a slot's.
    for (std::uint32_t i = 0; i < nb; ++i) {
        LirBlockId const blk = r.body.funcBlockAt(fn, i);
        std::uint32_t const n = r.body.blockInstCount(blk);
        for (std::uint32_t k = 0; k < n; ++k) {
            LirInstId const inst = r.body.blockInstAt(blk, k);
            std::optional<LirReg> stray;
            auto const check = [&](LirReg reg) {
                if (stray.has_value() || !reg.valid() || reg.isPhysical != 0) return;
                if (!slotOf(r, reg).has_value()) stray = reg;
            };
            check(r.body.instResult(inst));
            for (auto const& o : r.body.instOperands(inst)) {
                if (o.kind == LirOperandKind::Reg) check(o.reg);
            }
            if (stray.has_value()) {
                return std::format("body instruction {} names virtual register "
                                   "v{}, which is no slot's — the expansion would "
                                   "have no physical register to put there",
                                   inst.v,
                                   static_cast<std::uint32_t>(stray->id));
            }
        }
    }
    return std::nullopt;
}

std::vector<std::uint8_t>
lirAsmRegionSlotAccessWidthBits(LirAsmRegion const& region) {
    std::vector<std::uint8_t> widest(region.bodyRegs.size(), 0);
    if (region.body.moduleFuncCount() != 1) return widest;
    LirFuncId const fn = region.body.funcAt(0);
    std::uint32_t const nb = region.body.funcBlockCount(fn);
    for (std::uint32_t i = 0; i < nb; ++i) {
        LirBlockId const blk = region.body.funcBlockAt(fn, i);
        std::uint32_t const n = region.body.blockInstCount(blk);
        for (std::uint32_t k = 0; k < n; ++k) {
            LirInstId const inst = region.body.blockInstAt(blk, k);
            std::uint8_t const bits =
                lirInstWidthBits(region.body.instFlags(inst));
            auto const note = [&](LirReg reg) {
                if (!reg.valid() || reg.isPhysical != 0) return;
                if (auto const s = slotOf(region, reg); s.has_value()) {
                    if (bits > widest[*s]) widest[*s] = bits;
                }
            };
            note(region.body.instResult(inst));
            for (auto const& o : region.body.instOperands(inst)) {
                if (o.kind == LirOperandKind::Reg) note(o.reg);
            }
        }
    }
    return widest;
}

// ── the expansion ───────────────────────────────────────────────────────────

namespace {

// Where one bundle's body goes in the destination function, decided before a
// single instruction is emitted — the destination's block ORDER is its creation
// order, so every block of a function must be created in layout order first.
struct BundlePlan {
    LirInstId           inst{};
    LirAsmRegion const* region = nullptr;
    bool                isTerminator = false;
    // The body blocks that are EMITTED (not `exit`, not a goto stub), in the
    // order the template BEGAN them — its textual order.
    std::vector<LirBlockId> layout;
    // Per layout index: 1 = continues in the previous block's destination (it
    // is reached only by the template falling into it).
    std::vector<std::uint8_t> merges;
    // Non-goto only: the rest of the source block continues in the LAST layout
    // block's destination (the template falls off its end, and only there).
    bool exitMerges = false;
    // Body block → destination block, for every emitted body block.
    std::unordered_map<std::uint32_t, LirBlockId> bodyToDst;
    // Non-goto only: the destination block the rest of the source block goes
    // into (== the last layout block's destination when `exitMerges`).
    LirBlockId continuation{};
};

class RegionExpander {
public:
    RegionExpander(Lir const& src, TargetSchema const& schema,
                   DiagnosticReporter& reporter, LirBuilder& b)
        : src_(src), schema_(schema), reporter_(reporter), b_(b) {}

    [[nodiscard]] bool expandFunction(LirFuncId fn, std::size_t& expanded);

private:
    [[nodiscard]] bool planBundle(LirInstId inst, LirAsmRegion const& region,
                                  LirBlockId& currentPiece, BundlePlan& out);
    [[nodiscard]] bool emitBundle(BundlePlan const& plan, LirBlockId srcBlock,
                                  std::unordered_map<std::uint32_t, LirBlockId> const& srcToDst,
                                  std::optional<LirBlockId> nextAfterBlock);
    [[nodiscard]] bool fail(std::string message) {
        report(reporter_, DiagnosticCode::L_AsmRegionMalformed,
               DiagnosticSeverity::Error, std::move(message));
        return false;
    }

    Lir const&          src_;
    TargetSchema const& schema_;
    DiagnosticReporter& reporter_;
    LirBuilder&         b_;
};

bool RegionExpander::planBundle(LirInstId inst, LirAsmRegion const& region,
                                LirBlockId& currentPiece, BundlePlan& out) {
    out.inst   = inst;
    out.region = &region;
    auto const* info = schema_.opcodeInfo(src_.instOpcode(inst));
    out.isTerminator = info != nullptr && info->isTerminator();

    Lir const& body = region.body;
    LirFuncId const bfn = body.funcAt(0);
    std::uint32_t const nb = body.funcBlockCount(bfn);
    auto const isExitOrStub = [&](LirBlockId blk) {
        if (blk.v == region.exit.v) return true;
        for (LirBlockId const s : region.gotoTargets) {
            if (s.v == blk.v) return true;
        }
        return false;
    };
    for (std::uint32_t i = 0; i < nb; ++i) {
        LirBlockId const blk = body.funcBlockAt(bfn, i);
        if (!isExitOrStub(blk)) out.layout.push_back(blk);
    }
    // TEXTUAL order: the order the template BEGAN its blocks, which is the
    // order their first instructions were appended (a block is begun when its
    // label is reached; it may have been CREATED earlier, by a forward branch).
    std::stable_sort(out.layout.begin(), out.layout.end(),
                     [&](LirBlockId a, LirBlockId c) {
                         return body.blockInstAt(a, 0).v < body.blockInstAt(c, 0).v;
                     });
    if (out.layout.empty() || out.layout.front().v != body.funcEntry(bfn).v) {
        return fail(std::format(
            "asm-region bundle inst {}: the body's entry is not the first block "
            "the template began — the statement would not start at its first "
            "line", inst.v));
    }
    // Predecessor counts over the EMITTED blocks' edges.
    std::unordered_map<std::uint32_t, std::uint32_t> preds;
    for (LirBlockId const blk : out.layout) {
        for (LirBlockId const s : body.blockSuccessors(blk)) ++preds[s.v];
    }
    auto const synthetic = [&](LirBlockId blk) {
        auto const idx = bodyBlockIndex(body, blk);
        return idx.has_value() && region.syntheticFallthrough[*idx] != 0;
    };
    auto const soleSuccessorIs = [&](LirBlockId blk, LirBlockId target) {
        auto const succs = body.blockSuccessors(blk);
        return succs.size() == 1 && succs[0].v == target.v;
    };
    out.merges.assign(out.layout.size(), 0);
    for (std::size_t i = 1; i < out.layout.size(); ++i) {
        LirBlockId const prev = out.layout[i - 1];
        LirBlockId const cur  = out.layout[i];
        if (synthetic(prev) && soleSuccessorIs(prev, cur) && preds[cur.v] == 1) {
            out.merges[i] = 1;
        }
    }
    if (!out.isTerminator) {
        LirBlockId const last = out.layout.back();
        out.exitMerges = synthetic(last) && soleSuccessorIs(last, region.exit)
                      && preds[region.exit.v] == 1;
    }
    // Destination blocks, created in layout order.
    out.bodyToDst[out.layout[0].v] = currentPiece;
    for (std::size_t i = 1; i < out.layout.size(); ++i) {
        out.bodyToDst[out.layout[i].v] =
            out.merges[i] != 0 ? out.bodyToDst.at(out.layout[i - 1].v)
                               : b_.createBlock();
    }
    if (!out.isTerminator) {
        out.continuation = out.exitMerges ? out.bodyToDst.at(out.layout.back().v)
                                          : b_.createBlock();
        currentPiece = out.continuation;
    }
    return true;
}

bool RegionExpander::emitBundle(
        BundlePlan const& plan, LirBlockId srcBlock,
        std::unordered_map<std::uint32_t, LirBlockId> const& srcToDst,
        std::optional<LirBlockId> nextAfterBlock) {
    LirAsmRegion const& region = *plan.region;
    Lir const& body = region.body;
    auto const bundleOps = src_.instOperands(plan.inst);
    std::uint32_t const constraintHandle =
        src_.instRegConstraintHandle(plan.inst);

    // Every body block an edge may name → its destination.
    std::unordered_map<std::uint32_t, LirBlockId> bodyMap = plan.bodyToDst;
    if (plan.isTerminator) {
        auto const succs = src_.blockSuccessors(srcBlock);
        if (succs.size() != region.gotoTargets.size() + 1) {
            return fail(std::format(
                "`asm goto` bundle inst {} has {} CFG successor(s) but its region "
                "declares {} label target(s) — the layout is [labels…, "
                "fall-through]", plan.inst.v, succs.size(),
                region.gotoTargets.size()));
        }
        for (std::size_t j = 0; j < region.gotoTargets.size(); ++j) {
            bodyMap[region.gotoTargets[j].v] = srcToDst.at(succs[j].v);
        }
        bodyMap[region.exit.v] = srcToDst.at(succs.back().v);
    } else {
        bodyMap[region.exit.v] = plan.continuation;
    }

    // Substitute a body register: a slot's virtual register becomes the
    // physical register the allocator gave the bundle's operand in that slot.
    auto const substitute = [&](LirReg r, LirReg& out) -> bool {
        if (!r.valid() || r.isPhysical != 0) {
            out = r;
            return true;
        }
        auto const k = slotOf(region, r);
        if (!k.has_value()) {
            return fail(std::format(
                "asm-region bundle inst {}: its body names v{}, which is no "
                "slot's register", plan.inst.v,
                static_cast<std::uint32_t>(r.id)));
        }
        if (*k >= bundleOps.size() || bundleOps[*k].kind != LirOperandKind::Reg
            || !bundleOps[*k].reg.valid() || bundleOps[*k].reg.isPhysical == 0) {
            return fail(std::format(
                "asm-region bundle inst {}: operand {} ({} slot) is not a "
                "physical register — the expansion must run AFTER register "
                "allocation, when every slot has its register for the whole "
                "statement", plan.inst.v, *k, lirAsmRoleName(region.roles[*k])));
        }
        out = bundleOps[*k].reg;
        return true;
    };
    auto const stamp = [&](LirInstId dst) {
        if (constraintHandle == kLirNoRegConstraints) return;
        b_.setInstRegConstraints(dst,
                                 lirRegConstraintIndexForHandle(constraintHandle));
    };
    auto const synthetic = [&](LirBlockId blk) {
        auto const idx = bodyBlockIndex(body, blk);
        return idx.has_value() && region.syntheticFallthrough[*idx] != 0;
    };

    for (std::size_t li = 0; li < plan.layout.size(); ++li) {
        LirBlockId const blk = plan.layout[li];
        if (li > 0 && plan.merges[li] == 0) b_.beginBlock(plan.bodyToDst.at(blk.v));
        std::uint32_t const n = body.blockInstCount(blk);
        for (std::uint32_t k = 0; k + 1 < n; ++k) {
            LirInstId const bi = body.blockInstAt(blk, k);
            LirReg result = InvalidLirReg;
            if (!substitute(body.instResult(bi), result)) return false;
            std::vector<LirOperand> ops;
            for (auto const& o : body.instOperands(bi)) {
                LirOperand c = lir_pass_util::remapBlockRef(o, bodyMap);
                if (c.kind == LirOperandKind::Reg && !substitute(o.reg, c.reg)) {
                    return false;
                }
                ops.push_back(c);
            }
            LirInstId const dst = b_.addInst(body.instOpcode(bi), result, ops,
                                             body.instPayload(bi),
                                             body.instFlags(bi));
            stamp(dst);
        }
        // The terminator.
        LirInstId const term = body.blockTerminator(blk);
        auto const termSuccs = body.blockSuccessors(blk);
        bool const lastInLayout = li + 1 == plan.layout.size();
        if (synthetic(blk) && termSuccs.size() == 1) {
            LirBlockId const target = termSuccs[0];
            // Falls into the next line, which continues in this destination
            // block: nothing to emit.
            if (!lastInLayout && plan.merges[li + 1] != 0
                && plan.layout[li + 1].v == target.v) {
                continue;
            }
            // Falls off the end of the template into the rest of the source
            // block, which continues here.
            if (lastInLayout && plan.exitMerges && target.v == region.exit.v) {
                continue;
            }
        }
        std::uint16_t const op = body.instOpcode(term);
        auto const* info = schema_.opcodeInfo(op);
        std::vector<LirOperand> ops;
        for (auto const& o : body.instOperands(term)) {
            LirOperand c = lir_pass_util::remapBlockRef(o, bodyMap);
            if (c.kind == LirOperandKind::Reg && !substitute(o.reg, c.reg)) {
                return false;
            }
            ops.push_back(c);
        }
        // R2, re-asked for the template's own blocks: the peephole ran before
        // they existed. The destination block laid out next is the next layout
        // block's, or — after the last — the continuation (non-goto) or the
        // source block laid out after this one (`asm goto`).
        std::optional<LirBlockId> nextDst;
        if (!lastInLayout) {
            nextDst = plan.bodyToDst.at(plan.layout[li + 1].v);
        } else if (!plan.isTerminator) {
            nextDst = plan.continuation;
        } else {
            nextDst = nextAfterBlock;
        }
        std::vector<LirBlockId> dstSuccs;
        dstSuccs.reserve(termSuccs.size());
        for (LirBlockId const s : termSuccs) dstSuccs.push_back(bodyMap.at(s.v));
        if (lir_pass_util::canElideFallthroughOperand(schema_, op, ops, dstSuccs,
                                                      nextDst)) {
            ops.pop_back();
        }
        if (!lir_pass_util::emitTerminator(b_, op, info, termSuccs, ops,
                                           body.instPayload(term),
                                           body.instFlags(term), bodyMap,
                                           "asm-region-expansion", reporter_)) {
            return false;
        }
        stamp(b_.lastInst());
    }
    if (!plan.isTerminator && !plan.exitMerges) b_.beginBlock(plan.continuation);
    return true;
}

bool RegionExpander::expandFunction(LirFuncId fn, std::size_t& expanded) {
    (void)b_.addFunction(src_.funcSymbol(fn));
    std::uint32_t const blockCount = src_.funcBlockCount(fn);
    std::unordered_map<std::uint32_t, LirBlockId> srcToDst;
    srcToDst.reserve(blockCount);
    std::vector<BundlePlan> plans;

    // Plan: every destination block, in layout order.
    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        LirBlockId const srcBlk = src_.funcBlockAt(fn, bi);
        LirBlockId currentPiece = b_.createBlock();
        srcToDst[srcBlk.v] = currentPiece;
        std::uint32_t const n = src_.blockInstCount(srcBlk);
        for (std::uint32_t i = 0; i < n; ++i) {
            LirInstId const inst = src_.blockInstAt(srcBlk, i);
            LirAsmRegion const* region = src_.instAsmRegion(inst);
            if (region == nullptr) continue;
            if (auto const defect = lirAsmRegionShapeDefect(*region)) {
                return fail(std::format("asm-region bundle inst {}: its region is "
                                        "malformed: {}", inst.v, *defect));
            }
            if (src_.instOperands(inst).size() != region->roles.size()) {
                return fail(std::format(
                    "asm-region bundle inst {} has {} operand(s) for {} slot(s)",
                    inst.v, src_.instOperands(inst).size(),
                    region->roles.size()));
            }
            BundlePlan plan;
            if (!planBundle(inst, *region, currentPiece, plan)) return false;
            plans.push_back(std::move(plan));
        }
    }

    // Emit.
    std::size_t planIdx = 0;
    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        LirBlockId const srcBlk = src_.funcBlockAt(fn, bi);
        std::optional<LirBlockId> const nextAfterBlock =
            bi + 1 < blockCount
                ? std::optional<LirBlockId>{srcToDst.at(
                      src_.funcBlockAt(fn, bi + 1).v)}
                : std::nullopt;
        b_.beginBlock(srcToDst.at(srcBlk.v));
        std::uint32_t const n = src_.blockInstCount(srcBlk);
        for (std::uint32_t i = 0; i < n; ++i) {
            LirInstId const inst = src_.blockInstAt(srcBlk, i);
            if (src_.instAsmRegion(inst) != nullptr) {
                if (!emitBundle(plans[planIdx++], srcBlk, srcToDst,
                                nextAfterBlock)) {
                    return false;
                }
                ++expanded;
                continue;
            }
            std::uint16_t const op = src_.instOpcode(inst);
            auto const* info = schema_.opcodeInfo(op);
            std::vector<LirOperand> ops;
            for (auto const& o : src_.instOperands(inst)) {
                ops.push_back(lir_pass_util::remapBlockRef(o, srcToDst));
            }
            if (info != nullptr && info->isTerminator()) {
                if (!lir_pass_util::emitTerminator(
                        b_, op, info, src_.blockSuccessors(srcBlk), ops,
                        src_.instPayload(inst), src_.instFlags(inst), srcToDst,
                        "asm-region-expansion", reporter_)) {
                    return false;
                }
                lir_pass_util::carryInstSideData(src_, inst, b_);
            } else {
                LirInstId const dst =
                    b_.addInst(op, src_.instResult(inst), ops,
                               src_.instPayload(inst), src_.instFlags(inst));
                lir_pass_util::carryInstSideData(src_, inst, b_, dst);
            }
        }
    }
    return true;
}

} // namespace

LirAsmRegionExpansionResult
expandAsmRegions(Lir const& src, TargetSchema const& schema,
                 DiagnosticReporter& reporter) {
    LirAsmRegionExpansionResult result;
    if (src.moduleFuncCount() == 0) {
        result.ok = true;
        return result;
    }
    LirBuilder b{schema};
    // ★ THE ONE PASS THAT CONSUMES A SIDE STRUCTURE: every other structure is
    // carried exactly as any rebuild carries it; the region pool is not — each
    // bundle becomes its body here, so the output references no region.
    lir_pass_util::copyModuleSideStructuresConsumingAsmRegions(src, b);
    RegionExpander expander{src, schema, reporter, b};
    std::size_t const funcCount = src.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < funcCount; ++fi) {
        if (!expander.expandFunction(src.funcAt(fi), result.regionsExpanded)) {
            b.poison();
            return result;
        }
    }
    result.lir = std::move(b).finish();
    result.ok  = result.lir.moduleFuncCount() == funcCount;
    return result;
}

} // namespace dss
