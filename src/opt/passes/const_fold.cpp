#include "opt/passes/const_fold.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "hir/const_eval.hpp"
#include "hir/const_eval_arith.hpp"
#include "hir/hir_literal_pool.hpp"
#include "hir/hir_op.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"

#include <cstdlib>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dss::opt::passes {

namespace {

// ── MirLiteralValue ↔ HirLiteralValue bridge ───────────────────────────
// The two structs are field-identical (same variant arms in the same
// order, same TypeKind core). The aggregate arm recurses.
[[nodiscard]] HirLiteralValue toHirLiteral(MirLiteralValue const& src);
[[nodiscard]] MirLiteralValue toMirLiteral(HirLiteralValue const& src);

HirLiteralValue toHirLiteral(MirLiteralValue const& src) {
    HirLiteralValue dst;
    dst.core = src.core;
    std::visit([&](auto const& arm) {
        using T = std::decay_t<decltype(arm)>;
        if constexpr (std::is_same_v<T, MirAggregateValue>) {
            HirAggregateValue agg;
            agg.fields.reserve(arm.fields.size());
            for (auto const& f : arm.fields) agg.fields.push_back(toHirLiteral(f));
            dst.value = std::move(agg);
        } else {
            dst.value = arm;
        }
    }, src.value);
    return dst;
}

MirLiteralValue toMirLiteral(HirLiteralValue const& src) {
    MirLiteralValue dst;
    dst.core = src.core;
    std::visit([&](auto const& arm) {
        using T = std::decay_t<decltype(arm)>;
        if constexpr (std::is_same_v<T, HirAggregateValue>) {
            MirAggregateValue agg;
            agg.fields.reserve(arm.fields.size());
            for (auto const& f : arm.fields) agg.fields.push_back(toMirLiteral(f));
            dst.value = std::move(agg);
        } else {
            dst.value = arm;
        }
    }, src.value);
    return dst;
}

// ── MirOpcode → HirOpKind translation ──────────────────────────────────
[[nodiscard]] std::optional<HirOpKind> binaryOpKind(MirOpcode op) noexcept {
    switch (op) {
        case MirOpcode::Add:     return HirOpKind::Add;
        case MirOpcode::Sub:     return HirOpKind::Sub;
        case MirOpcode::Mul:     return HirOpKind::Mul;
        case MirOpcode::SDiv:    return HirOpKind::Div;
        case MirOpcode::SMod:    return HirOpKind::Rem;
        case MirOpcode::And:     return HirOpKind::BitAnd;
        case MirOpcode::Or:      return HirOpKind::BitOr;
        case MirOpcode::Xor:     return HirOpKind::BitXor;
        case MirOpcode::Shl:     return HirOpKind::Shl;
        case MirOpcode::AShr:    return HirOpKind::Shr;
        case MirOpcode::ICmpEq:  return HirOpKind::Eq;
        case MirOpcode::ICmpNe:  return HirOpKind::Ne;
        case MirOpcode::ICmpSlt: return HirOpKind::Lt;
        case MirOpcode::ICmpSle: return HirOpKind::Le;
        case MirOpcode::ICmpSgt: return HirOpKind::Gt;
        case MirOpcode::ICmpSge: return HirOpKind::Ge;
        default:                 return std::nullopt;
    }
}

[[nodiscard]] std::optional<HirOpKind> unaryOpKind(MirOpcode op) noexcept {
    switch (op) {
        case MirOpcode::Neg: return HirOpKind::Neg;
        case MirOpcode::Not: return HirOpKind::BitNot;
        default:             return std::nullopt;
    }
}

// True iff the opcode is one of the unsigned-semantic arms whose folding
// must route through uint64 rather than int64 (the default CE path).
[[nodiscard]] bool isUnsignedOp(MirOpcode op) noexcept {
    switch (op) {
        case MirOpcode::UDiv:
        case MirOpcode::UMod:
        case MirOpcode::LShr:
        case MirOpcode::ICmpUlt:
        case MirOpcode::ICmpUle:
        case MirOpcode::ICmpUgt:
        case MirOpcode::ICmpUge:
            return true;
        default:
            return false;
    }
}

// Per-function rebuilder. Rewrite map is slot-keyed (.v) because old +
// new arenas carry different module tags.
class FunctionRebuilder {
public:
    FunctionRebuilder(Mir const& src, MirBuilder& dst, TypeInterner const& interner)
        : src_(src), dst_(dst), interner_(interner) {}

