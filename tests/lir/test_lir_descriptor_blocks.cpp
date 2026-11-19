// D-LIR-DESCRIPTOR-BLOCK-IDS-SHIFTED-BY-A-BLOCK-INSERTING-PASS — `lir/lir_descriptor_blocks.hpp`.
//
// The blocks a jump table, a static `&&label` binding and a `__try` scope name are MIR→LIR's ids; the
// pipeline binds them against the FINAL module's byte offsets. These tests hold the translation to its
// rule on synthetic modules whose block layout is chosen exactly:
//   * through a pass that PUBLISHES its entry image, an entry goes to its block's FIRST piece and a scope's
//     last block to its LAST piece — an inserting pass's image and a block-for-block pass's identity alike;
//   * a pass that publishes NONE is REFUSED, by name, whatever its blocks look like: nothing is inferred,
//     because two blocks no edge tells apart may have changed places;
//   * a descriptor id with no image, an image out of layout order, and an offset table that does not
//     follow the final layout are each REFUSED, never passed through;
//   * the four REAL block-for-block rebuilds (wide-call arguments, the rewrite, the two-address legalizer,
//     the peephole) each publish the identity they performed.
// The runtime half is the three examples `*_after_an_asm_label_function`, which fail without it.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_2addr_legalize.hpp"
#include "lir/lir_descriptor_blocks.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_peephole.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"
#include "lir/lir_wide_call_args.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "lowered_lir_fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace dss;

namespace {

// Throws rather than aborts, so one failing test is reported as one failure.
std::shared_ptr<TargetSchema> const& x86Schema() {
    static std::shared_ptr<TargetSchema> const schema = [] {
        auto r = TargetSchema::loadShipped("x86_64");
        if (!r.has_value()) {
            throw std::runtime_error(
                "test environment: TargetSchema::loadShipped(\"x86_64\") failed - the shipped target "
                "documents are not reachable");
        }
        return *r;
    }();
    return schema;
}

std::uint16_t op(std::string_view mnemonic) {
    auto const i = x86Schema()->opcodeByMnemonic(mnemonic);
    if (!i.has_value()) {
        throw std::runtime_error(std::string("the shipped x86_64 target declares no opcode '")
                                 + std::string(mnemonic) + "' - this fixture names it directly");
    }
    return *i;
}

// A module of `fns.size()` functions: block k of function f has as successors the blocks
// `fns[f][k]` of the same function (none: `ret`; one: `jmp`; two: `jcc`). Blocks are laid out — and
// numbered — in creation order, from 1 (slot 0 is the arena sentinel).
using FuncShape = std::vector<std::vector<std::uint32_t>>;

[[nodiscard]] Lir moduleOf(std::vector<FuncShape> const& fns) {
    LirBuilder b{*x86Schema()};
    for (std::size_t f = 0; f < fns.size(); ++f) {
        (void)b.addFunction(SymbolId{static_cast<std::uint32_t>(100 + f)});
        std::vector<LirBlockId> ids;
        for (std::size_t k = 0; k < fns[f].size(); ++k) ids.push_back(b.createBlock());
        for (std::size_t k = 0; k < fns[f].size(); ++k) {
            b.beginBlock(ids[k]);
            auto const& s = fns[f][k];
            if (s.empty()) {
                b.addReturn(op("ret"), std::span<LirOperand const>{});
            } else if (s.size() == 1) {
                b.addBr(op("jmp"), ids[s[0]]);
            } else {
                b.addCondBr(op("jcc"), std::span<LirOperand const>{}, ids[s[0]], ids[s[1]]);
            }
        }
    }
    return std::move(b).finish();
}

// MIR→LIR's module: f0 = [b1 → b2, b2 ret]; f1 = [b3 → b4 | b5, b4 ret, b5 ret].
std::vector<FuncShape> const kProduced{{{1}, {}}, {{1, 2}, {}, {}}};
// What a pass that SPLITS f0's first block makes of it: f0 = [1 → 2, 2 → 3, 3 ret]; f1 = [4, 5, 6].
std::vector<FuncShape> const kSplit{{{1}, {2}, {}}, {{1, 2}, {}, {}}};
// The split's entry image, indexed by the produced module's block ids (slot 0 = the sentinel).
std::vector<std::uint32_t> const kSplitEntries{0, 1, 3, 4, 5, 6};
// What a pass that rebuilds the produced module block for block publishes: every block where it was.
std::vector<std::uint32_t> const kIdentity{0, 1, 2, 3, 4, 5};

struct Descriptors {
    std::vector<JumpTableDescriptor>   jumpTables;
    std::vector<LirBlockSymbolBinding> labels;
    std::vector<SehScopeDescriptor>    scopes;
};

// One of each kind, every id in the PRODUCED module: a jump table and a label binding in f1, and a
// `__try` in f0 whose guarded body is block 1 and whose handler is block 2.
[[nodiscard]] Descriptors oneOfEach() {
    Descriptors d;
    JumpTableDescriptor jt;
    jt.tableSymbol = SymbolId{900};
    jt.slotCount   = 3;
    jt.funcIndex   = 1;
    jt.slotBindings = {{4u, 0u}, {5u, 1u}, {4u, 2u}};
    jt.blockSymbols = {{4u, SymbolId{901}}, {5u, SymbolId{902}}};
    d.jumpTables.push_back(std::move(jt));
    LirBlockSymbolBinding lb;
    lb.funcIndex = 1;
    lb.lirBlockV = 5;
    lb.symbol    = SymbolId{903};
    d.labels.push_back(lb);
    SehScopeDescriptor s;
    s.funcIndex        = 0;
    s.beginLirBlockV   = 1;
    s.endLirBlockV     = 1;
    s.handlerLirBlockV = 2;
    d.scopes.push_back(s);
    return d;
}

[[nodiscard]] bool translate(std::vector<LirBlockRebuild> const& steps, Descriptors& d,
                             DiagnosticReporter& rep) {
    return translateDescriptorBlockIds(steps, d.jumpTables, d.labels, d.scopes, rep);
}

[[nodiscard]] std::string firstMessage(DiagnosticReporter const& rep) {
    return rep.all().empty() ? std::string{} : rep.all()[0].actual;
}

}  // namespace

