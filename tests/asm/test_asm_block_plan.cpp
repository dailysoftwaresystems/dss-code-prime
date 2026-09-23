// ★★★ THE `.s` BLOCK PLAN — every block of a hand-written function is created
// in TEXT order, including the two kinds that begin WITHOUT a label (P68 round
// 8: D-ASM-CONDITIONAL-BRANCH-FALLTHROUGH-LAID-OUT-AT-THE-FUNCTION-END and
// D-ASM-INSTRUCTION-AFTER-A-TERMINATOR-REFUSED).
//
// LIR lays a function's blocks out in the order they are created. A block
// begins at a code label, at the line after a conditional branch (its false
// edge) and at the line after any other terminator (code no fall-through
// reaches). The `.s` walker created the first kind up front and minted the
// other two when it met them, so a conditional branch's fall-through code was
// laid out after the function's LAST label — ✔MEASURED 2026-09-23, gas's
// 52-byte `main` of `examples/asm/asm_x86_64_numeric_local_labels` was 139
// bytes under DSS — and a line after `ret`/`jmp` was refused outright although
// gas 2.42 and clang 18.1.3 assemble and run it (x86_64 and aarch64).
//
// ⚠ CONFIG-LEVEL (the shipped dialects and targets, and two corpus sources):
// run through ctest, which points DSS_CONFIG_ROOT at the run's snapshot.

#include "asm/asm_template_to_lir.hpp"
#include "asm_text_fixture.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_pass_util.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using dss::test_support::asm_text::LoweringRun;
using dss::test_support::asm_text::lowerAsmText;
using dss::test_support::asm_text::messages;
using dss::test_support::asm_text::parseMessages;
using dss::test_support::asm_text::parsedCleanly;
using dss::test_support::asm_text::shippedDialectDoc;