    [[nodiscard]] std::size_t instructionsFolded() const noexcept { return folded_; }

    void rebuildFunction(MirFuncId oldFn) {
        dst_.addFunction(src_.funcSignature(oldFn), src_.funcSymbol(oldFn),
                         src_.funcBinding(oldFn), src_.funcVisibility(oldFn));

        std::uint32_t const blockCount = src_.funcBlockCount(oldFn);

        // Phase 1: pre-create every block so terminators may target
        // forward references (loop back-edges).
        std::unordered_map<std::uint32_t, MirBlockId> blockMap;
        blockMap.reserve(blockCount);
        std::vector<MirBlockId> oldBlocks;
        oldBlocks.reserve(blockCount);
        for (std::uint32_t i = 0; i < blockCount; ++i) {
            MirBlockId const oldB = src_.funcBlockAt(oldFn, i);
            MirBlockId const newB = dst_.createBlock(src_.blockMarker(oldB));
            blockMap.emplace(oldB.v, newB);
            oldBlocks.push_back(oldB);
        }

        // Phase 2: fill each block. Phi incomings are deferred — their
        // values may live in not-yet-rebuilt blocks (back-edges).
        struct DeferredPhi {
            MirInstId oldPhi;
            MirInstId newPhi;
        };
        std::vector<DeferredPhi> deferredPhis;
        rewrite_.clear();
        constCache_.clear();
        rewrite_.reserve(src_.instCount());

        for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
            MirBlockId const oldB = oldBlocks[bi];
            MirBlockId const newB = blockMap.at(oldB.v);
            dst_.beginBlock(newB);

            std::uint32_t const ninst = src_.blockInstCount(oldB);
            for (std::uint32_t ii = 0; ii < ninst; ++ii) {
                MirInstId const oldId = src_.blockInstAt(oldB, ii);
                MirOpcode const op    = src_.instOpcode(oldId);

                if (opcodeInfo(op).isTerminator) {
                    emitTerminator(op, oldId, blockMap);
                    continue;
                }
                if (op == MirOpcode::Phi) {
                    MirInstId const newPhi = dst_.addPhi(src_.instType(oldId));
                    rewrite_.emplace(oldId.v, newPhi);
                    deferredPhis.push_back({oldId, newPhi});
                    continue;
                }
                emitValue(op, oldId);
            }
        }

        // Phase 3: populate phi incomings via the now-complete rewrite map.
        for (auto const& dp : deferredPhis) {
            for (auto const& inc : src_.phiIncomings(dp.oldPhi)) {
                MirInstId const newVal = rewriteOperand(inc.value);
                MirBlockId const newPred = blockMap.at(inc.pred.v);
                dst_.addPhiIncoming(dp.newPhi, MirPhiIncoming{newVal, newPred});
            }
        }
    }

private:
    // Resolve an OLD-module operand to its NEW-module replacement.
    // Every value-producing inst MUST be in the rewrite map by the
    // time a consumer reads it (SSA scan-order invariant). Phi back-
    // edges are the one delayed case — handled in phase 3 only,
    // which calls rewriteOperand AFTER all blocks are filled. A
    // missing entry at terminator/value emit time is a substrate-
    // contract violation. Fail loud rather than silently propagate
    // an invalid id — `MirInstId{}` (arenaTag=0) is admitted by
    // `checkSameModule_` AND skipped by the verifier's two
    // `if (!op.valid()) continue` arms, defeating three safety nets
    // simultaneously (silent-failure CRITICAL post-fold).
    [[nodiscard]] MirInstId rewriteOperand(MirInstId oldOp) const {
        auto const it = rewrite_.find(oldOp.v);
        if (it == rewrite_.end()) {
            std::fprintf(stderr,
                "dss::opt::passes::FunctionRebuilder fatal: rewriteOperand: "
                "old MirInstId v=%u has no rewrite entry — scan-order "
                "violation (D-OPT2-REWRITE-MAP-COMPLETENESS).\n",
                oldOp.v);
            std::abort();
        }
        return it->second;
    }

