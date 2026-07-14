#include "opt/passes/const_fold.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "hir/const_eval.hpp"
#include "hir/const_eval_arith.hpp"
#include "hir/hir_literal_pool.hpp"
#include "hir/hir_op.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/passes/mir_rebuild_helper.hpp"

#include <cstddef>
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
        } else if constexpr (std::is_same_v<T, MirSymbolAddrValue>) {
            // F5: a symbol-address literal is a MIR-tier link-time constant with
            // NO HIR representation — it arises only from global-init
            // classification, never from foldable expressions, so it cannot reach
            // a foldable use. Map to monostate (unknown/opaque) so the const-fold
            // bridge treats it as non-foldable rather than failing to compile.
            dst.value = std::monostate{};
        } else if constexpr (std::is_same_v<T, BitIntValue>) {
            // C4b (I1+C2): the `_BitInt` bit-precise value is the SAME host type in
            // both pools — copy directly (NEVER monostate). The I3 optimizer-fold
            // bail means a BitInt Const never reaches `tryFold`, but the bridge must
            // still round-trip it losslessly for any other consumer.
            dst.value = arm;
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
        } else if constexpr (std::is_same_v<T, HirAddressValue>) {
            // c43 (D-CSUBSET-ADDRESS-CONSTANT-FOLD): mirror the main hir_to_mir
            // toMirLiteral — an HIR address constant → the MIR symbol-address
            // relocation (the inverse of the toHirLiteral MirSymbolAddrValue arm
            // above, which stays monostate: a symbol address is non-foldable here).
            dst.value = MirSymbolAddrValue{arm.base, arm.byteOffset};
        } else if constexpr (std::is_same_v<T, BitIntValue>) {
            // C4b (I1+C2): the `_BitInt` bit-precise value copies directly between
            // the structurally-parallel pools (NEVER monostate).
            dst.value = arm;
        } else {
            dst.value = arm;
        }
    }, src.value);
    return dst;
}

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

// Constant-folding policy. The shared `MirFunctionRebuilder` owns the
// 3-phase walk + rewrite map + fail-loud contract; this policy adds
// the pass-specific tryRewrite that performs the fold.
class ConstFoldPolicy : public MirRebuildPolicy {
public:
    ConstFoldPolicy(Mir const& src, TypeInterner const& interner)
        : src_(src), interner_(interner) {}

    [[nodiscard]] std::size_t instructionsFolded() const noexcept {
        return folded_;
    }

    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        std::uint32_t const blockCount = src.funcBlockCount(fn);
        std::vector<MirBlockId> all;
        all.reserve(blockCount);
        for (std::uint32_t i = 0; i < blockCount; ++i) {
            all.push_back(src.funcBlockAt(fn, i));
        }
        return all;
    }

    [[nodiscard]] std::optional<MirInstId>
    tryRewrite(MirOpcode op, MirInstId oldId, MirBuilder& dst,
               std::unordered_map<std::uint32_t, MirInstId> const& /*rewrite*/) override {
        auto folded = tryFold(op, oldId);
        if (!folded.has_value()) return std::nullopt;
        MirInstId const newId = dst.addConst(toMirLiteral(*folded),
                                             src_.instType(oldId));
        constCache_.emplace(oldId.v, std::move(*folded));
        ++folded_;
        return newId;
    }