namespace {

struct Dialect {
    std::string_view language;
    std::string_view target;
    std::string_view example;   // the numeric-local-labels corpus source
};
constexpr Dialect kX86{"asm-x86_64-att", "x86_64",
                       "examples/asm/asm_x86_64_numeric_local_labels/main.s"};
constexpr Dialect kArm{"asm-arm64-gas", "arm64",
                       "examples/asm/asm_arm64_numeric_local_labels/main.s"};

[[nodiscard]] std::string readRepoFile(std::string_view rel) {
    std::ifstream in{dss::test::repoRoot() / std::string{rel},
                     std::ios::binary};
    if (!in) throw std::runtime_error{"cannot open " + std::string{rel}};
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

[[nodiscard]] std::unique_ptr<LoweringRun> lower(Dialect const& d,
                                                 std::string const& source) {
    return lowerAsmText(shippedDialectDoc(d.language), source, d.target);
}

[[nodiscard]] std::string diagnosticsOf(LoweringRun const& run) {
    return parseMessages(run) + messages(run);
}

[[nodiscard]] TargetTerminatorKind kindOf(LoweringRun const& run,
                                          LirInstId inst) {
    auto const* info = run.target->opcodeInfo(run.module->lir.instOpcode(inst));
    return info == nullptr ? TargetTerminatorKind::None : info->terminatorKind;
}

// The index of `block` in `fn`'s layout, or nullopt.
[[nodiscard]] std::optional<std::uint32_t>
layoutIndexOf(Lir const& lir, LirFuncId fn, LirBlockId block) {
    for (std::uint32_t i = 0; i < lir.funcBlockCount(fn); ++i) {
        if (lir.funcBlockAt(fn, i).v == block.v) return i;
    }
    return std::nullopt;
}

}  // namespace

// ══ A conditional branch falls into the block laid out NEXT ══════════════════
//
// ★ THE LAYOUT PIN. Every conditional branch of the numeric-local-labels
// example's `main` — each followed in the text by the line it falls into — must
// have that line's block as the NEXT block of the function, and where the
// target declares the shorter fall-through form (x86 `jcc`, arm64 `b.cond`)
// the branch must be written in it: its false edge is layout, not a jump.
// ✔MEASURED 2026-09-23 against gas 2.42's build of the same file: `main` is 52
// bytes under gas; DSS's is laid out in gas's order, and the bytes that remain
// are the synthesized jumps into a label reached by falling (F-d cause 1) and
// x86 branches that are always rel32 (cause 4) — round-9 rows.
// ⚠ arm64's `cbz`/`cbnz` still write their false edge as a second word — the
// shipped target declares no fall-through form for them (cause 2) — so the
// elision half is asserted only where the target declares the form.
TEST(AsmBlockPlan, AConditionalBranchFallsIntoTheBlockLaidOutNext) {
    for (Dialect const* d : {&kX86, &kArm}) {
        SCOPED_TRACE(d->language);
        auto const run = lower(*d, readRepoFile(d->example));
        ASSERT_TRUE(parsedCleanly(*run)) << diagnosticsOf(*run);
        ASSERT_TRUE(run->module.has_value()) << diagnosticsOf(*run);
        Lir const& lir = run->module->lir;
        LirFuncId const fn = lir.funcAt(0);
        std::size_t condBrs = 0;
        std::size_t elided  = 0;
        for (std::uint32_t i = 0; i < lir.funcBlockCount(fn); ++i) {
            LirBlockId const blk  = lir.funcBlockAt(fn, i);
            LirInstId const  term = lir.blockTerminator(blk);
            if (kindOf(*run, term) != TargetTerminatorKind::CondBr) continue;
            ++condBrs;
            auto const succs = lir.blockSuccessors(blk);
            ASSERT_EQ(succs.size(), 2u);
            ASSERT_LT(i + 1, lir.funcBlockCount(fn))
                << "a conditional branch ends the function's last block";
            EXPECT_EQ(succs[1].v, lir.funcBlockAt(fn, i + 1).v)
                << "block " << i << ": the false edge is not the block laid "
                   "out next — the fall-through code was placed elsewhere";
            auto const ops = lir.instOperands(term);
            std::size_t blockRefs = 0;
            for (auto const& o : ops) {
                if (o.kind == LirOperandKind::BlockRef) ++blockRefs;
            }
            bool const onlyBlockRefs = blockRefs == ops.size();
            if (onlyBlockRefs
                && lir_pass_util::declaresFallthroughBranchForm(
                       *run->target, lir.instOpcode(term), succs.size())) {
                EXPECT_EQ(blockRefs, 1u)
                    << "block " << i << ": the false edge is the next block and "
                       "the target declares the fall-through form, yet the "
                       "branch still writes it";
                ++elided;
            }
        }
        // Non-vacuous: the example branches conditionally five times on each
        // processor, and x86 writes every one of them in the short form.
        EXPECT_EQ(condBrs, 5u);
        if (d == &kX86) EXPECT_EQ(elided, 5u);
    }
}

// ══ An instruction after a terminator begins a block where the text put it ══
//
// ✔MEASURED 2026-09-23: gas 2.42 and clang 18.1.3 assemble this body and run
// it to 42 on x86_64 and aarch64 (qemu). DSS refused the dead `add` after the
// `jmp` as code "it cannot place in a basic block".
TEST(AsmBlockPlan, AnInstructionAfterATerminatorBeginsABlockWhereTheTextPutsIt) {
    struct Case {
        Dialect const*   d;
        std::string_view source;
    };
    Case const cases[] = {
        {&kX86, ".globl main\n.type main, @function\nmain:\n"
                "  movl $40, %eax\n"
                "  jmp 1f\n"
                "  addl $100, %eax\n"          // dead: after an unconditional jump
                "1: cmpl $40, %eax\n"
                "  jne 2f\n"
                "  addl $2, %eax\n"            // the false edge
                "  ret\n"
                "  addl $1000, %eax\n"         // dead: after a return
                "2: movl $1, %eax\n"
                "  ret\n"},
        {&kArm, ".globl main\n.type main, %function\nmain:\n"
                "  mov w0, #40\n"
                "  b 1f\n"
                "  add w0, w0, #100\n"
                "1: cmp w0, #40\n"
                "  b.ne 2f\n"
                "  add w0, w0, #2\n"
                "  ret\n"
                "  add w0, w0, #1000\n"
                "2: mov w0, #1\n"
                "  ret\n"},
    };
    for (Case const& c : cases) {
        SCOPED_TRACE(c.d->language);
        auto const run = lower(*c.d, std::string{c.source});
        ASSERT_TRUE(parsedCleanly(*run)) << diagnosticsOf(*run);
        ASSERT_TRUE(run->module.has_value())
            << "code after a terminator must lower: " << diagnosticsOf(*run);
        Lir const& lir = run->module->lir;
        LirFuncId const fn = lir.funcAt(0);
        // entry | dead | `1:` | false edge | dead | `2:` — six blocks in the
        // TEXT's order.
        ASSERT_EQ(lir.funcBlockCount(fn), 6u);
        auto const block = [&](std::uint32_t i) { return lir.funcBlockAt(fn, i); };
        // The jump over the dead line lands on `1:`, laid out right after it.
        ASSERT_EQ(lir.blockSuccessors(block(0)).size(), 1u);
        EXPECT_EQ(layoutIndexOf(lir, fn, lir.blockSuccessors(block(0))[0]), 2u);
        // The dead line's block sits between the jump and `1:` and falls into
        // `1:`; nothing branches to it.
        ASSERT_EQ(lir.blockSuccessors(block(1)).size(), 1u);
        EXPECT_EQ(lir.blockSuccessors(block(1))[0].v, block(2).v);
        // `1:`'s conditional branch falls into the block laid out next.
        ASSERT_EQ(lir.blockSuccessors(block(2)).size(), 2u);
        EXPECT_EQ(lir.blockSuccessors(block(2))[1].v, block(3).v);
        EXPECT_EQ(layoutIndexOf(lir, fn, lir.blockSuccessors(block(2))[0]), 5u);
        // The dead line after `ret` sits between it and `2:`.
        EXPECT_EQ(lir.blockSuccessors(block(4))[0].v, block(5).v);
        for (std::uint32_t i = 0; i < lir.funcBlockCount(fn); ++i) {
            for (LirBlockId const s : lir.blockSuccessors(block(i))) {
                EXPECT_NE(s.v, block(1).v) << "block " << i << " reaches dead code";
                EXPECT_NE(s.v, block(4).v) << "block " << i << " reaches dead code";
            }
        }
    }
}

// ══ Code nothing reaches, ending the function, ends in the target's trap ══════
//
// ✔MEASURED 2026-09-23: gas 2.42 and clang 18.1.3 assemble `ret` followed by
// `nop` and an `add` as the last lines of `main` and run it to 42 (x86_64 and
// aarch64). No control arrives at the end of those lines, so falling off it has
// no meaning to refuse; the block ends in the target's unreachable trap.
TEST(AsmBlockPlan, CodeNothingReachesAtTheEndClosesWithTheTargetsTrap) {
    struct Case {
        Dialect const*   d;
        std::string_view source;
    };
    Case const cases[] = {
        {&kX86, ".globl main\n.type main, @function\nmain:\n"
                "  movl $42, %eax\n  ret\n  nop\n  addl $1, %eax\n"},
        {&kArm, ".globl main\n.type main, %function\nmain:\n"
                "  mov w0, #42\n  ret\n  nop\n  add w0, w0, #1\n"},
    };
    for (Case const& c : cases) {
        SCOPED_TRACE(c.d->language);
        auto const run = lower(*c.d, std::string{c.source});
        ASSERT_TRUE(parsedCleanly(*run)) << diagnosticsOf(*run);
        ASSERT_TRUE(run->module.has_value()) << diagnosticsOf(*run);
        Lir const& lir = run->module->lir;
        LirFuncId const fn = lir.funcAt(0);
        ASSERT_EQ(lir.funcBlockCount(fn), 2u);
        LirBlockId const tail = lir.funcBlockAt(fn, 1);
        EXPECT_EQ(lir.blockInstCount(tail), 3u) << "nop, add, and the trap";
        EXPECT_EQ(kindOf(*run, lir.blockTerminator(tail)),
                  TargetTerminatorKind::Unreachable);
        EXPECT_EQ(kindOf(*run, lir.blockTerminator(lir.funcBlockAt(fn, 0))),
                  TargetTerminatorKind::Return);
    }
}

// ══ …but control that DOES reach the end is still refused ════════════════════
//
// CONTROL for the arm above: a conditional branch whose false edge passes the
// function's last line leaves control running off the end — the fall-off
// refusal every unterminated function gets, naming the branch. And a line that
// falls off the end with no terminator at all keeps its refusal too.
TEST(AsmBlockPlan, AFalseEdgePastTheLastLineIsRefused) {
    auto const branch = lower(kX86, ".globl main\n.type main, @function\nmain:\n"
                                    "1: cmpl $0, %eax\n  jne 1b\n");
    ASSERT_TRUE(parsedCleanly(*branch)) << diagnosticsOf(*branch);
    EXPECT_FALSE(branch->module.has_value());
    EXPECT_NE(messages(*branch).find("conditional branch whose false edge falls "
                                     "off the end"),
              std::string::npos)
        << messages(*branch);

    auto const plain = lower(kX86, ".globl main\n.type main, @function\nmain:\n"
                                   "  movl $1, %eax\n");
    ASSERT_TRUE(parsedCleanly(*plain)) << diagnosticsOf(*plain);
    EXPECT_FALSE(plain->module.has_value());
    EXPECT_NE(messages(*plain).find("no terminating instruction"),
              std::string::npos)
        << messages(*plain);
}