    void emitValue(MirOpcode op, MirInstId oldId) {
        // Value origins: copy verbatim.
        if (op == MirOpcode::Const) {
            std::uint32_t const idx = src_.constLiteralIndex(oldId);
            MirInstId const newId = dst_.addConst(src_.literalValue(idx),
                                                  src_.instType(oldId));
            rewrite_.emplace(oldId.v, newId);
            return;
        }
        if (op == MirOpcode::Arg) {
            MirInstId const newId = dst_.addArg(src_.argIndex(oldId),
                                                src_.instType(oldId));
            rewrite_.emplace(oldId.v, newId);
            return;
        }
        if (op == MirOpcode::GlobalAddr) {
            MirInstId const newId = dst_.addGlobalAddr(src_.globalAddrSymbol(oldId),
                                                       src_.instType(oldId));
            rewrite_.emplace(oldId.v, newId);
            return;
        }

        // Try to fold. nullopt = not foldable here (or fold would trap);
        // fall through to a verbatim copy.
        if (auto folded = tryFold(op, oldId); folded.has_value()) {
            MirInstId const newId = dst_.addConst(toMirLiteral(*folded),
                                                  src_.instType(oldId));
            rewrite_.emplace(oldId.v, newId);
            constCache_.emplace(oldId.v, std::move(*folded));
            ++folded_;
            return;
        }

        // Verbatim copy with remapped value operands.
        auto const oldOps = src_.instOperands(oldId);
        std::vector<MirInstId> newOps;
        newOps.reserve(oldOps.size());
        for (auto o : oldOps) newOps.push_back(rewriteOperand(o));
        MirInstId const newId = dst_.addInst(op, newOps, src_.instType(oldId),
                                             src_.instPayload(oldId),
                                             src_.instFlags(oldId));
        rewrite_.emplace(oldId.v, newId);
    }

    void emitTerminator(MirOpcode op, MirInstId oldId,
                        std::unordered_map<std::uint32_t, MirBlockId> const& blockMap) {
        auto const oldOps  = src_.instOperands(oldId);
        auto const oldBlk  = src_.instBlock(oldId);
        auto const oldSucc = src_.blockSuccessors(oldBlk);

        switch (op) {
            case MirOpcode::Br: {
                MirInstId const newId = dst_.addBr(blockMap.at(oldSucc[0].v));
                rewrite_.emplace(oldId.v, newId);
                return;
            }
            case MirOpcode::CondBr: {
                MirInstId const cond = rewriteOperand(oldOps[0]);
                MirInstId const newId = dst_.addCondBr(
                    cond, blockMap.at(oldSucc[0].v), blockMap.at(oldSucc[1].v));
                rewrite_.emplace(oldId.v, newId);
                return;
            }
            case MirOpcode::Switch: {
                MirInstId const disc = rewriteOperand(oldOps[0]);
                std::vector<std::pair<MirInstId, MirBlockId>> cases;
                std::size_t const ncases = oldSucc.size() - 1;
                cases.reserve(ncases);
                for (std::size_t i = 0; i < ncases; ++i) {
                    cases.emplace_back(rewriteOperand(oldOps[1 + i]),
                                       blockMap.at(oldSucc[i].v));
                }
                MirInstId const newId = dst_.addSwitch(
                    disc, cases, blockMap.at(oldSucc[ncases].v));
                rewrite_.emplace(oldId.v, newId);
                return;
            }
            case MirOpcode::Return: {
                std::optional<MirInstId> retVal;
                if (!oldOps.empty()) retVal = rewriteOperand(oldOps[0]);
                MirInstId const newId = dst_.addReturn(retVal);
                rewrite_.emplace(oldId.v, newId);
                return;
            }
            case MirOpcode::Unreachable: {
                MirInstId const newId = dst_.addUnreachable();
                rewrite_.emplace(oldId.v, newId);
                return;
            }
            default:
                // A new terminator landed without a clone arm — fail loud.
                std::abort();
        }
    }