private:
    [[nodiscard]] std::optional<HirLiteralValue>
    tryFold(MirOpcode op, MirInstId oldId) {
        // I3 (D-CSUBSET-BITINT): NEVER fold a `_BitInt`-typed instruction here — for
        // ANY N, narrow OR wide. This pass's int64/uint64 helpers have NO mod-2^N
        // wrap, so a NARROW `_BitInt` Const pair would otherwise reach applyBinaryInt
        // via `asInt64` and produce an UN-wrapped int64 (a silent miscompile). The
        // wrap-aware fold lives in the HIR/CST const-eval (C4b); the optimizer defers
        // BitInt entirely. (A wide `_BitInt` is memory-resident — no scalar Const —
        // but the explicit guard pins the intent + is a red-on-disable boundary.)
        // A non-value instruction (Store/Br/…) carries InvalidType — `kind()` aborts
        // on it, so gate on `valid()` first.
        if (TypeId const it = src_.instType(oldId);
            it.valid() && interner_.kind(it) == TypeKind::BitInt) {
            return std::nullopt;
        }
        auto const oldOps = src_.instOperands(oldId);

        // Unary integer fold.
        if (oldOps.size() == 1 && unaryOpKind(op).has_value()) {
            auto const operand = readConstOperand(oldOps[0]);
            if (!operand.has_value()) return std::nullopt;
            auto folded = detail::applyUnaryInt(*unaryOpKind(op), *operand);
            if (!folded.has_value()) return std::nullopt;
            return normalizeToType(std::move(*folded), src_.instType(oldId));
        }

        // Binary integer fold.
        if (oldOps.size() == 2) {
            auto const a = readConstOperand(oldOps[0]);
            auto const b = readConstOperand(oldOps[1]);
            if (!a.has_value() || !b.has_value()) return std::nullopt;

            if (isUnsignedOp(op)) {
                auto folded = foldUnsigned(op, *a, *b);
                if (!folded.has_value()) return std::nullopt;
                return normalizeToType(std::move(*folded), src_.instType(oldId));
            }

            auto const bok = binaryOpKind(op);
            if (!bok.has_value()) return std::nullopt;

            EvalOptions opts{};
            opts.refuseOnOverflow         = false;
            opts.refuseOnDivByZero        = true;
            opts.refuseOnShiftOutOfRange  = true;
            ConstEvalFailure why = ConstEvalFailure::None;
            auto folded = detail::applyBinaryInt(*bok, *a, *b, opts, why);
            if (!folded.has_value()) return std::nullopt;
            return normalizeToType(std::move(*folded), src_.instType(oldId));
        }

        return std::nullopt;
    }

    [[nodiscard]] std::optional<HirLiteralValue>
    readConstOperand(MirInstId oldOp) const {
        if (auto cit = constCache_.find(oldOp.v); cit != constCache_.end()) {
            return cit->second;
        }
        if (src_.instOpcode(oldOp) == MirOpcode::Const) {
            std::uint32_t const idx = src_.constLiteralIndex(oldOp);
            return toHirLiteral(src_.literalValue(idx));
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<HirLiteralValue>
    normalizeToType(HirLiteralValue folded, TypeId dstType) const {
        TypeKind const dstKind = interner_.kind(dstType);
        if (folded.core == TypeKind::Bool) {
            auto const v = detail::asInt64(folded);
            folded.value = std::int64_t{v.value_or(0) != 0 ? 1 : 0};
            return folded;
        }
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
                folded.value = static_cast<std::int64_t>(ua / ub); return folded;
            case MirOpcode::UMod:
                if (ub == 0) return std::nullopt;
                folded.value = static_cast<std::int64_t>(ua % ub); return folded;
            case MirOpcode::LShr:
                if (*bv < 0 || *bv >= 64) return std::nullopt;
                folded.value = static_cast<std::int64_t>(ua >> *bv); return folded;
            case MirOpcode::ICmpUlt:
                folded.value = std::int64_t{ua <  ub ? 1 : 0}; folded.core = TypeKind::Bool; return folded;
            case MirOpcode::ICmpUle:
                folded.value = std::int64_t{ua <= ub ? 1 : 0}; folded.core = TypeKind::Bool; return folded;
            case MirOpcode::ICmpUgt:
                folded.value = std::int64_t{ua >  ub ? 1 : 0}; folded.core = TypeKind::Bool; return folded;
            case MirOpcode::ICmpUge:
                folded.value = std::int64_t{ua >= ub ? 1 : 0}; folded.core = TypeKind::Bool; return folded;
            default: return std::nullopt;
        }
    }

    Mir const&          src_;
    TypeInterner const& interner_;
    std::unordered_map<std::uint32_t, HirLiteralValue> constCache_;
    std::size_t         folded_ = 0;
};

} // namespace

ConstFoldResult runConstFold(Mir& mir, TypeInterner const& interner,
                             DiagnosticReporter& reporter) {
    ConstFoldResult result{};
    MirBuilder builder;

    if (cloneGlobalsOrCarveOut(mir, builder, reporter, "ConstFold")
        == GlobalClonePrelude::CarvedOut) {
        result.ok = true;
        return result;
    }

    // Drive the shared rebuilder with the ConstFold policy.
    ConstFoldPolicy policy{mir, interner};
    MirFunctionRebuilder rb{mir, builder, policy};
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        rb.rebuildFunction(mir.funcAt(i));
    }

    mir = std::move(builder).finish();
    result.ok = true;
    result.instructionsFolded = policy.instructionsFolded();
    return result;
}

} // namespace dss::opt::passes