// ── the translation ────────────────────────────────────────────────────────

TEST(LirDescriptorBlocks, AnInsertingPassTakesAnEntryToItsFirstPieceAndALastBlockToItsLast) {
    Lir const produced  = moduleOf(kProduced);
    Lir const preserved = moduleOf(kProduced);
    Lir const split     = moduleOf(kSplit);
    std::vector<LirBlockRebuild> const steps{
        {"rewrite", &produced, &preserved, kIdentity},
        {"callconv", &preserved, &split, kSplitEntries},
    };
    Descriptors d = oneOfEach();
    DiagnosticReporter rep;
    ASSERT_TRUE(translate(steps, d, rep)) << firstMessage(rep);
    EXPECT_EQ(rep.errorCount(), 0u);

    // Every f1 block moved up by one: the split block sits before them.
    auto const& jt = d.jumpTables[0];
    ASSERT_EQ(jt.slotBindings.size(), 3u);
    EXPECT_EQ(jt.slotBindings[0], (std::pair<std::uint32_t, std::size_t>{5u, 0u}));
    EXPECT_EQ(jt.slotBindings[1], (std::pair<std::uint32_t, std::size_t>{6u, 1u}));
    EXPECT_EQ(jt.slotBindings[2], (std::pair<std::uint32_t, std::size_t>{5u, 2u}));
    ASSERT_EQ(jt.blockSymbols.size(), 2u);
    EXPECT_EQ(jt.blockSymbols.at(5u).v, 901u);
    EXPECT_EQ(jt.blockSymbols.at(6u).v, 902u);
    EXPECT_EQ(d.labels[0].lirBlockV, 6u);

    // The guarded block became pieces 1 and 2: the scope begins at the first and ENDS AT THE LAST, so the
    // range still covers all of it; the handler is the next block's entry.
    EXPECT_EQ(d.scopes[0].beginLirBlockV, 1u);
    EXPECT_EQ(d.scopes[0].endLirBlockV, 2u);
    EXPECT_EQ(d.scopes[0].handlerLirBlockV, 3u);
}