    [[nodiscard]] std::optional<HirLiteralValue>
    tryFold(MirOpcode op, MirInstId oldId) {
        auto const oldOps = src_.instOperands(oldId);

        // Unary integer fold.
        if (oldOps.size() == 1 && unaryOpKind(op).has_value()) {
            auto const operand = readConstOperand(oldOps[0]);
            if (!operand.has_value()) return std::nullopt;
            auto folded = detail::applyUnaryInt(*unaryOpKind(op), *operand);
            if (!folded.has_value()) return std::nullopt;
            return normalizeToType(std::move(*folded), src_.instType(oldId), op);
        }

        // Binary integer fold (includes unsigned-routed arms).
        if (oldOps.size() == 2) {
            auto const a = readConstOperand(oldOps[0]);
            auto const b = readConstOperand(oldOps[1]);
            if (!a.has_value() || !b.has_value()) return std::nullopt;

            if (isUnsignedOp(op)) {
                auto folded = foldUnsigned(op, *a, *b);
                if (!folded.has_value()) return std::nullopt;
                return normalizeToType(std::move(*folded), src_.instType(oldId), op);
            }

            auto const bok = binaryOpKind(op);
            if (!bok.has_value()) return std::nullopt;

            // Wrap-on-overflow; refuse div/shift edge cases (we DON'T
            // fold them — fold-to-trap would observably differ from
            // the unoptimized path's deferred runtime trap).
            EvalOptions opts{};
            opts.refuseOnOverflow         = false;
            opts.refuseOnDivByZero        = true;
            opts.refuseOnShiftOutOfRange  = true;
            ConstEvalFailure why = ConstEvalFailure::None;
            auto folded = detail::applyBinaryInt(*bok, *a, *b, opts, why);
            if (!folded.has_value()) return std::nullopt;
            return normalizeToType(std::move(*folded), src_.instType(oldId), op);
        }

        return std::nullopt;
    }

