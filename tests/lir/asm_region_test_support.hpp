#pragma once

// P68 round 8 part 4 — THE INLINE-ASM BUNDLE, SEEN FROM A PIN.
//
// An inline-asm statement lowers to ONE `asm_region` instruction (the
// `asm_region_goto` terminator for `asm goto`) whose operands are the
// statement's slots and whose BODY — a one-function module side structure —
// holds the template's own instructions (`lir/lir_asm_region.hpp`). A pin that
// used to look for "the template's `add`" among the function's instructions
// looks for it in the body now; a pin about the EMITTED shape runs the
// pipeline through `expandAsmRegions`, which is where the body becomes real
// instructions of the function. These helpers are that vocabulary, stated once.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_2addr_legalize.hpp"
#include "lir/lir_asm_region.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_peephole.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"
#include "lir/lir_verifier.hpp"
#include "lir/lir_wide_call_args.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace dss::test_support {

// Every asm-region bundle of the module, in block order.
[[nodiscard]] inline std::vector<LirInstId> asmRegionBundles(Lir const& lir) {
    std::vector<LirInstId> out;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const f = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(f); ++bi) {
            LirBlockId const b = lir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(b); ++ii) {
                LirInstId const id = lir.blockInstAt(b, ii);
                if (lir.instAsmRegion(id) != nullptr) out.push_back(id);
            }
        }
    }
    return out;
}

struct AsmRegionRef {
    LirInstId           bundle{};
    LirAsmRegion const* region = nullptr;
};

// The ONE bundle a single-statement snippet lowers to (a failure otherwise).
[[nodiscard]] inline std::optional<AsmRegionRef> onlyAsmRegion(Lir const& lir) {
    auto const all = asmRegionBundles(lir);
    EXPECT_EQ(all.size(), 1u)
        << "the snippet's one inline-asm statement lowers to exactly one "
           "`asm_region` bundle";
    if (all.size() != 1) return std::nullopt;
    return AsmRegionRef{all.front(), lir.instAsmRegion(all.front())};
}

// The body's EMITTED blocks — every block but `exit` and the `asm goto` stubs —
// in the order the template BEGAN them, which is its textual order and the
// order the expansion lays them out in.
[[nodiscard]] inline std::vector<LirBlockId>
asmRegionBodyBlocks(LirAsmRegion const& r) {
    std::vector<LirBlockId> out;
    Lir const& body = r.body;
    LirFuncId const fn = body.funcAt(0);
    for (std::uint32_t bi = 0; bi < body.funcBlockCount(fn); ++bi) {
        LirBlockId const b = body.funcBlockAt(fn, bi);
        if (b.v == r.exit.v) continue;
        if (std::any_of(r.gotoTargets.begin(), r.gotoTargets.end(),
                        [&](LirBlockId s) { return s.v == b.v; })) {
            continue;
        }
        out.push_back(b);
    }
    std::stable_sort(out.begin(), out.end(), [&](LirBlockId a, LirBlockId c) {
        return body.blockInstAt(a, 0).v < body.blockInstAt(c, 0).v;
    });
    return out;
}

// The body's instructions in that layout — the template's lines, in order,
// with the terminators the template (or its fall-off-the-end) ends each block
// with.
[[nodiscard]] inline std::vector<LirInstId>
asmRegionBodyInsts(LirAsmRegion const& r) {
    std::vector<LirInstId> out;
    for (LirBlockId const b : asmRegionBodyBlocks(r)) {
        for (std::uint32_t ii = 0; ii < r.body.blockInstCount(b); ++ii) {
            out.push_back(r.body.blockInstAt(b, ii));
        }
    }
    return out;
}

// The role of the slot whose body register is `reg`, or nullopt when `reg` is
// no slot's (a pinned register, or not an operand at all).
[[nodiscard]] inline std::optional<LirAsmOperandRole>
asmSlotRoleOf(LirAsmRegion const& r, LirReg reg) {
    for (std::size_t k = 0; k < r.bodyRegs.size(); ++k) {
        if (r.bodyRegs[k] == reg) return r.roles[k];
    }
    return std::nullopt;
}

// Run the LIR pipeline exactly as `compile_pipeline` does, from the MIR→LIR
// output up to and INCLUDING the asm-region expansion — the module callconv
// would receive. Every stage's paired check runs too; a failure is a test
// failure and yields nullopt.
[[nodiscard]] inline std::optional<Lir>
lirThroughAsmExpansion(Lir const& lowered, TargetSchema const& schema,
                       std::uint16_t ccIndex = 0) {
    DiagnosticReporter rep;
    auto wide = lowerWideCallArgs(lowered, schema, ccIndex, rep);
    if (!wide.ok) { ADD_FAILURE() << "wide-call lowering refused"; return std::nullopt; }
    auto const liveness = analyzeLiveness(wide.lir);
    auto const alloc = allocateRegisters(wide.lir, schema, liveness, ccIndex, rep);
    if (!alloc.ok()) { ADD_FAILURE() << "allocation refused"; return std::nullopt; }
    auto rewritten = rewriteWithAllocation(wide.lir, schema, alloc, rep);
    if (!rewritten.ok) { ADD_FAILURE() << "rewrite refused"; return std::nullopt; }
    auto legal = legalizeTwoAddress(rewritten.lir, schema, rep);
    if (!legal.ok()) { ADD_FAILURE() << "legalize refused"; return std::nullopt; }
    auto peeped = runLirPeephole(legal.lir, schema, rep);
    if (!peeped.ok()) { ADD_FAILURE() << "peephole refused"; return std::nullopt; }
    if (peeped.lir.asmRegionPool().empty()) {
        EXPECT_EQ(rep.errorCount(), 0u);
        return std::move(peeped.lir);
    }
    auto expanded = expandAsmRegions(peeped.lir, schema, rep);
    if (!expanded.ok) {
        ADD_FAILURE() << "asm-region expansion refused: "
                      << (rep.all().empty() ? std::string{}
                                            : rep.all().back().actual);
        return std::nullopt;
    }
    EXPECT_TRUE(verifyLirAsmRegionExpansion(peeped.lir, expanded.lir, schema, rep));
    EXPECT_TRUE(verifyLirPostRegalloc(expanded.lir, schema, rep));
    EXPECT_EQ(rep.errorCount(), 0u)
        << (rep.all().empty() ? std::string{} : rep.all().front().actual);
    return std::move(expanded.lir);
}

} // namespace dss::test_support
