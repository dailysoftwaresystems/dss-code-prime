// D-C-LABEL-ADDRESS-IN-A-STATIC-INITIALIZER-REFUSED — the MIR→LIR half.
//
// HIR→MIR turns `static void *const tbl[] = {&&L0, &&L1};` into an initializer
// literal that relocates against a per-block symbol, plus a `BlockAddressExport`
// in the owning function that says which block that symbol names. This tier owes
// two things in exchange, and each of them fails SILENTLY if it is wrong:
//
//   1. THE SYMBOL MUST BE PUBLISHED WITH A BYTE OFFSET. Nothing in the code names
//      an exported block when the address is only ever read out of the table, so
//      the assembler's encoder-driven `BlockSymPatch` channel never fires. The
//      binding rides `MirToLirResult::blockSymbolBindings` instead — the same gap
//      the dense-switch jump table and a hand-written `.s` table already describe.
//      Lose it and the linker reports an undefined symbol, which is loud; bind the
//      WRONG block and it does not.
//
//   2. ONE BLOCK MUST HAVE ONE SYMBOL. A label addressed from BOTH the body and
//      the table has a `lea` and a relocation, and if the two mint separately the
//      link fails with "declared more than once" — a defect this project already
//      measured once, on the computed-goto path.
//
// The pins below read the LIR and the descriptor list directly rather than
// asserting "it lowered", because the whole defect this row names WAS a clean
// refusal: a compile-only assertion goes green the moment the refusal is deleted.

#include "lowered_lir_fixture.hpp"

#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <unordered_set>

using namespace dss;
using dss::test_support::lowerCToLir;

namespace {

// Count BLOCK-ADDRESS materializations: a `lea` carrying BOTH a SymbolRef and a
// trailing BlockRef, which is the exact operand pair `lowerBlockAddress` emits and
// the assembler reads as "bind this symbol to that block".
//
// ⚠ THE OPCODE ALONE IS NOT THE INSTRUMENT, and measuring it as one is how this
// pin first went red against correct code: `goto *tbl[i]` also `lea`s the TABLE's
// own global symbol, so a plain `lea` count over this function is 2 with zero
// block addresses materialized. The BlockRef is what distinguishes them. The
// opcode still comes off the TARGET's mnemonic table rather than being assumed —
// a lookup that silently failed would make every "no lea" assertion vacuous.
[[nodiscard]] std::uint32_t countBlockAddressLea(Lir const& L, TargetSchema const& t) {
    auto const leaOp = t.opcodeByMnemonic("lea");
    EXPECT_TRUE(leaOp.has_value())
        << "x86_64 must declare a `lea` — without it this instrument counts "
           "nothing and every assertion built on it is vacuous";
    if (!leaOp.has_value()) return UINT32_MAX;
    std::uint32_t n = 0;
    for (std::uint32_t fi = 0; fi < L.moduleFuncCount(); ++fi) {
        LirFuncId const fn = L.funcAt(fi);
        for (std::uint32_t bi = 0; bi < L.funcBlockCount(fn); ++bi) {
            LirBlockId const b = L.funcBlockAt(fn, bi);
            for (std::uint32_t ii = 0; ii < L.blockInstCount(b); ++ii) {
                LirInstId const inst = L.blockInstAt(b, ii);
                if (L.instOpcode(inst) != *leaOp) continue;
                bool sym = false, blk = false;
                for (LirOperand const& o : L.instOperands(inst)) {
                    if (o.kind == LirOperandKind::SymbolRef) sym = true;
                    if (o.kind == LirOperandKind::BlockRef)  blk = true;
                }
                if (sym && blk) ++n;
            }
        }
    }
    return n;
}

// The MIR-side export symbols, so a LIR binding can be checked against the symbol
// the DATA actually relocates against rather than against itself.
[[nodiscard]] std::unordered_set<std::uint32_t> exportSymbols(Mir const& m) {
    std::unordered_set<std::uint32_t> out;
    for (std::uint32_t fi = 0; fi < m.moduleFuncCount(); ++fi) {
        MirFuncId const fn = m.funcAt(fi);
        for (std::uint32_t bi = 0; bi < m.funcBlockCount(fn); ++bi) {
            MirBlockId const b = m.funcBlockAt(fn, bi);
            for (std::uint32_t ii = 0; ii < m.blockInstCount(b); ++ii) {
                MirInstId const inst = m.blockInstAt(b, ii);
                if (m.instOpcode(inst) != MirOpcode::BlockAddressExport) continue;
                out.insert(m.blockAddressExportSymbol(inst).v);
            }
        }
    }
    return out;
}

} // namespace

TEST(LirLabelAddressStaticTable, ExportOnlyAddressPublishesABindingAndNoInstruction) {
    auto L = lowerCToLir(
        "int f(int i) {\n"
        "  static void *const tbl[] = {&&L0, &&L1};\n"
        "  goto *tbl[i];\n"
        "L0:\n"
        "  return 1;\n"
        "L1:\n"
        "  return 2;\n"
        "}\n");
    ASSERT_TRUE(L.mir.ok)
        << "MIR: " << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    ASSERT_TRUE(L.lir.ok)
        << "LIR: " << (L.lirReporter.all().empty() ? "" : L.lirReporter.all()[0].actual);

    // TWO bindings — one per exported label — and each names a symbol the MIR
    // export minted, so the data and the code cannot be naming different symbols.
    ASSERT_EQ(L.lir.blockSymbolBindings.size(), 2u)
        << "each block reachable only from static data needs its byte offset "
           "published; without a binding the linker sees an undefined symbol";
    auto const syms = exportSymbols(L.mir.mir);
    std::unordered_set<std::uint32_t> bound;
    for (auto const& b : L.lir.blockSymbolBindings) {
        EXPECT_EQ(b.funcIndex, 0u) << "both labels live in the only function";
        EXPECT_TRUE(syms.count(b.symbol.v) != 0)
            << "a binding names symbol " << b.symbol.v
            << ", which no MIR BlockAddressExport declared";
        bound.insert(b.symbol.v);
    }
    EXPECT_EQ(bound.size(), 2u) << "two labels, two distinct symbols";

    // ★ AND NO INSTRUCTION IS EMITTED FOR THEM. gcc emits no `lea` for this shape
    // either (✔measured, gcc 13.2 `-O0 -S`): the address is READ from the table,
    // never materialized. A dead `lea` per exported label would still be correct,
    // which is exactly why it needs a pin — nothing else would ever notice it.
    EXPECT_EQ(countBlockAddressLea(L.lir.lir, *L.target), 0u)
        << "an address that only ever reaches static data materializes no register";
}

