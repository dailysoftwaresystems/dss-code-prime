// [[D-CSUBSET-LONG-BRANCH]] — an intra-function branch that outgrows its field.
//
// ★★★ WHAT THIS FILE HAD TO SYNTHESIZE, AND WHY THERE WAS NO OTHER WAY.
// AArch64 `B.cond` carries a 19-bit word-scaled displacement: ±1 MiB. Nothing
// this compiler has ever emitted comes within three orders of magnitude of
// that, so the refusal it used to produce was UNREACHABLE from the corpus and
// the escape that replaces it is unreachable too. The only fixture that can
// reach either is a function body BUILT to exceed the reach — a quarter of a
// million padding instructions, laid out so the displacements land on CHOSEN
// values rather than merely large ones.
//
// ⚠ THE FIXTURE SYNTHESIZES THE NEGATIVE AS WELL AS THE POSITIVE, because a
// relaxation that fired on every function would "fix" the rare case by
// perturbing every common one. `NaturallyInReachJccIsByteIdenticalToThePreRelaxationForm`
// is the control arm: the six-word skeleton whose branches all fit must emit
// the SAME BYTES it emitted before relaxation existed, and those bytes are
// pinned as literals measured against the ARM ARM.
//
// The layout below is arithmetic, not decoration. Three properties are chosen:
//
//   (1) the INNER conditional is out of `Imm19` reach by exactly ONE word, so
//       it must escape;
//   (2) the OUTER conditional is EXACTLY AT the `Imm19` limit before anything
//       escapes, so it must NOT escape on the first pass;
//   (3) the inner escape's extra word sits BETWEEN the outer branch and its
//       target, so taking it pushes the outer branch out of reach.
//
// (3) is the whole point, and it produces a fingerprint no single-pass
// resolver can forge: the outer branch ends up in its ESCAPE form even though
// its settled displacement would have fit the field it escaped from. Neither
// layout a one-shot resolver could have examined — not the unrelaxed one, not
// the final one — justifies escaping it. Only the INTERMEDIATE layout does.
//
// ⓘ COST: ~1 MiB of emitted code, encoded three times (pass 0 measures, pass 1
// re-measures after the inner escape, pass 2 settles). That is the price of
// reaching a ±1 MiB edge at all, and it is paid once, in one test.

#include "asm/asm.hpp"
#include "asm/format/fixed32.hpp"
#include "asm/format/walker_util.hpp"
#include "asm/format/x86_variable.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_reg.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace dss;