TEST(LirDescriptorBlocks, ABlockForBlockPassPublishesTheIdentityAndIsFollowed) {
    Lir const produced  = moduleOf(kProduced);
    Lir const preserved = moduleOf(kProduced);
    std::vector<LirBlockRebuild> const steps{{"lir-peephole", &produced, &preserved, kIdentity}};
    Descriptors d = oneOfEach();
    DiagnosticReporter rep;
    ASSERT_TRUE(translate(steps, d, rep)) << firstMessage(rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(d.jumpTables[0].slotBindings[0].first, 4u);
    EXPECT_EQ(d.jumpTables[0].slotBindings[1].first, 5u);
    EXPECT_EQ(d.labels[0].lirBlockV, 5u);
    EXPECT_EQ(d.scopes[0].endLirBlockV, 1u);
    EXPECT_EQ(d.scopes[0].handlerLirBlockV, 2u);
}

// The same two modules, and the pass says nothing: refused. That the blocks LOOK unchanged — the same
// ranges, the same successors — is exactly what is no longer taken for an answer.
TEST(LirDescriptorBlocks, APassThatPublishesNoImageIsRefusedEvenWhenItsBlocksLookUnchanged) {
    Lir const produced  = moduleOf(kProduced);
    Lir const preserved = moduleOf(kProduced);
    std::vector<LirBlockRebuild> const steps{{"lir-peephole", &produced, &preserved, {}}};
    Descriptors d = oneOfEach();
    DiagnosticReporter rep;
    EXPECT_FALSE(translate(steps, d, rep));
    ASSERT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(rep.all()[0].code, DiagnosticCode::L_SideStructureIndexDangling);
    std::string const m = firstMessage(rep);
    EXPECT_NE(m.find("'lir-peephole'"), std::string::npos) << m;
    EXPECT_NE(m.find("publishes no block image for a module that has blocks"), std::string::npos) << m;
}

// A module with no block at all owes no image: a declaration-only translation unit goes through every
// rebuild empty.
TEST(LirDescriptorBlocks, AModuleWithNoBlockOwesNoImage) {
    Lir const produced  = moduleOf({});
    Lir const preserved = moduleOf({});
    std::vector<LirBlockRebuild> const steps{{"lir-peephole", &produced, &preserved, {}}};
    Descriptors d;
    DiagnosticReporter rep;
    EXPECT_TRUE(translate(steps, d, rep)) << firstMessage(rep);
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(LirDescriptorBlocks, APassThatChangedItsBlocksWithoutAnImageIsRefusedByName) {
    Lir const produced = moduleOf(kProduced);
    Lir const split    = moduleOf(kSplit);
    std::vector<LirBlockRebuild> const steps{{"lir-peephole", &produced, &split, {}}};
    Descriptors d = oneOfEach();
    DiagnosticReporter rep;
    EXPECT_FALSE(translate(steps, d, rep));
    ASSERT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(rep.all()[0].code, DiagnosticCode::L_SideStructureIndexDangling);
    std::string const m = firstMessage(rep);
    EXPECT_NE(m.find("'lir-peephole'"), std::string::npos) << m;
    EXPECT_NE(m.find("publishes no block image"), std::string::npos) << m;
}

// THE CASE AN INFERENCE CANNOT SEE. Three blocks that only DATA reaches — a static label table's targets —
// have no edge between them, so a rebuild that moved the second behind the third leaves a module with the
// same block range and the same successors, block by block. Only the pass knows; so the pass says, and what
// it says is held to the layout.
TEST(LirDescriptorBlocks, TwoBlocksNoEdgeTellsApartAreNeverInferredToBeInPlace) {
    std::vector<FuncShape> const labelTargets{{{}, {}, {}}};
    Lir const produced = moduleOf(labelTargets);
    Lir const moved    = moduleOf(labelTargets);
    auto const lastLabel = [] {
        Descriptors d;
        LirBlockSymbolBinding lb;
        lb.funcIndex = 0;
        lb.lirBlockV = 3;
        lb.symbol    = SymbolId{950};
        d.labels.push_back(lb);
        return d;
    };
    {
        // Published truthfully, the move is refused: a block's pieces run from its entry to the next
        // block's, so an image must follow the layout.
        std::vector<std::uint32_t> const swapped{0, 1, 3, 2};
        std::vector<LirBlockRebuild> const steps{{"lir-peephole", &produced, &moved, swapped}};
        Descriptors d = lastLabel();
        DiagnosticReporter rep;
        EXPECT_FALSE(translate(steps, d, rep));
        std::string const m = firstMessage(rep);
        EXPECT_NE(m.find("'lir-peephole' published entry 2 for block 3 (#2 of function #0)"),
                  std::string::npos) << m;
        EXPECT_NE(m.find("the block before it entered at 3"), std::string::npos) << m;
    }
    {
        // Published by nobody, it is not assumed away.
        std::vector<LirBlockRebuild> const steps{{"lir-peephole", &produced, &moved, {}}};
        Descriptors d = lastLabel();
        DiagnosticReporter rep;
        EXPECT_FALSE(translate(steps, d, rep));
        ASSERT_EQ(rep.errorCount(), 1u);
        EXPECT_NE(firstMessage(rep).find("publishes no block image"), std::string::npos)
            << firstMessage(rep);
    }
    {
        // The control: a pass that kept the three where they were says so, and is followed.
        std::vector<std::uint32_t> const identity{0, 1, 2, 3};
        std::vector<LirBlockRebuild> const steps{{"lir-peephole", &produced, &moved, identity}};
        Descriptors d = lastLabel();
        DiagnosticReporter rep;
        ASSERT_TRUE(translate(steps, d, rep)) << firstMessage(rep);
        EXPECT_EQ(d.labels[0].lirBlockV, 3u);
    }
}

// An identity published over an output whose blocks are NOT where the input's were is refused too: the
// image is checked against the output it claims to describe.
TEST(LirDescriptorBlocks, AnIdentityImageOverAnOutputWhoseBlocksMovedIsRefused) {
    Lir const produced = moduleOf(kProduced);
    Lir const split    = moduleOf(kSplit);   // f1's blocks are 4..6 there, not 3..5
    std::vector<LirBlockRebuild> const steps{{"rewrite", &produced, &split, kIdentity}};
    Descriptors d = oneOfEach();
    DiagnosticReporter rep;
    EXPECT_FALSE(translate(steps, d, rep));
    ASSERT_EQ(rep.errorCount(), 1u);
    std::string const m = firstMessage(rep);
    EXPECT_NE(m.find("'rewrite' published entry 3 for block 3 (#0 of function #1)"), std::string::npos)
        << m;
    EXPECT_NE(m.find("occupy 4..6 and its first block must enter at the first of them"),
              std::string::npos) << m;
}

TEST(LirDescriptorBlocks, ADescriptorIdWithNoImageIsRefusedNamingTheDescriptorAndTheId) {
    Lir const produced = moduleOf(kProduced);
    Lir const split    = moduleOf(kSplit);
    std::vector<LirBlockRebuild> const steps{{"callconv", &produced, &split, kSplitEntries}};
    {
        // Block 1 is f0's: the jump table is f1's.
        Descriptors d = oneOfEach();
        d.jumpTables[0].slotBindings[1].first = 1u;
        DiagnosticReporter rep;
        EXPECT_FALSE(translate(steps, d, rep));
        ASSERT_EQ(rep.errorCount(), 1u);
        EXPECT_EQ(rep.all()[0].code, DiagnosticCode::L_SideStructureIndexDangling);
        std::string const m = firstMessage(rep);
        EXPECT_NE(m.find("the jump table { 900 }"), std::string::npos) << m;
        EXPECT_NE(m.find("names block 1 of function #1"), std::string::npos) << m;
    }
    {
        // Past every block of the module.
        Descriptors d = oneOfEach();
        d.labels[0].lirBlockV = 99u;
        DiagnosticReporter rep;
        EXPECT_FALSE(translate(steps, d, rep));
        std::string const m = firstMessage(rep);
        EXPECT_NE(m.find("the static label-address binding { 903 }"), std::string::npos) << m;
        EXPECT_NE(m.find("names block 99"), std::string::npos) << m;
    }
    {
        Descriptors d = oneOfEach();
        d.scopes[0].handlerLirBlockV = 0u;
        DiagnosticReporter rep;
        EXPECT_FALSE(translate(steps, d, rep));
        EXPECT_NE(firstMessage(rep).find("__try scope #0 (its handler) names block 0"), std::string::npos)
            << firstMessage(rep);
    }
}

TEST(LirDescriptorBlocks, AnImageOutOfLayoutOrderOrOfTheWrongSizeIsRefused) {
    Lir const produced = moduleOf(kProduced);
    Lir const split    = moduleOf(kSplit);
    {
        std::vector<std::uint32_t> const backwards{0, 3, 1, 4, 5, 6};
        std::vector<LirBlockRebuild> const steps{{"asm-region-expansion", &produced, &split, backwards}};
        Descriptors d = oneOfEach();
        DiagnosticReporter rep;
        EXPECT_FALSE(translate(steps, d, rep));
        std::string const m = firstMessage(rep);
        EXPECT_NE(m.find("'asm-region-expansion' published entry 3 for block 1"), std::string::npos) << m;
    }
    {
        std::vector<std::uint32_t> const shortImage{0, 1, 3};
        std::vector<LirBlockRebuild> const steps{{"callconv", &produced, &split, shortImage}};
        Descriptors d = oneOfEach();
        DiagnosticReporter rep;
        EXPECT_FALSE(translate(steps, d, rep));
        EXPECT_NE(firstMessage(rep).find("a block image of 3 entr(ies) for a module of 6 block slot(s)"),
                  std::string::npos) << firstMessage(rep);
    }
}

TEST(LirDescriptorBlocks, ABrokenRebuildChainIsRefused) {
    Lir const produced  = moduleOf(kProduced);
    Lir const preserved = moduleOf(kProduced);
    Lir const split     = moduleOf(kSplit);
    // The second step does not take the module the first produced.
    std::vector<LirBlockRebuild> const steps{
        {"rewrite", &produced, &preserved, kIdentity},
        {"callconv", &produced, &split, kSplitEntries},
    };
    Descriptors d = oneOfEach();
    DiagnosticReporter rep;
    EXPECT_FALSE(translate(steps, d, rep));
    EXPECT_NE(firstMessage(rep).find("broken at pass 'rewrite'"), std::string::npos) << firstMessage(rep);
}

// ── the four real block-for-block rebuilds ─────────────────────────────────

// The pipeline's own passes, over a real lowering: each publishes an image of the size of its input's
// block arena that takes block k of function f to block k of function f of its output — the identity it
// performed — and the chain of the four carries a block that data names to the same place in the last
// module. Withhold any ONE image and the chain is refused naming that pass.
TEST(LirDescriptorBlocks, TheFourBlockForBlockRebuildsPublishTheIdentityTheyPerformed) {
    auto const lowered = test_support::lowerCToLir(R"(
        int pick(int a, int b, int c) {
            int r = 0;
            for (int i = 0; i < a; ++i) {
                if (i & 1) r += b; else r -= c;
            }
            switch (r & 3) {
                case 0: return a;
                case 1: return b;
                case 2: return c;
                default: return r;
            }
        }
        int wide(int a, int b, int c, int d, int e, int f, int g, int h) {
            return a + b + c + d + e + f + g + h;
        }
        int main(int argc, char **argv) {
            (void)argv;
            if (argc > 3) return wide(1, 2, 3, 4, 5, 6, 7, argc);
            return pick(argc, 2, 3);
        }
    )");
    ASSERT_TRUE(lowered.lir.ok);
    TargetSchema const& schema   = *lowered.target;
    Lir const&          produced = lowered.lir.lir;
    DiagnosticReporter  rep;
    auto const wide = lowerWideCallArgs(produced, schema, 0, rep);
    ASSERT_TRUE(wide.ok) << firstMessage(rep);
    auto const liveness = analyzeLiveness(wide.lir);
    auto const alloc    = allocateRegisters(wide.lir, schema, liveness, 0, rep);
    ASSERT_TRUE(alloc.ok()) << firstMessage(rep);
    auto const rewritten = rewriteWithAllocation(wide.lir, schema, alloc, rep);
    ASSERT_TRUE(rewritten.ok) << firstMessage(rep);
    auto const legal = legalizeTwoAddress(rewritten.lir, schema, rep);
    ASSERT_TRUE(legal.ok()) << firstMessage(rep);
    auto const peeped = runLirPeephole(legal.lir, schema, rep);
    ASSERT_TRUE(peeped.ok()) << firstMessage(rep);
    ASSERT_EQ(rep.errorCount(), 0u) << firstMessage(rep);

    struct Real {
        std::string_view                  pass;
        Lir const*                        in;
        Lir const*                        out;
        std::vector<std::uint32_t> const* image;
    };
    std::vector<Real> const real{
        {"wide-call-args", &produced, &wide.lir, &wide.blockEntryImage},
        {"rewrite", &wide.lir, &rewritten.lir, &rewritten.blockEntryImage},
        {"two-address-legalize", &rewritten.lir, &legal.lir, &legal.blockEntryImage},
        {"lir-peephole", &legal.lir, &peeped.lir, &peeped.blockEntryImage},
    };
    // Not a vacuous module: three functions, and enough blocks that a slot left at 0 could not hide.
    ASSERT_EQ(produced.moduleFuncCount(), 3u);
    std::size_t producedBlocks = 0;
    for (std::uint32_t fi = 0; fi < produced.moduleFuncCount(); ++fi) {
        producedBlocks += produced.funcBlockCount(produced.funcAt(fi));
    }
    ASSERT_GE(producedBlocks, 10u);

    for (Real const& s : real) {
        SCOPED_TRACE(std::string(s.pass));
        ASSERT_EQ(s.image->size(), s.in->blockCount());
        ASSERT_EQ(s.in->moduleFuncCount(), s.out->moduleFuncCount());
        for (std::uint32_t fi = 0; fi < s.in->moduleFuncCount(); ++fi) {
            LirFuncId const inFn  = s.in->funcAt(fi);
            LirFuncId const outFn = s.out->funcAt(fi);
            ASSERT_EQ(s.in->funcBlockCount(inFn), s.out->funcBlockCount(outFn)) << "function #" << fi;
            for (std::uint32_t k = 0; k < s.in->funcBlockCount(inFn); ++k) {
                EXPECT_EQ((*s.image)[s.in->funcBlockAt(inFn, k).v], s.out->funcBlockAt(outFn, k).v)
                    << "block #" << k << " of function #" << fi;
            }
        }
    }

    // A label binding to each function's LAST block, through the four real steps.
    auto const lastBlocks = [&] {
        Descriptors d;
        for (std::uint32_t fi = 0; fi < produced.moduleFuncCount(); ++fi) {
            LirFuncId const fn = produced.funcAt(fi);
            LirBlockSymbolBinding lb;
            lb.funcIndex = fi;
            lb.lirBlockV = produced.funcBlockAt(fn, produced.funcBlockCount(fn) - 1).v;
            lb.symbol    = SymbolId{700 + fi};
            d.labels.push_back(lb);
        }
        return d;
    };
    std::vector<LirBlockRebuild> steps;
    for (Real const& s : real) steps.push_back({s.pass, s.in, s.out, *s.image});
    {
        Descriptors d = lastBlocks();
        DiagnosticReporter trep;
        ASSERT_TRUE(translate(steps, d, trep)) << firstMessage(trep);
        for (std::uint32_t fi = 0; fi < produced.moduleFuncCount(); ++fi) {
            LirFuncId const fn = peeped.lir.funcAt(fi);
            EXPECT_EQ(d.labels[fi].lirBlockV,
                      peeped.lir.funcBlockAt(fn, peeped.lir.funcBlockCount(fn) - 1).v)
                << "function #" << fi;
        }
    }
    for (std::size_t withheld = 0; withheld < steps.size(); ++withheld) {
        SCOPED_TRACE(std::string(steps[withheld].pass));
        auto silent = steps;
        silent[withheld].entryImage = {};
        Descriptors d = lastBlocks();
        DiagnosticReporter trep;
        EXPECT_FALSE(translate(silent, d, trep));
        ASSERT_EQ(trep.errorCount(), 1u);
        std::string const m = firstMessage(trep);
        EXPECT_NE(m.find(std::string("LIR pass '") + std::string(steps[withheld].pass) + "'"),
                  std::string::npos) << m;
        EXPECT_NE(m.find("publishes no block image"), std::string::npos) << m;
    }
}

// ── the assembler's offsets ────────────────────────────────────────────────

namespace {
struct FakeAssembledFunction {
    std::unordered_map<std::uint32_t, std::uint32_t> blockByteOffsets;
};
}  // namespace

TEST(LirDescriptorBlocks, BlockOffsetsMustCoverTheFinalLayoutInOrder) {
    Lir const split = moduleOf(kSplit);   // f0 = blocks 1..3, f1 = blocks 4..6
    std::vector<FakeAssembledFunction> fns(2);
    fns[0].blockByteOffsets = {{1u, 0u}, {2u, 10u}, {3u, 10u}};   // an empty block shares its offset
    fns[1].blockByteOffsets = {{4u, 0u}, {5u, 7u}, {6u, 9u}};
    {
        DiagnosticReporter rep;
        EXPECT_TRUE(verifyBlockOffsetsFollowLayout(split, fns, rep)) << firstMessage(rep);
    }
    {
        auto missing = fns;
        missing[1].blockByteOffsets.erase(5u);
        missing[1].blockByteOffsets.emplace(7u, 7u);   // same count, a block the function does not have
        DiagnosticReporter rep;
        EXPECT_FALSE(verifyBlockOffsetsFollowLayout(split, missing, rep));
        EXPECT_NE(firstMessage(rep).find("block 5 (#1 of function #1) is missing"), std::string::npos)
            << firstMessage(rep);
    }
    {
        auto reordered = fns;
        reordered[0].blockByteOffsets[2u] = 30u;   // block 2 after block 3
        reordered[0].blockByteOffsets[3u] = 20u;
        DiagnosticReporter rep;
        EXPECT_FALSE(verifyBlockOffsetsFollowLayout(split, reordered, rep));
        EXPECT_NE(firstMessage(rep).find("block 3 (#2 of function #0) is 20, before the 30"),
                  std::string::npos) << firstMessage(rep);
    }
    {
        auto extra = fns;
        extra[0].blockByteOffsets.emplace(9u, 40u);
        DiagnosticReporter rep;
        EXPECT_FALSE(verifyBlockOffsetsFollowLayout(split, extra, rep));
        EXPECT_NE(firstMessage(rep).find("published 4 block offset(s) for function #0, which has 3"),
                  std::string::npos) << firstMessage(rep);
    }
}