TEST(LirLabelAddressStaticTable, BodyAndTableReferencesShareOneSymbol) {
    // The SAME label addressed twice over: once materialized in the body, once
    // relocated from the table. `mintBlockSymbol` is memoized per BLOCK and primed
    // from the export before any function lowers, so both resolve to one symbol.
    auto L = lowerCToLir(
        "int f(int i) {\n"
        "  static void *const tbl[] = {&&A};\n"
        "  void *body = &&A;\n"
        "  if (i == 2) goto *body;\n"
        "  goto *tbl[i];\n"
        "A:\n"
        "  return 5;\n"
        "}\n");
    ASSERT_TRUE(L.mir.ok)
        << "MIR: " << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    ASSERT_TRUE(L.lir.ok)
        << "LIR: " << (L.lirReporter.all().empty() ? "" : L.lirReporter.all()[0].actual);

    auto const syms = exportSymbols(L.mir.mir);
    ASSERT_EQ(syms.size(), 1u) << "one label exports one symbol";

    // The BODY reference is a real value, so its `lea` IS emitted — and the
    // assembler binds the symbol from that instruction's trailing BlockRef. The
    // export therefore needs no descriptor of its own here; publishing one anyway
    // would be harmless (the pipeline's `alreadyBound` seed collapses it) but the
    // instruction is what proves the two references converged on ONE symbol.
    EXPECT_GE(countBlockAddressLea(L.lir.lir, *L.target), 1u)
        << "a body-materialized `&&label` still emits its address instruction";
    for (auto const& b : L.lir.blockSymbolBindings) {
        EXPECT_TRUE(syms.count(b.symbol.v) != 0)
            << "any binding emitted must name the one exported symbol, never a "
               "second one minted for the same block";
    }
}

TEST(LirLabelAddressStaticTable, PreMintedSymbolsAreNotReissuedToOtherBlocks) {
    // ★ THE COLLISION PIN. A pre-minted block symbol is neither a function, a
    // global, nor an extern, so the block-symbol minter's high-water scan walks
    // straight past it. If the seed does not account for the pre-minted ceiling,
    // an ORDINARY computed-goto label in the same module is handed the SAME id —
    // two definitions of one symbol, which the linker refuses.
    //
    // The second function's `&&B` takes the ordinary path, so its symbol comes off
    // the minter while the first function's came off HIR→MIR.
    auto L = lowerCToLir(
        "int f(int i) {\n"
        "  static void *const tbl[] = {&&L0, &&L1};\n"
        "  goto *tbl[i];\n"
        "L0:\n"
        "  return 1;\n"
        "L1:\n"
        "  return 2;\n"
        "}\n"
        "int g(int i) {\n"
        "  void *p = &&B;\n"
        "  void *q = &&C;\n"
        "  goto *(i ? p : q);\n"
        "B:\n"
        "  return 3;\n"
        "C:\n"
        "  return 4;\n"
        "}\n");
    ASSERT_TRUE(L.mir.ok)
        << "MIR: " << (L.mirReporter.all().empty() ? "" : L.mirReporter.all()[0].actual);
    ASSERT_TRUE(L.lir.ok)
        << "LIR: " << (L.lirReporter.all().empty() ? "" : L.lirReporter.all()[0].actual);

    auto const preMinted = exportSymbols(L.mir.mir);
    ASSERT_EQ(preMinted.size(), 2u);

    // Every SymbolRef operand the LIR carries for a block address in `g` must be
    // OUTSIDE the pre-minted set. Reading the operands is the direct statement of
    // the invariant; counting symbols would not distinguish a collision from a
    // coincidence.
    auto const leaOp = L.target->opcodeByMnemonic("lea");
    ASSERT_TRUE(leaOp.has_value());
    Lir const& lir = L.lir.lir;
    ASSERT_GE(lir.moduleFuncCount(), 2u);
    LirFuncId const g = lir.funcAt(1);
    std::uint32_t seen = 0;
    for (std::uint32_t bi = 0; bi < lir.funcBlockCount(g); ++bi) {
        LirBlockId const b = lir.funcBlockAt(g, bi);
        for (std::uint32_t ii = 0; ii < lir.blockInstCount(b); ++ii) {
            LirInstId const inst = lir.blockInstAt(b, ii);
            if (lir.instOpcode(inst) != *leaOp) continue;
            for (LirOperand const& o : lir.instOperands(inst)) {
                if (o.kind != LirOperandKind::SymbolRef) continue;
                ++seen;
                EXPECT_TRUE(preMinted.count(o.symbolV) == 0)
                    << "block symbol " << o.symbolV << " in `g` collides with a "
                       "symbol pre-minted for `f`'s static label table";
            }
        }
    }
    EXPECT_GE(seen, 2u)
        << "`g` takes two label addresses — if this counted none, the pin above "
           "asserted nothing";
}