namespace {

// ── The AArch64 facts this file reads BACK OUT of emitted bytes ──────────
// Decoding, not encoding: the assembler's job is to produce these words and
// the test's job is to disagree when it does not. Every mask here is the ARM
// ARM's, quoted so an assertion can name the field it read.
constexpr std::uint32_t kBCondOpcodeMask = 0xFF000010u;  // B.cond signature
constexpr std::uint32_t kBCondOpcode     = 0x54000000u;
constexpr std::uint32_t kBOpcodeMask     = 0xFC000000u;  // B signature
constexpr std::uint32_t kBOpcode         = 0x14000000u;

// The reach of the two block-relative fields, in SCALED (word) units — read
// from the substrate's OWN geometry table, so the fixture and the resolver
// can never disagree about where the edge is.
std::int64_t imm19Max() {
    return walker_util::blockRelFieldMax(walker_util::blockRelFieldGeometry(
        walker_util::BlockRelPatchKind::Arm64Imm19));
}

std::uint32_t wordAt(std::vector<std::uint8_t> const& b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off])
         | (static_cast<std::uint32_t>(b[off + 1]) << 8)
         | (static_cast<std::uint32_t>(b[off + 2]) << 16)
         | (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

std::int64_t signExtend(std::uint32_t field, std::uint32_t width) {
    std::uint32_t const signBit = 1u << (width - 1);
    if ((field & signBit) != 0)
        return static_cast<std::int64_t>(field) - (std::int64_t{1} << width);
    return static_cast<std::int64_t>(field);
}

std::int64_t  imm19Of(std::uint32_t w) { return signExtend((w >> 5) & 0x7FFFFu, 19); }
std::int64_t  imm26Of(std::uint32_t w) { return signExtend(w & 0x3FFFFFFu, 26); }
std::uint32_t condOf (std::uint32_t w) { return w & 0xFu; }

struct Arm64Kit {
    std::shared_ptr<TargetSchema const> schema;
    std::uint16_t jccOp{}, jmpOp{}, subOp{}, retOp{}, cmpOp{};
    LirReg x0{}, xzr{};
};

Arm64Kit loadArm64Kit() {
    Arm64Kit kit;
    auto sOpt = TargetSchema::loadShipped("arm64");
    if (!sOpt.has_value()) return kit;
    kit.schema = *sOpt;
    auto const& s = *kit.schema;
    auto opc = [&](char const* m) {
        auto o = s.opcodeByMnemonic(m);
        EXPECT_TRUE(o.has_value()) << "arm64 missing opcode '" << m << "'";
        return o.value_or(0);
    };
    kit.jccOp = opc("jcc");
    kit.jmpOp = opc("jmp");
    kit.subOp = opc("sub");
    kit.retOp = opc("ret");
    kit.cmpOp = opc("cmp");
    auto xreg = [&](char const* name) {
        auto ord = s.registerByName(name);
        EXPECT_TRUE(ord.has_value()) << "arm64 missing register '" << name << "'";
        return makePhysicalReg(static_cast<std::uint32_t>(ord.value_or(0)),
                               LirRegClass::GPR);
    };
    kit.x0  = xreg("x0");
    kit.xzr = xreg("xzr");
    return kit;
}

// ── THE OVERSIZED BODY ───────────────────────────────────────────────────
//
// Block layout, as emitted on PASS 0 (before any escape is taken). Every
// block is sealed by a terminator, because `LirBuilder::beginBlock` on an
// unsealed block is a producer-contract abort, not a diagnostic.
//
//   b0    +0  : jcc(sgt) bEnd, bMid      2 words — B.cond(Imm19→bEnd) ; B(Imm26→bMid)
//   bMid  +8  : jcc(ne)  bFar            1 word  — the D-OPT-JCC-FALLTHROUGH form,
//                                                  ifFalse = bPad = the next block
//   bPad  +12 : sub ×kPad ; jmp bEnd     kPad+1 words
//   bEnd      : ret                      1 word   <- the OUTER target
//   bTail     : sub ×kTail ; jmp bFar    kTail+1 words
//   bFar      : ret                      1 word   <- the INNER target
//
// Pass-0 displacements, in words:
//   outer (Imm19 @+0) -> bEnd = (16 + 4·kPad)/4          = 4 + kPad
//   inner (Imm19 @+8) -> bFar = (16 + 4·kPad + 4·kTail)/4 = 4 + kPad + kTail
//
// Choose  4 + kPad          == imm19Max      (outer EXACTLY in reach)
//         4 + kPad + kTail  == imm19Max + 1  (inner out of reach by ONE word)
// ⇒      kPad  = imm19Max - 4,  kTail = 1.
struct OversizedBuild {
    Lir           lir;
    LirBlockId    bEnd{}, bFar{};
    std::uint64_t padCount  = 0;
    std::uint64_t tailCount = 0;
};

OversizedBuild buildOversizedFunction(Arm64Kit const& kit) {
    std::uint64_t const kPad  = static_cast<std::uint64_t>(imm19Max()) - 4u;
    std::uint64_t const kTail = 1u;

    LirBuilder b{*kit.schema};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const b0    = b.createBlock();
    LirBlockId const bMid  = b.createBlock();
    LirBlockId const bPad  = b.createBlock();
    LirBlockId const bEnd  = b.createBlock();
    LirBlockId const bTail = b.createBlock();
    LirBlockId const bFar  = b.createBlock();

    // b0 — the OUTER conditional, two-word form (both successors named).
    b.beginBlock(b0);
    {
        std::array<LirOperand, 2> ops{LirOperand::makeBlockRef(bEnd.v),
                                      LirOperand::makeBlockRef(bMid.v)};
        (void)b.addCondBr(kit.jccOp, ops, bEnd, bMid,
                          static_cast<std::uint32_t>(TargetCondCode::Sgt));
    }
    // bMid — the INNER conditional, ONE-word fallthrough form: only the taken
    // successor is an operand, and the not-taken one IS the next-laid-out
    // block. This is the shape whose escape must INVERT the condition,
    // because the escape word lands exactly where the fallthrough used to.
    b.beginBlock(bMid);
    {
        std::array<LirOperand, 1> ops{LirOperand::makeBlockRef(bFar.v)};
        (void)b.addCondBr(kit.jccOp, ops, bFar, bPad,
                          static_cast<std::uint32_t>(TargetCondCode::Ne));
    }
    auto emitPadding = [&](LirBlockId blk, std::uint64_t n, LirBlockId next) {
        b.beginBlock(blk);
        std::array<LirOperand, 2> subOps{LirOperand::makeReg(kit.x0),
                                         LirOperand::makeReg(kit.x0)};
        for (std::uint64_t i = 0; i < n; ++i)
            (void)b.addInst(kit.subOp, kit.x0, subOps);
        (void)b.addBr(kit.jmpOp, next);
    };
    emitPadding(bPad, kPad, bEnd);
    b.beginBlock(bEnd);
    (void)b.addReturn(kit.retOp, {});
    emitPadding(bTail, kTail, bFar);
    b.beginBlock(bFar);
    (void)b.addReturn(kit.retOp, {});

    OversizedBuild built;
    built.lir       = std::move(b).finish();
    built.bEnd      = bEnd;
    built.bFar      = bFar;
    built.padCount  = kPad;
    built.tailCount = kTail;
    return built;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// THE CONTROL ARM — the naturally-in-reach function must not move a byte.
// ─────────────────────────────────────────────────────────────────────────
//
// ★★★ THIS IS THE ASSERTION THAT MAKES THE FIX A FIX RATHER THAN A REWRITE.
// The six words below are the ones `D-AS3-BLOCK-REL-IMM19-26` measured against
// the ARM ARM when it closed, reproduced here so a relaxation that fires too
// eagerly — on a branch that fits, on the wrong wire, on every conditional —
// is caught by a byte comparison rather than by a semantic argument.
TEST(AsmLongBranch, NaturallyInReachJccIsByteIdenticalToThePreRelaxationForm) {
    Arm64Kit const kit = loadArm64Kit();
    ASSERT_NE(kit.schema, nullptr);

    LirBuilder b{*kit.schema};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const header = b.createBlock();
    LirBlockId const body   = b.createBlock();
    LirBlockId const exit   = b.createBlock();

    b.beginBlock(header);
    {
        std::array<LirOperand, 2> cmpOps{LirOperand::makeReg(kit.x0),
                                         LirOperand::makeReg(kit.xzr)};
        (void)b.addInst(kit.cmpOp, InvalidLirReg, cmpOps);
        std::array<LirOperand, 2> jccOps{LirOperand::makeBlockRef(body.v),
                                         LirOperand::makeBlockRef(exit.v)};
        (void)b.addCondBr(kit.jccOp, jccOps, body, exit,
                          static_cast<std::uint32_t>(TargetCondCode::Sgt));
    }
    b.beginBlock(body);
    {
        std::array<LirOperand, 2> subOps{LirOperand::makeReg(kit.x0),
                                         LirOperand::makeReg(kit.x0)};
        (void)b.addInst(kit.subOp, kit.x0, subOps);
        (void)b.addBr(kit.jmpOp, header);
    }
    b.beginBlock(exit);
    (void)b.addReturn(kit.retOp, {});

    Lir lir = std::move(b).finish();
    std::vector<MirInstId> lirToMir(lir.instCount(), InvalidMirInst);
    DiagnosticReporter rep;
    auto mod = assemble(lir, *kit.schema, lirToMir, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(mod.functions.empty());

    //   [+0x00] cmp x0, xzr      0xEB1F001F
    //   [+0x04] b.gt body        0x5400004C   imm19=+2, cond GT=0xC
    //   [+0x08] b   exit         0x14000003   imm26=+3
    //   [+0x0C] sub x0, x0, x0   0xCB000000
    //   [+0x10] b   header       0x17FFFFFC   imm26=-4 (back-edge)
    //   [+0x14] ret              0xD65F03C0
    static constexpr std::array<std::uint32_t, 6> kExpected{
        0xEB1F001Fu, 0x5400004Cu, 0x14000003u,
        0xCB000000u, 0x17FFFFFCu, 0xD65F03C0u};
    auto const& bytes = mod.functions[0].bytes;
    ASSERT_EQ(bytes.size(), kExpected.size() * 4u)
        << "an in-reach function must emit exactly its template's words — a "
           "size change here means relaxation fired on a branch that fits";
    for (std::size_t i = 0; i < kExpected.size(); ++i)
        EXPECT_EQ(wordAt(bytes, i * 4), kExpected[i])
            << "word " << i << " diverged from the pre-relaxation encoding";
}

// ─────────────────────────────────────────────────────────────────────────
// THE POSITIVE ARM — the oversized body assembles, and both escapes are real.
// ─────────────────────────────────────────────────────────────────────────
TEST(AsmLongBranch, OutOfReachConditionalBranchesEscapeToTheWiderField) {
    Arm64Kit const kit = loadArm64Kit();
    ASSERT_NE(kit.schema, nullptr);

    OversizedBuild built = buildOversizedFunction(kit);
    std::vector<MirInstId> lirToMir(built.lir.instCount(), InvalidMirInst);
    DiagnosticReporter rep;
    auto mod = assemble(built.lir, *kit.schema, lirToMir, rep);

    // ⚠ THE HEADLINE ASSERTION. Before this cycle this function could not be
    // assembled at all: the resolver refused with `A_ImmediateOperandOutOfRange`
    // naming this anchor, and dropped the whole function's bytes.
    // The refusal this replaces is the thing a reader will want to see when
    // this reds, so carry it into the failure message rather than making
    // them re-run under a debugger to find out WHICH branch complained.
    std::string diagnostics;
    for (auto const& d : rep.all())
        diagnostics += "\n    " + d.actual;
    EXPECT_EQ(rep.errorCount(), 0u)
        << "a function whose conditional branch exceeds the Imm19 reach must "
           "now assemble via the long-branch escape (D-CSUBSET-LONG-BRANCH)."
           " Diagnostics:" << diagnostics;
    ASSERT_FALSE(mod.functions.empty());
    auto const& bytes = mod.functions[0].bytes;
    ASSERT_FALSE(bytes.empty())
        << "the function was dropped — the escape did not resolve";
    ASSERT_GE(bytes.size(), 20u);

    // ── Settled layout, after BOTH escapes ──
    //   +0  B.gt  ->+2       (imm19 = 2: hop TO the appended escape word)
    //   +4  B     -> bMid    (the original trailing word, Imm26)
    //   +8  B     -> bEnd    (THE OUTER ESCAPE, Imm26)
    //   +12 B.!ne ->+2       (inverted: hop PAST the escape, into bPad)
    //   +16 B     -> bFar    (THE INNER ESCAPE, Imm26)
    //   +20 bPad ...
    std::uint32_t const w0 = wordAt(bytes, 0);
    std::uint32_t const w1 = wordAt(bytes, 4);
    std::uint32_t const w2 = wordAt(bytes, 8);
    std::uint32_t const w3 = wordAt(bytes, 12);
    std::uint32_t const w4 = wordAt(bytes, 16);

    EXPECT_EQ(w0 & kBCondOpcodeMask, kBCondOpcode) << "word 0 must be a B.cond";
    EXPECT_EQ(imm19Of(w0), 2)
        << "the outer conditional's Imm19 must hold the INSTRUCTION-INTERNAL "
           "hop to its escape word, not a block displacement";
    EXPECT_EQ(w1 & kBOpcodeMask, kBOpcode) << "word 1 must be the trailing B";
    EXPECT_EQ(w2 & kBOpcodeMask, kBOpcode) << "word 2 must be the outer escape B";
    EXPECT_EQ(w3 & kBCondOpcodeMask, kBCondOpcode)
        << "word 3 must be the inner B.cond";
    EXPECT_EQ(imm19Of(w3), 2)
        << "the inner conditional must hop PAST its escape word";
    EXPECT_EQ(w4 & kBOpcodeMask, kBOpcode) << "word 4 must be the inner escape B";

    // ⚠ ONE SHAPE KEEPS THE CONDITION, THE OTHER INVERTS IT, AND GETTING IT
    // BACKWARDS IS A SILENT MISCOMPILE RATHER THAN A CRASH. The outer branch
    // has a word of its own macro between it and its escape, so it still
    // means `sgt`; the inner branch's escape lands where its fallthrough was,
    // so it must mean the INVERSE of `ne`. Both nibbles are read from the
    // SCHEMA's own `condCodeEncoding`, so this asserts the mapping rather
    // than a memory of the AArch64 numbers.
    auto nib = [&](TargetCondCode c) {
        auto n = kit.schema->condCodeEncoding(c);
        EXPECT_TRUE(n.has_value());
        return static_cast<std::uint32_t>(n.value_or(0));
    };
    EXPECT_EQ(condOf(w0), nib(TargetCondCode::Sgt))
        << "this shape must PRESERVE the condition — inverting it here sends "
           "the taken and not-taken paths to each other's blocks";
    EXPECT_EQ(condOf(w3), nib(TargetCondCode::Ne) ^ 1u)
        << "this shape must INVERT the condition — preserving it here makes "
           "the fallthrough run the escape branch";

    // ── The escapes must land on the blocks they were aimed at ──
    auto const& offs = mod.functions[0].blockByteOffsets;
    ASSERT_EQ(offs.size(), 6u) << "six blocks were built";
    auto offsetOf = [&](LirBlockId blk) -> std::int64_t {
        auto it = offs.find(blk.v);
        EXPECT_NE(it, offs.end());
        return it == offs.end() ? -1
                                : static_cast<std::int64_t>(it->second);
    };
    EXPECT_EQ(8 + imm26Of(w2) * 4, offsetOf(built.bEnd))
        << "the outer escape's Imm26 must resolve to bEnd's byte offset";
    EXPECT_EQ(16 + imm26Of(w4) * 4, offsetOf(built.bFar))
        << "the inner escape's Imm26 must resolve to bFar's byte offset";

    // ── ★★★ THE FIXED POINT ITSELF ─────────────────────────────────────
    // The inner branch was genuinely out of Imm19 reach and still is.
    EXPECT_GT(imm26Of(w4), imm19Max())
        << "the inner branch must genuinely have been out of Imm19 reach — "
           "if it fits, the fixture stopped testing what it was built for";
    // The outer branch is in its ESCAPE form, and yet its settled
    // displacement FITS the field it escaped from. That is the fingerprint:
    // neither layout a single-pass resolver could have inspected — the
    // unrelaxed one (where this displacement was exactly at the limit) nor
    // the final one (where it is at the limit again) — asks for this branch
    // to be escaped. Only the INTERMEDIATE layout, after the inner escape
    // pushed its target one word further away, does. A resolver that patched
    // once cannot produce these bytes.
    EXPECT_LE(imm26Of(w2), imm19Max())
        << "the outer branch's settled displacement was expected to fit Imm19 "
           "again — if the layout arithmetic moved, re-derive kPad/kTail";
    EXPECT_EQ(imm26Of(w2), imm19Max())
        << "the outer branch must sit EXACTLY at the Imm19 limit in the "
           "settled layout, which is the fixture's whole construction";

    // Total size: the pass-0 body plus exactly two escape words. One word per
    // escaped branch, and no word anywhere else.
    std::uint64_t const expectedWords =
        2u                      // b0's two-word macro
      + 1u                      // bMid's one-word fallthrough form
      + (built.padCount + 1u)   // bPad's padding + its `jmp`
      + 1u                      // bEnd's ret
      + (built.tailCount + 1u)  // bTail's padding + its `jmp`
      + 1u                      // bFar's ret
      + 2u;                     // the two escape words
    EXPECT_EQ(bytes.size(), expectedWords * 4u)
        << "relaxation must add exactly ONE word per escaped branch — a "
           "different total means it fired on a branch that fit, or twice on "
           "one that did not";
}

// ─────────────────────────────────────────────────────────────────────────
// THE UN-ESCAPABLE FIELD, AND WHY IT STILL REFUSES.
// ─────────────────────────────────────────────────────────────────────────
//
// The WIDEST block-relative field an opcode declares has nowhere further to
// go. Escaping AArch64 `B`'s Imm26 needs a literal-pool `LDR`+`BR` and
// escaping x86 `rel32` needs an indirect `jmp` through an absolute address —
// both are different ADDRESSING MODES rather than wider fields, so no
// encoding row declares them and the walker's election correctly finds
// nothing. This pins the ordering the election depends on: if it ever
// inverted, an escape would be elected onto a field that cannot reach.
TEST(AsmLongBranch, TheWidestBlockRelativeFieldHasNoWiderFieldToEscapeInto) {
    using walker_util::BlockRelPatchKind;
    using walker_util::blockRelByteReach;
    EXPECT_GT(blockRelByteReach(BlockRelPatchKind::Arm64Imm26),
              blockRelByteReach(BlockRelPatchKind::Arm64Imm19))
        << "Imm26 must out-reach Imm19 or no escape could ever be elected";
    // ±1 MiB and ±128 MiB, as the ARM ARM states them.
    EXPECT_EQ(blockRelByteReach(BlockRelPatchKind::Arm64Imm19),
              (std::int64_t{1} << 20) - 4);
    EXPECT_EQ(blockRelByteReach(BlockRelPatchKind::Arm64Imm26),
              (std::int64_t{1} << 27) - 4);
    // rel32 is the widest PC-relative branch field x86-64 has, so it is its
    // own ceiling: nothing in the table out-reaches it.
    EXPECT_GE(blockRelByteReach(BlockRelPatchKind::X86Rel32),
              blockRelByteReach(BlockRelPatchKind::Arm64Imm26))
        << "rel32 must be the widest field in the table — if some kind "
           "out-reaches it, the x86 arm has an escape it is not taking";
}

// ─────────────────────────────────────────────────────────────────────────
// THE GEOMETRY TABLE IS THE ONLY SOURCE, AND THESE ARMS MEASURE THAT.
// ─────────────────────────────────────────────────────────────────────────
//
// ★★★ WHAT THESE ADD THAT THE ARMS ABOVE COULD NOT SEE. Relaxation converted
// the SCAN phase and the escape election to read `blockRelFieldGeometry`, and
// the row that shipped it claimed the table was read by the range check, the
// patch write and the escape election alike. THE PATCH WRITE WAS NOT: the
// resolver kept deriving lsb and width from a CPU-named ternary, and spelled
// the scale as a bare shift and x86's PC bias as a bare addition. Two
// components deciding one field's shape from two sources agree only by review
// — and when they stop agreeing the output is a VALID INSTRUCTION WITH THE
// WRONG DISPLACEMENT, which no assertion about diagnostics can catch. The arms
// below tie the RESOLVED BYTES to the table's own numbers, on both ISAs, so the
// two cannot drift apart again without something going red.

// Every kind in the enum must have a geometry row. This is the ratchet on the
// one drift the table cannot catch itself: a kind added to the enum and not to
// `blockRelFieldGeometry` takes the zero-width backstop, which is SAFE (an
// empty signed range refuses every displacement) but wants to be loud here
// rather than discovered by a function that will not assemble.
TEST(AsmLongBranch, EveryBlockRelPatchKindHasAGeometryRow) {
    using walker_util::BlockRelPatchKind;
    for (std::size_t i = 0; i < walker_util::kBlockRelPatchKindCount; ++i) {
        auto const g =
            walker_util::blockRelFieldGeometry(static_cast<BlockRelPatchKind>(i));
        EXPECT_NE(g.width, 0u)
            << "BlockRelPatchKind ordinal " << i << " has no row in "
               "`blockRelFieldGeometry` — it would take the zero-width "
               "backstop and refuse EVERY displacement. Add its row.";
    }
}

// ⚠ THE TABLE SPANS ISAs, SO ITS MAXIMUM IS NOT ANY TARGET'S MAXIMUM — and
// this arm exists because a first draft of the refusal's advice read it that
// way. `X86Rel32` out-reaches `Arm64Imm26`, so "some row reaches further than
// mine" is TRUE for the widest field AArch64 has, and an advice line derived
// from it would have told a reader to declare a wider word that the ISA does
// not have. The reach ordering pinned above is a fact about the TABLE; whether
// a wider field can be DECLARED is a fact about one opcode's encoding row, and
// only the walker can see that. This pins the distinction so the two do not get
// confused again.
TEST(AsmLongBranch, TheTablesWidestReachSpansISAsAndIsNotATargetsCeiling) {
    using walker_util::BlockRelPatchKind;
    using walker_util::blockRelByteReach;
    EXPECT_GT(blockRelByteReach(BlockRelPatchKind::X86Rel32),
              blockRelByteReach(BlockRelPatchKind::Arm64Imm26))
        << "if these ever compare equal, the cross-ISA leak this arm records "
           "stops being observable and the next author will re-introduce it";
}

// ★★★ THE RESOLVED FIELD LANDS WHERE THE TABLE SAYS, NOT WHERE THE RESOLVER
// REMEMBERS. The control arm above pins six words byte-for-byte against the ARM
// ARM; this arm pins the same kind of word against the TABLE's own `lsb`,
// `width`, `scaleLog2` and `pcBias`, so the two spellings of one fact are
// checked against each other. A resolver that writes at a position the table
// does not name reds here.
TEST(AsmLongBranch, Arm64ResolvedFieldLandsAtTheGeometryTablesOwnBitWindow) {
    Arm64Kit const kit = loadArm64Kit();
    ASSERT_NE(kit.schema, nullptr);

    LirBuilder b{*kit.schema};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const header = b.createBlock();
    LirBlockId const body   = b.createBlock();
    LirBlockId const exit   = b.createBlock();
    b.beginBlock(header);
    {
        std::array<LirOperand, 2> jccOps{LirOperand::makeBlockRef(body.v),
                                         LirOperand::makeBlockRef(exit.v)};
        (void)b.addCondBr(kit.jccOp, jccOps, body, exit,
                          static_cast<std::uint32_t>(TargetCondCode::Sgt));
    }
    b.beginBlock(body);
    {
        std::array<LirOperand, 2> subOps{LirOperand::makeReg(kit.x0),
                                         LirOperand::makeReg(kit.x0)};
        (void)b.addInst(kit.subOp, kit.x0, subOps);
        (void)b.addBr(kit.jmpOp, exit);
    }
    b.beginBlock(exit);
    (void)b.addReturn(kit.retOp, {});

    Lir lir = std::move(b).finish();
    std::vector<MirInstId> lirToMir(lir.instCount(), InvalidMirInst);
    DiagnosticReporter rep;
    auto mod = assemble(lir, *kit.schema, lirToMir, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(mod.functions.empty());
    auto const& bytes = mod.functions[0].bytes;
    auto const& offs  = mod.functions[0].blockByteOffsets;
    ASSERT_GE(bytes.size(), 8u);

    auto offsetOf = [&](LirBlockId blk) -> std::int64_t {
        auto it = offs.find(blk.v);
        EXPECT_NE(it, offs.end());
        return it == offs.end() ? -1 : static_cast<std::int64_t>(it->second);
    };
    // Read the field out of the emitted word USING THE TABLE, and compare it to
    // the displacement the table's OWN formula demands. The two agree only if
    // the resolver consulted the same row.
    auto fieldAt = [&](walker_util::BlockRelPatchKind kind,
                       std::uint32_t patchOffset) -> std::int64_t {
        auto const g = walker_util::blockRelFieldGeometry(kind);
        std::uint32_t const mask =
            (g.width >= 32u) ? 0xFFFFFFFFu : ((1u << g.width) - 1u);
        return signExtend((wordAt(bytes, patchOffset) >> g.lsb) & mask,
                          g.width);
    };
    auto expectedDisp = [&](walker_util::BlockRelPatchKind kind,
                            std::uint32_t patchOffset,
                            LirBlockId target) -> std::int64_t {
        auto const g = walker_util::blockRelFieldGeometry(kind);
        return (offsetOf(target)
                - (static_cast<std::int64_t>(patchOffset) + g.pcBias))
               >> g.scaleLog2;
    };
    using walker_util::BlockRelPatchKind;
    EXPECT_EQ(fieldAt(BlockRelPatchKind::Arm64Imm19, 0),
              expectedDisp(BlockRelPatchKind::Arm64Imm19, 0, body))
        << "the B.cond's Imm19 is not at the table's [lsb, lsb+width) window, "
           "or does not carry the table's own (target - pc) >> scale";
    EXPECT_EQ(fieldAt(BlockRelPatchKind::Arm64Imm26, 4),
              expectedDisp(BlockRelPatchKind::Arm64Imm26, 4, exit))
        << "the trailing B's Imm26 is not at the table's window";
    // ⚠ AND THE BITS OUTSIDE THE WINDOW MUST BE UNTOUCHED. A whole-word store
    // would clobber the opcode and the cond nibble into a DIFFERENT valid
    // instruction, which is why the table carries `wholeField` at all.
    std::uint32_t const outsideMask =
        ~(((1u << 19) - 1u) << 5);  // ARM ARM: B.cond's imm19 is bits 5..23
    auto nib = kit.schema->condCodeEncoding(TargetCondCode::Sgt);
    ASSERT_TRUE(nib.has_value());
    EXPECT_EQ(wordAt(bytes, 0) & outsideMask,
              (0x54000000u | static_cast<std::uint32_t>(*nib)))
        << "the read-modify-write disturbed a bit outside the field window";
}

// ─────────────────────────────────────────────────────────────────────────
// THE x86 ARM OF THE SAME RESOLVER — this file's first byte pin on it.
// ─────────────────────────────────────────────────────────────────────────
//
// ⚠ THIS FILE PINNED ONLY AArch64 UNTIL NOW, AND THE RESOLVER IS SHARED. The
// two ISAs used to reach it through two hand-written `switch` arms, so an
// AArch64-only fixture could not see an x86 regression at all; they now reach
// it through ONE body parameterized by a geometry row, and an x86 pin is what
// proves the unification did not move the other ISA. Thirteen bytes, every one
// of them an Intel SDM fact, with the two rel32 fields additionally tied to the
// table's `pcBias` so a change to it cannot pass unobserved.
TEST(AsmLongBranch, X86BlockRelativeBranchesResolveThroughTheSameGeometryRow) {
    auto sOpt = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(sOpt.has_value());
    auto const& s = **sOpt;
    auto opc = [&](char const* m) {
        auto o = s.opcodeByMnemonic(m);
        EXPECT_TRUE(o.has_value()) << "x86_64 missing opcode '" << m << "'";
        return o.value_or(0);
    };
    std::uint16_t const jccOp = opc("jcc");
    std::uint16_t const retOp = opc("ret");

    LirBuilder b{s};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const head  = b.createBlock();
    LirBlockId const taken = b.createBlock();
    LirBlockId const fall  = b.createBlock();
    b.beginBlock(head);
    {
        std::array<LirOperand, 2> ops{LirOperand::makeBlockRef(taken.v),
                                      LirOperand::makeBlockRef(fall.v)};
        (void)b.addCondBr(jccOp, ops, taken, fall,
                          static_cast<std::uint32_t>(TargetCondCode::Sgt));
    }
    b.beginBlock(taken);
    (void)b.addReturn(retOp, {});
    b.beginBlock(fall);
    (void)b.addReturn(retOp, {});

    Lir lir = std::move(b).finish();
    std::vector<MirInstId> lirToMir(lir.instCount(), InvalidMirInst);
    DiagnosticReporter rep;
    auto mod = assemble(lir, s, lirToMir, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(mod.functions.empty());
    auto const& bytes = mod.functions[0].bytes;

    //   [+0x00] 0F 8F 05 00 00 00   jg  .taken    rel32 = 11 - (2+4) = 5
    //   [+0x06] E9    01 00 00 00   jmp .fall     rel32 = 12 - (7+4) = 1
    //   [+0x0B] C3                  ret           <- .taken
    //   [+0x0C] C3                  ret           <- .fall
    // `0F 8F` is `jg` (SDM 0F 8x, cc = F for G/NLE); `E9` the near `jmp`.
    static constexpr std::array<std::uint8_t, 13> kExpected{
        0x0Fu, 0x8Fu, 0x05u, 0x00u, 0x00u, 0x00u,
        0xE9u, 0x01u, 0x00u, 0x00u, 0x00u,
        0xC3u, 0xC3u};
    ASSERT_EQ(bytes.size(), kExpected.size())
        << "the jcc compound is 11 bytes and each `ret` one — a different "
           "total means the encoder selected another variant";
    for (std::size_t i = 0; i < kExpected.size(); ++i)
        EXPECT_EQ(bytes[i], kExpected[i])
            << "byte " << i << " diverged from the SDM encoding";

    // The same two displacements, recomputed from the TABLE. If `pcBias` or
    // `wholeField` moves, the literals above and this recomputation part ways
    // — which is the whole point of stating one fact two ways.
    auto const g = walker_util::blockRelFieldGeometry(
        walker_util::BlockRelPatchKind::X86Rel32);
    EXPECT_TRUE(g.wholeField)
        << "rel32 IS the four bytes at the patch site; a read-modify-write "
           "path here would depend on placeholder bytes it must not read";
    auto const& offs = mod.functions[0].blockByteOffsets;
    auto rel32At = [&](std::uint32_t patchOffset) -> std::int64_t {
        return static_cast<std::int32_t>(wordAt(bytes, patchOffset));
    };
    auto offsetOf = [&](LirBlockId blk) -> std::int64_t {
        auto it = offs.find(blk.v);
        EXPECT_NE(it, offs.end());
        return it == offs.end() ? -1 : static_cast<std::int64_t>(it->second);
    };
    EXPECT_EQ(rel32At(2), offsetOf(taken) - (2 + g.pcBias))
        << "the conditional's rel32 is not the table's (target - (pc + bias))";
    EXPECT_EQ(rel32At(7), offsetOf(fall) - (7 + g.pcBias))
        << "the fallthrough jmp's rel32 is not the table's formula";
}

// ═════════════════════════════════════════════════════════════════════════
// [[D-CSUBSET-LONG-BRANCH]] — THE BRANCH ISLAND: THE WIDEST FIELD'S ESCAPE
// ═════════════════════════════════════════════════════════════════════════
//
// ★★★ WHAT THE ARMS ABOVE CANNOT SEE, AND WHY THIS IS A DIFFERENT FIXTURE.
// Everything above is about a branch escaping into a WIDER field. The field
// that has no wider field — AArch64 `B`'s Imm26, x86-64's `rel32` — escapes by
// standing NEARER instead: a landing pad holding the same branch again, the
// same field twice, chained. No wider field, no absolute address, no
// relocation, no synthetic symbol, and NO SCRATCH REGISTER.
//
// ⚠⚠ READ THIS BEFORE BELIEVING ANY GREEN BELOW — THE COMPOSITION IS STATED,
// NOT IMPLIED. The two real edges are unaffordable as fixtures and this file
// does not pretend otherwise:
//
//   * the arm64 `Imm26` edge needs a function body over 134 MB (the ±1 MiB
//     fixture above already emits 1048596 bytes from 262146 LIR instructions
//     and encodes it three times);
//   * the x86-64 `rel32` edge needs one over 2 GiB, held resident across every
//     relaxation pass.
//
// **NEITHER EDGE IS MEASURED HERE, OR ANYWHERE.** What is measured is:
//
//   (1) THE MACHINERY, end-to-end through the real `assemble()`, on a
//       SYNTHETIC target whose only block-relative field is AArch64's
//       NARROWEST (`imm14`, ±32 KiB — the TBZ/TBNZ displacement). A ±32 KiB
//       edge is a 52 KB function body. The resolver, the fixed point, the
//       placement, the chaining and the jump-over are the SAME CODE at either
//       width: nothing in `asm.cpp` reads a field's identity, only its
//       geometry row.
//   (2) THE GEOMETRY of the real fields, by the byte-for-byte arms above and
//       by `EveryBlockRelPatchKindHasAGeometryRow`.
//   (3) THE ISLAND BODY each REAL target declares, by reading it
//       (`EveryShippedBranchOpcodeDeclaresARegisterFreeIslandBody`).
//
// (1) x (2) x (3) is a composition, and a composition is not an edge
// measurement. It is written down here so no reader has to infer it.
//
// ⚠ AND THE SYNTHETIC FIXTURE CARRIES ITS OWN POSITIVE CONTROL, because an
// instrument that answers an adjacent question fails toward CLEAN. The arm
// `TheSameFunctionOnAWideFieldNeedsNoIslandAtAll` builds the IDENTICAL
// function on a target differing in ONE character — `imm26` where the other
// says `imm14` — and requires it to emit no island at all. Without that arm a
// fixture that had stopped exercising the narrow field would still pass.

namespace {

// ── THE SYNTHETIC NARROW-REACH TARGET ────────────────────────────────────
//
// ⚠ WHAT THIS TARGET IS NOT. Its `jmp` row is SYNTHETIC: it wires a BlockRef
// straight onto the `imm14` slot of a one-word template. A real AArch64
// `TBZ` also carries a source register and a bit position, and this row wires
// neither — so the emitted word is NOT a TBZ and this file never claims it is.
// What is borrowed is the FIELD: bits 5..18, scaled by 4, the ARM ARM's
// numbers, which is the only property the machinery under test reads. The base
// word is the TBZ family's so that a reader disassembling the output is not
// left wondering what they are looking at.
//
// ⓘ `{FIELD}` is substituted so the two arms below differ in the slot NAME and
// in nothing else — which is what makes the wide-field arm a control on the
// narrow-field one rather than a second, differently-built fixture.
constexpr char const* kSpanTargetJsonTemplate = R"({
    "dssTargetVersion": 1,
    "target": { "name": "synth_span", "version": "0.1" },
    "registers": [
        { "name": "r0", "class": "gpr", "widthBytes": 8,
          "hwEncoding": 0 }
    ],
    "opcodes": [
        { "mnemonic": "invalid", "result": "none" },
        { "mnemonic": "sub", "result": "value",
          "minOperands": 2, "maxOperands": 2,
          "encoding": { "format": "fixed32", "registerClass": "gpr",
            "variants": [
              { "guard": { "operandKinds": ["reg", "reg"], "width": 64 },
                "template": { "fixedWord": 3405774848 },
                "resultSlot": "rd",
                "wires": [ { "index": 0, "slotKind": "rn" },
                           { "index": 1, "slotKind": "rm" } ] } ] } },
        { "mnemonic": "jmp", "result": "none", "terminatorKind": "br",
          "hasSideEffects": true,
          "minOperands": 1, "maxOperands": 1,
          "minSuccessors": 1, "maxSuccessors": 1,
          "encoding": { "format": "fixed32",
            "variants": [
              { "guard": { "operandKinds": ["blockref"] },
                "template": { "fixedWord": 905969664 },
                "wires": [ { "index": 0, "slotKind": "{FIELD}" } ] } ] } },
        { "mnemonic": "ret", "result": "none", "terminatorKind": "return",
          "hasSideEffects": true,
          "minOperands": 0, "maxOperands": 0,
          "minSuccessors": 0, "maxSuccessors": 0,
          "encoding": { "format": "fixed32",
            "variants": [
              { "guard": { "operandKinds": [] },
                "template": { "fixedWord": 3596551104 } } ] } }
    ]
})";

// The synthetic branch word's own signature: base 0x36000000 with the field
// bits cleared. Reading it back is how the chain walk below knows it is
// standing on a branch and not on a `sub`.
constexpr std::uint32_t kSpanBranchBase = 0x36000000u;

std::string spanTargetJson(char const* slotName) {
    std::string json = kSpanTargetJsonTemplate;
    auto const at = json.find("{FIELD}");
    EXPECT_NE(at, std::string::npos);
    if (at != std::string::npos)
        json.replace(at, std::string("{FIELD}").size(), slotName);
    return json;
}

struct SpanKit {
    std::shared_ptr<TargetSchema const> schema;
    std::uint16_t subOp{}, jmpOp{}, retOp{};
    LirReg r0{};
};

SpanKit loadSpanKit(char const* slotName) {
    SpanKit kit;
    auto const json = spanTargetJson(slotName);
    auto sOpt = TargetSchema::loadFromText(json, "<synth_span>");
    if (!sOpt.has_value()) {
        for (auto const& d : sOpt.error())
            ADD_FAILURE() << "synthetic target refused at '" << d.path
                          << "': " << d.message;
        return kit;
    }
    kit.schema  = *sOpt;
    auto const& s = *kit.schema;
    auto opc = [&](char const* m) {
        auto o = s.opcodeByMnemonic(m);
        EXPECT_TRUE(o.has_value()) << "synthetic target missing '" << m << "'";
        return o.value_or(0);
    };
    kit.subOp = opc("sub");
    kit.jmpOp = opc("jmp");
    kit.retOp = opc("ret");
    auto ord = s.registerByName("r0");
    EXPECT_TRUE(ord.has_value());
    kit.r0 = makePhysicalReg(static_cast<std::uint32_t>(ord.value_or(0)),
                             LirRegClass::GPR);
    return kit;
}

// ── THE SPANNING FUNCTION ────────────────────────────────────────────────
//
//   b0    +0 : jmp bEnd                1 word   <- THE LONG BRANCH
//   bPad  +4 : sub x kPad ; jmp bEnd    kPad+1 words
//   bEnd     : ret                     1 word   <- the target
//
// `b0`'s branch spans the whole padding block, so its displacement is
// 4*kPad + 8 bytes. `bPad`'s own terminator sits one word from its target and
// is never long — which matters, because a fixture where EVERY branch is long
// could not tell "the long one was rescued" from "all of them were".
struct SpanBuild {
    Lir        lir;
    LirBlockId bEnd{};
    std::uint64_t padCount = 0;
};

SpanBuild buildSpanningFunction(SpanKit const& kit, std::uint64_t kPad) {
    LirBuilder b{*kit.schema};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const b0   = b.createBlock();
    LirBlockId const bPad = b.createBlock();
    LirBlockId const bEnd = b.createBlock();

    b.beginBlock(b0);
    (void)b.addBr(kit.jmpOp, bEnd);

    b.beginBlock(bPad);
    {
        std::array<LirOperand, 2> subOps{LirOperand::makeReg(kit.r0),
                                         LirOperand::makeReg(kit.r0)};
        for (std::uint64_t i = 0; i < kPad; ++i)
            (void)b.addInst(kit.subOp, kit.r0, subOps);
        (void)b.addBr(kit.jmpOp, bEnd);
    }
    b.beginBlock(bEnd);
    (void)b.addReturn(kit.retOp, {});

    SpanBuild built;
    built.lir      = std::move(b).finish();
    built.bEnd     = bEnd;
    built.padCount = kPad;
    return built;
}

// The number of padding instructions that puts `b0`'s branch beyond ONE island
// hop, so the fixture measures a CHAIN rather than a single landing pad.
// Derived from the geometry table, never from a memory of ±32 KiB: one pad
// placed a stride along the way covers `stride + reach` bytes, so a span of
// `2 * reach` needs at least two.
std::uint64_t padCountForAChain() {
    auto const reach = walker_util::blockRelByteReach(
        walker_util::BlockRelPatchKind::Arm64Imm14);
    // span = 4*kPad + 8  >  2 * reach
    return static_cast<std::uint64_t>((2 * reach) / 4) + 4u;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// THE POSITIVE ARM — a branch past its WIDEST field's reach now assembles,
// and what carries it is a chain of register-free landing pads.
// ─────────────────────────────────────────────────────────────────────────
TEST(AsmBranchIsland, ABranchBeyondTheWidestFieldReachesThroughAChainOfIslands) {
    SpanKit const kit = loadSpanKit("imm14");
    ASSERT_NE(kit.schema, nullptr);

    SpanBuild built = buildSpanningFunction(kit, padCountForAChain());
    std::vector<MirInstId> lirToMir(built.lir.instCount(), InvalidMirInst);
    DiagnosticReporter rep;
    auto mod = assemble(built.lir, *kit.schema, lirToMir, rep);

    // ⚠ THE HEADLINE. Before islands this function could not be assembled at
    // all: the resolver refused with `A_ImmediateOperandOutOfRange`, said the
    // opcode declared no wider field, and prescribed a veneer through a
    // per-block symbol the assembler cannot mint. The branch never needed one.
    std::string diagnostics;
    for (auto const& d : rep.all()) diagnostics += "\n    " + d.actual;
    EXPECT_EQ(rep.errorCount(), 0u)
        << "a branch beyond the reach of the WIDEST block-relative field its "
           "opcode declares must now reach its target through branch islands "
           "(D-CSUBSET-LONG-BRANCH). Diagnostics:" << diagnostics;
    ASSERT_FALSE(mod.functions.empty());
    auto const& bytes = mod.functions[0].bytes;
    ASSERT_FALSE(bytes.empty()) << "the function was dropped";

    auto const g = walker_util::blockRelFieldGeometry(
        walker_util::BlockRelPatchKind::Arm64Imm14);
    auto const reach = walker_util::blockRelByteReach(
        walker_util::BlockRelPatchKind::Arm64Imm14);
    auto const fieldAt = [&](std::size_t off) {
        return signExtend((wordAt(bytes, off) >> g.lsb)
                              & ((1u << g.width) - 1u), g.width);
    };
    auto const& offs = mod.functions[0].blockByteOffsets;
    auto const endIt = offs.find(built.bEnd.v);
    ASSERT_NE(endIt, offs.end());
    auto const endOffset = static_cast<std::int64_t>(endIt->second);

    // ── WALK THE CHAIN, HOP BY HOP ───────────────────────────────────────
    // Start at `b0`'s branch (byte 0) and follow the field until it lands on
    // the target block. Every intermediate landing point is an island.
    std::vector<std::int64_t> chain;
    std::int64_t at = 0;
    for (int guard = 0; guard < 64; ++guard) {
        ASSERT_EQ(wordAt(bytes, static_cast<std::size_t>(at))
                      & ~(((1u << g.width) - 1u) << g.lsb),
                  kSpanBranchBase)
            << "the chain stepped onto something that is not this target's "
               "branch word, at byte " << at;
        std::int64_t const disp = fieldAt(static_cast<std::size_t>(at));
        std::int64_t const next = at + (disp << g.scaleLog2);
        EXPECT_LE(std::abs(disp << g.scaleLog2), reach)
            << "a hop exceeded the field's own reach — the resolver wrote a "
               "displacement the field cannot hold";
        if (next == endOffset) break;
        chain.push_back(next);
        at = next;
        ASSERT_LT(guard, 63) << "the chain did not terminate";
    }

    // ★★★ IT IS A CHAIN, NOT A SINGLE PAD. The span was built at twice the
    // field's reach precisely so one pad cannot cover it; a fixture that
    // settled for one would never have exercised the recursion-free chaining
    // the fixed point provides.
    ASSERT_GE(chain.size(), 2u)
        << "the branch reached its target in fewer than two hops — the span "
           "arithmetic moved, or a pad is reaching further than its field can";

    // ★★★ CONTROL CANNOT FALL INTO A PAD. A landing pad is reachable ONLY
    // through a branch aimed at it; the instruction laid out before it has no
    // idea it is there. Every cluster is therefore introduced by a JUMP-OVER
    // whose own displacement steps past it. Getting this wrong is not a crash:
    // the stream would fall into the pad and take a branch nobody asked for.
    for (auto const island : chain) {
        ASSERT_GE(island, 4);
        auto const over = island - 4;
        EXPECT_EQ(wordAt(bytes, static_cast<std::size_t>(over))
                      & ~(((1u << g.width) - 1u) << g.lsb),
                  kSpanBranchBase)
            << "the word before a landing pad is not a branch at all — "
               "control falls straight into the pad";
        std::int64_t const overTarget =
            over + (fieldAt(static_cast<std::size_t>(over)) << g.scaleLog2);
        EXPECT_EQ(overTarget, island + 4)
            << "the jump-over does not step exactly past the one pad in its "
               "cluster — control lands inside the cluster";
    }

    // ★★★ AND THE PAD READS NO REGISTER. Every veneer the row prescribed —
    // ADRP+ADD+BR, a literal-pool LDR+BR — needs a scratch GPR, and the
    // assembler runs after register allocation with no liveness to consult, so
    // such a veneer is a silent wrong answer rather than a refusal. A pad is
    // the opcode's own branch word with ONLY its displacement field changed,
    // which this compares byte-for-byte.
    for (auto const island : chain) {
        EXPECT_EQ(wordAt(bytes, static_cast<std::size_t>(island))
                      & ~(((1u << g.width) - 1u) << g.lsb),
                  kSpanBranchBase)
            << "a landing pad's word differs from the declared branch word "
               "outside its displacement field — it is not a bare branch";
    }

    // Exactly two words per cluster (the jump-over and its one pad), and not a
    // byte anywhere else: a placement that fired twice for one target, or on a
    // branch that fit, changes this total.
    std::uint64_t const expectedWords =
        1u                        // b0's jmp
      + (built.padCount + 1u)     // bPad's padding and its own jmp
      + 1u                        // bEnd's ret
      + 2u * chain.size();        // one jump-over + one pad per cluster
    EXPECT_EQ(bytes.size(), expectedWords * 4u)
        << "island placement added a different number of words than one "
           "cluster per hop";
}

// ─────────────────────────────────────────────────────────────────────────
// THE INSTRUMENT'S OWN POSITIVE CONTROL — remove the narrow field and the
// fixture must stop measuring anything.
// ─────────────────────────────────────────────────────────────────────────
//
// ★★★ WITHOUT THIS ARM THE ONE ABOVE PROVES NOTHING. A synthetic target is an
// instrument that answers an ADJACENT question, and every such instrument in
// this project's history has failed toward *clean*: had the fixture quietly
// stopped reaching the narrow field's edge, it would have gone on passing. So
// the same function, the same builder, the same assembler, and ONE character
// of difference in the target — `imm26` where the other says `imm14` — must
// produce no island at all. The arm is the fixture asserting what it is
// measuring, not merely that it is green.
TEST(AsmBranchIsland, TheSameFunctionOnAWideFieldNeedsNoIslandAtAll) {
    SpanKit const narrow = loadSpanKit("imm14");
    SpanKit const wide   = loadSpanKit("imm26");
    ASSERT_NE(narrow.schema, nullptr);
    ASSERT_NE(wide.schema, nullptr);

    std::uint64_t const kPad = padCountForAChain();
    SpanBuild built = buildSpanningFunction(wide, kPad);
    std::vector<MirInstId> lirToMir(built.lir.instCount(), InvalidMirInst);
    DiagnosticReporter rep;
    auto mod = assemble(built.lir, *wide.schema, lirToMir, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(mod.functions.empty());

    std::uint64_t const bareWords = 1u + (kPad + 1u) + 1u;
    EXPECT_EQ(mod.functions[0].bytes.size(), bareWords * 4u)
        << "a span that FITS the field must emit the function unchanged — an "
           "island here means placement fired on a branch that was in reach";

    // And the span really is one the narrow field cannot hold, which is the
    // fact the positive arm depends on. Stated against the geometry table so
    // the two arms cannot disagree about where the edge is.
    auto const span = static_cast<std::int64_t>(bareWords - 1u) * 4;
    EXPECT_GT(span, walker_util::blockRelByteReach(
                        walker_util::BlockRelPatchKind::Arm64Imm14))
        << "the fixture's span no longer exceeds the narrow field's reach — "
           "it has stopped testing islands entirely";
    EXPECT_LE(span, walker_util::blockRelByteReach(
                        walker_util::BlockRelPatchKind::Arm64Imm26))
        << "the fixture's span outgrew the WIDE field too, so this control arm "
           "would emit islands of its own and control nothing";
}

// ─────────────────────────────────────────────────────────────────────────
// THE COMPOSITION'S THIRD FACTOR — what the REAL targets declare as the body
// of an island, read rather than synthesized.
// ─────────────────────────────────────────────────────────────────────────
//
// ★★★ THIS IS THE ARM THAT KEEPS THE ROW HONEST. The machinery above is
// measured at ±32 KiB; the real fields' edges are a 134 MB and a 2 GiB
// function body and are NOT measured. What connects the two is the claim that
// each real branch opcode declares a self-contained, register-free
// unconditional branch for an island to be made of — and that claim is
// CHEAP to check, because it is a fact about config.
TEST(AsmBranchIsland, EveryShippedBranchOpcodeDeclaresARegisterFreeIslandBody) {
    auto const arm64 = TargetSchema::loadShipped("arm64");
    auto const x86   = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(arm64.has_value());
    ASSERT_TRUE(x86.has_value());

    auto opcodeOf = [](TargetSchema const& s, char const* m)
        -> TargetOpcodeInfo const* {
        auto const o = s.opcodeByMnemonic(m);
        EXPECT_TRUE(o.has_value()) << "missing opcode '" << m << "'";
        return o.has_value() ? s.opcodeInfo(*o) : nullptr;
    };

    // ── AArch64: `B`, 0x14000000, four bytes, no register field ──────────
    for (char const* m : {"jmp", "jcc"}) {
        auto const* info = opcodeOf(**arm64, m);
        ASSERT_NE(info, nullptr);
        auto const body = fixed32::islandBody(*info);
        EXPECT_TRUE(body.declared())
            << "arm64 '" << m << "' declares no self-contained unconditional "
               "branch word, so its widest field has no island to escape into";
        EXPECT_EQ(body.byteCount, 4u);
        EXPECT_EQ(body.fieldOffset, 0u);
        EXPECT_EQ(body.kind, walker_util::BlockRelPatchKind::Arm64Imm26);
        std::uint32_t const word =
            static_cast<std::uint32_t>(body.bytes[0])
          | (static_cast<std::uint32_t>(body.bytes[1]) << 8)
          | (static_cast<std::uint32_t>(body.bytes[2]) << 16)
          | (static_cast<std::uint32_t>(body.bytes[3]) << 24);
        // ARM ARM: `B <label>` is 0x14000000 with imm26 at bits 0..25. A body
        // carrying ANY other bit would be a different instruction — and on
        // AArch64 the ones that would fit here all name a register.
        EXPECT_EQ(word, kBOpcode)
            << "arm64 '" << m << "' would build its island out of a word that "
               "is not a bare B — a register field zeroed by the copy is the "
               "silent-miscompile shape this election exists to refuse";
    }

    // ── x86-64: `E9` + rel32, five bytes, no ModR/M and no register ──────
    for (char const* m : {"jmp", "jcc"}) {
        auto const* info = opcodeOf(**x86, m);
        ASSERT_NE(info, nullptr);
        auto const body = x86_variable::islandBody(*info);
        EXPECT_TRUE(body.declared())
            << "x86_64 '" << m << "' declares no self-contained unconditional "
               "branch, so rel32 — its WIDEST block-relative field — would "
               "have no island to escape into";
        EXPECT_EQ(body.byteCount, 5u);
        EXPECT_EQ(body.fieldOffset, 1u);
        EXPECT_EQ(body.kind, walker_util::BlockRelPatchKind::X86Rel32);
        EXPECT_EQ(body.bytes[0], 0xE9u)
            << "the near `jmp rel32` opcode byte is what an x86 island is made "
               "of (Intel SDM E9 cd); anything else is a different instruction";
        for (std::size_t i = 1; i < 5; ++i)
            EXPECT_EQ(body.bytes[i], 0u)
                << "the displacement field must be quoted ZERO — the resolver "
                   "writes it, and a non-zero quote would OR into the answer";
    }
}
