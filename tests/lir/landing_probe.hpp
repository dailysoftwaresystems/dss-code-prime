#pragma once

// THE LANDING PROBE — a hand-built function with one guarded run and one landing
// block, in which nothing but the landing rule can keep a value out of a volatile
// register. Shared by the allocator's own cases (lir/test_lir_regalloc) and by the
// binary that contains the allocator's abort
// (lir/test_landing_rule_contains_the_audit), so the two read one module.
//
//   E  : g = mov #7 ; f = <gpr→fpr move> g ; w = mov #1 ; jmp B1
//   B1 : t = mov w ; jmp B2                      ┐ the guarded run
//   B2 : u = mov t ; jmp J                       ┘
//   H  : gx = mov g ; fx = <fpr→gpr move> f ; jmp J     the landing block
//   J  : ret
// No call anywhere. The two cross-class moves are the target's own declared ones.

#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_node.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_set>
#include <utility>

namespace dss::test_support {

struct LandingProbe {
    Lir        lir;
    LirBlockId b1{};
    LirBlockId b2{};
    LirBlockId landing{};
    LirReg     g{};   // GPR, defined before the run; the landing block's FIRST instruction reads it
    LirReg     f{};   // FPR, defined before the run; the landing block reads it

    [[nodiscard]] LirGuardedRegion region() const {
        LirGuardedRegion r;
        r.funcIndex     = 0;
        r.firstBlockV   = b1.v;
        r.lastBlockV    = b2.v;
        r.landingBlockV = landing.v;
        return r;
    }
};

// Nullopt when the target declares no cross-class move (the probe then has no FPR
// value to keep).
[[nodiscard]] inline std::optional<LandingProbe> buildLandingProbe(TargetSchema const& sch) {
    auto const movOp = sch.opcodeByMnemonic("mov");
    auto const jmpOp = sch.opcodeByMnemonic("jmp");
    auto const retOp = sch.opcodeByMnemonic("ret");
    auto const toFpr = sch.regClassOpOpcode(TargetRegClass::GPR, TargetRegClass::FPR,
                                            RegClassOp::Move);
    auto const toGpr = sch.regClassOpOpcode(TargetRegClass::FPR, TargetRegClass::GPR,
                                            RegClassOp::Move);
    if (!movOp.has_value() || !jmpOp.has_value() || !retOp.has_value()
        || !toFpr.has_value() || !toGpr.has_value()) {
        return std::nullopt;
    }
    auto const imm = [](std::int32_t k) {
        return std::array<LirOperand, 1>{LirOperand::makeImmInt32(k)};
    };
    auto const use = [](LirReg r) {
        return std::array<LirOperand, 1>{LirOperand::makeReg(r)};
    };

    LirBuilder b{sch};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    LirBlockId const b1    = b.createBlock();
    LirBlockId const b2    = b.createBlock();
    LirBlockId const h     = b.createBlock();
    LirBlockId const j     = b.createBlock();

    b.beginBlock(entry);
    LirReg const g = b.newVReg(LirRegClass::GPR);
    (void)b.addInst(*movOp, g, imm(7));
    LirReg const f = b.newVReg(LirRegClass::FPR);
    (void)b.addInst(*toFpr, f, use(g));
    LirReg const w = b.newVReg(LirRegClass::GPR);
    (void)b.addInst(*movOp, w, imm(1));
    (void)b.addBr(*jmpOp, b1);

    b.beginBlock(b1);
    LirReg const t = b.newVReg(LirRegClass::GPR);
    (void)b.addInst(*movOp, t, use(w));
    (void)b.addBr(*jmpOp, b2);

    b.beginBlock(b2);
    LirReg const u = b.newVReg(LirRegClass::GPR);
    (void)b.addInst(*movOp, u, use(t));
    (void)b.addBr(*jmpOp, j);

    b.beginBlock(h);
    LirReg const gx = b.newVReg(LirRegClass::GPR);
    (void)b.addInst(*movOp, gx, use(g));
    LirReg const fx = b.newVReg(LirRegClass::GPR);
    (void)b.addInst(*toGpr, fx, use(f));
    (void)b.addBr(*jmpOp, j);

    b.beginBlock(j);
    (void)b.addReturn(*retOp, std::span<LirOperand const>{});

    return LandingProbe{std::move(b).finish(), b1, b2, h, g, f};
}

// The convention's volatile registers, by ordinal, and whether it keeps ANY
// register of class FPR across a call — both read from its own two lists.
struct ConventionFacts {
    std::unordered_set<std::uint16_t> volatileOrdinals;
    bool                              keepsAFpr = false;
    std::optional<std::uint16_t>      aVolatileGpr;
    std::optional<std::uint16_t>      aVolatileFpr;
};

[[nodiscard]] inline ConventionFacts conventionFacts(TargetSchema const& sch,
                                                     std::uint16_t ccIndex) {
    ConventionFacts out;
    auto const* cc = sch.callingConvention(ccIndex);
    if (cc == nullptr) return out;
    for (auto const& n : cc->callerSaved) {
        auto const ord = sch.registerByName(n);
        if (!ord.has_value()) continue;
        out.volatileOrdinals.insert(*ord);
        auto const* info = sch.registerInfo(*ord);
        if (info == nullptr) continue;
        if (info->regClass == TargetRegClass::GPR && !out.aVolatileGpr.has_value()) {
            out.aVolatileGpr = *ord;
        }
        if (info->regClass == TargetRegClass::FPR && !out.aVolatileFpr.has_value()) {
            out.aVolatileFpr = *ord;
        }
    }
    for (auto const& n : cc->calleeSaved) {
        auto const ord = sch.registerByName(n);
        if (!ord.has_value()) continue;
        auto const* info = sch.registerInfo(*ord);
        if (info != nullptr && info->regClass == TargetRegClass::FPR) out.keepsAFpr = true;
    }
    return out;
}

} // namespace dss::test_support
