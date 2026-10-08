// D-LIR-DESCRIPTOR-BLOCK-IDS-SHIFTED-BY-A-BLOCK-INSERTING-PASS — `lir/lir_descriptor_blocks.hpp`.
//
// The blocks a jump table, a static `&&label` binding and a `__try` scope name are MIR→LIR's ids; the
// pipeline binds them against the FINAL module's byte offsets. These tests hold the translation to its
// rule on synthetic modules whose block layout is chosen exactly:
//   * through a pass that PUBLISHES its entry image, an entry goes to its block's FIRST piece and a scope's
//     last block to its LAST piece;
//   * a pass that publishes none is the identity — and is REFUSED, by name, the moment its blocks differ;
//   * a descriptor id with no image, an image out of layout order, and an offset table that does not
//     follow the final layout are each REFUSED, never passed through.
// The runtime half is the three examples `*_after_an_asm_label_function`, which fail without it.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_descriptor_blocks.hpp"
#include "lir/lowering/mir_to_lir.hpp"

#include <gtest/gtest.h>

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
        {"rewrite", &produced, &preserved, {}},
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

TEST(LirDescriptorBlocks, APassThatPublishesNoImageIsProvedTheIdentity) {
    Lir const produced  = moduleOf(kProduced);
    Lir const preserved = moduleOf(kProduced);
    std::vector<LirBlockRebuild> const steps{{"lir-peephole", &produced, &preserved, {}}};
    Descriptors d = oneOfEach();
    DiagnosticReporter rep;
    ASSERT_TRUE(translate(steps, d, rep)) << firstMessage(rep);
    EXPECT_EQ(d.jumpTables[0].slotBindings[0].first, 4u);
    EXPECT_EQ(d.jumpTables[0].slotBindings[1].first, 5u);
    EXPECT_EQ(d.labels[0].lirBlockV, 5u);
    EXPECT_EQ(d.scopes[0].endLirBlockV, 1u);
    EXPECT_EQ(d.scopes[0].handlerLirBlockV, 2u);
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

TEST(LirDescriptorBlocks, SameBlockCountsWithDifferentSuccessorsAreNotTheIdentity) {
    // f0's first block returns instead of branching: the ranges agree and the CFG does not.
    Lir const produced  = moduleOf(kProduced);
    Lir const rewired   = moduleOf({{{}, {}}, {{1, 2}, {}, {}}});
    std::vector<LirBlockRebuild> const steps{{"rewrite", &produced, &rewired, {}}};
    Descriptors d = oneOfEach();
    DiagnosticReporter rep;
    EXPECT_FALSE(translate(steps, d, rep));
    std::string const m = firstMessage(rep);
    EXPECT_NE(m.find("block 1's successors differ"), std::string::npos) << m;
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
        {"rewrite", &produced, &preserved, {}},
        {"callconv", &produced, &split, kSplitEntries},
    };
    Descriptors d = oneOfEach();
    DiagnosticReporter rep;
    EXPECT_FALSE(translate(steps, d, rep));
    EXPECT_NE(firstMessage(rep).find("broken at pass 'rewrite'"), std::string::npos) << firstMessage(rep);
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