    [[nodiscard]] std::optional<HirLiteralValue>
    readConstOperand(MirInstId oldOp) const {
        // Folded-in-this-rebuild cache (operand was a foldable op whose
        // fold just succeeded earlier in scan order).
        if (auto cit = constCache_.find(oldOp.v); cit != constCache_.end()) {
            return cit->second;
        }
        // Original-Const in source module.
        if (src_.instOpcode(oldOp) == MirOpcode::Const) {
            std::uint32_t const idx = src_.constLiteralIndex(oldOp);
            return toHirLiteral(src_.literalValue(idx));
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<HirLiteralValue>
    normalizeToType(HirLiteralValue folded, TypeId dstType, MirOpcode /*op*/) const {
        TypeKind const dstKind = interner_.kind(dstType);

        // ICmp / FCmp results are signalled by the CE helper's
        // returned `core == Bool` (applyBinaryInt's compare arms +
        // foldUnsigned's compare arms both re-tag the literal).
        // Clamp to canonical 0/1 + leave core=Bool — the dst type
        // is also Bool, no wrap-to-int needed.
        if (folded.core == TypeKind::Bool) {
            auto const v = detail::asInt64(folded);
            folded.value = std::int64_t{v.value_or(0) != 0 ? 1 : 0};
            return folded;
        }

        // Integer arithmetic / bitwise / unary: mask to destination
        // bit width via wrapToIntTarget. Matches the unoptimized
        // runtime's wrap semantics exactly.
        if (auto const ik = detail::intKindInfo(dstKind); ik.has_value()) {
            auto const iv = detail::asInt64(folded);
            if (!iv.has_value()) return std::nullopt;
            folded.core  = dstKind;
            folded.value = detail::wrapToIntTarget(*iv, *ik);
            return folded;
        }

        return std::nullopt;
    }

    [[nodiscard]] std::optional<HirLiteralValue>
    foldUnsigned(MirOpcode op, HirLiteralValue const& a, HirLiteralValue const& b) const {
        auto const av = detail::asInt64(a);
        auto const bv = detail::asInt64(b);
        if (!av.has_value() || !bv.has_value()) return std::nullopt;
        std::uint64_t const ua = static_cast<std::uint64_t>(*av);
        std::uint64_t const ub = static_cast<std::uint64_t>(*bv);
        HirLiteralValue folded;
        folded.core = a.core;
        switch (op) {
            case MirOpcode::UDiv:
                if (ub == 0) return std::nullopt;
                folded.value = static_cast<std::int64_t>(ua / ub);
                return folded;
            case MirOpcode::UMod:
                if (ub == 0) return std::nullopt;
                folded.value = static_cast<std::int64_t>(ua % ub);
                return folded;
            case MirOpcode::LShr:
                if (*bv < 0 || *bv >= 64) return std::nullopt;
                folded.value = static_cast<std::int64_t>(ua >> *bv);
                return folded;
            case MirOpcode::ICmpUlt:
                folded.value = std::int64_t{ua <  ub ? 1 : 0};
                folded.core  = TypeKind::Bool;
                return folded;
            case MirOpcode::ICmpUle:
                folded.value = std::int64_t{ua <= ub ? 1 : 0};
                folded.core  = TypeKind::Bool;
                return folded;
            case MirOpcode::ICmpUgt:
                folded.value = std::int64_t{ua >  ub ? 1 : 0};
                folded.core  = TypeKind::Bool;
                return folded;
            case MirOpcode::ICmpUge:
                folded.value = std::int64_t{ua >= ub ? 1 : 0};
                folded.core  = TypeKind::Bool;
                return folded;
            default:
                return std::nullopt;
        }
    }

    Mir const& src_;
    MirBuilder& dst_;
    TypeInterner const& interner_;
    std::unordered_map<std::uint32_t, MirInstId> rewrite_;
    std::unordered_map<std::uint32_t, HirLiteralValue> constCache_;
    std::size_t folded_ = 0;
};

} // namespace

ConstFoldResult runConstFold(Mir& mir, TypeInterner const& interner,
                             DiagnosticReporter& reporter) {
    ConstFoldResult result{};
    MirBuilder builder;
    FunctionRebuilder rb{mir, builder, interner};

    // Pre-scan for runtime-init globals (D-OPT2-CONST-FOLD-RUNTIME-
    // INIT-GLOBALS). A runtime-init global's `initFunc` is a
    // MirFuncId in the ORIGINAL module's arena; cloning it would
    // require a two-pass func-id remap. Cycle 1 short-circuits with
    // `ok=true, instructionsFolded=0` so the caller keeps the
    // unoptimized MIR. NOT an error path — const-fold has no
    // correctness concern on these modules; just doesn't pay for
    // the rebuild yet. Emit an Info diagnostic (X_OptPassSkipped) so
    // the user / tooling can observe that ConstFold deliberately
    // declined to run on this specific module, distinguishing it
    // from "ran and produced 0 mutations because code was optimal."
    std::size_t const ng = mir.moduleGlobalCount();
    for (std::uint32_t i = 0; i < ng; ++i) {
        if (mir.globalInitFunc(mir.globalAt(i)).valid()) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::X_OptPassSkipped;
            d.severity = DiagnosticSeverity::Info;
            d.actual   = "opt::ConstFold: skipped — module has >= 1 "
                         "runtime-init global; func-id remap not yet "
                         "implemented (D-OPT2-CONST-FOLD-RUNTIME-INIT-"
                         "GLOBALS).";
            reporter.report(std::move(d));
            result.ok = true;
            return result;
        }
    }

    // Clone globals before any function — GlobalAddr value-origins
    // require the global to exist when a function references it.
    // Globals themselves don't fold (the initializer is already a
    // constant literal by definition for this carve-out's complement).
    for (std::uint32_t i = 0; i < ng; ++i) {
        MirGlobalId const g = mir.globalAt(i);
        std::uint32_t const initIdx = mir.globalInitLiteralIndex(g);
        std::uint32_t newInitIdx = UINT32_MAX;
        if (initIdx != UINT32_MAX) {
            newInitIdx = builder.literalPoolAdd(mir.literalValue(initIdx));
        }
        builder.addGlobal(mir.globalType(g), mir.globalSymbol(g),
                          newInitIdx, MirFuncId{},
                          mir.globalBinding(g), mir.globalVisibility(g));
    }

    // Walk every function and rebuild it.
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        rb.rebuildFunction(f);
    }

    mir = std::move(builder).finish();
    result.ok = true;
    result.instructionsFolded = rb.instructionsFolded();
    return result;
}

} // namespace dss::opt::passes
