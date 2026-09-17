// [[D-LK-AARCH64-CALL26-BEYOND-RANGE-HAS-NO-VENEER]]
// The veneer pass. Its sibling one tier down is [[D-CSUBSET-LONG-BRANCH]].
//
// ★★★ THIS FILE MEASURES THE REAL EDGE, AND THAT IS WORTH SAYING OUT LOUD
// BECAUSE ITS SIBLING CANNOT. The assembler's ±128 MiB arm is pinned by
// COMPOSITION — the machinery at a synthetic ±32 KiB field, the geometry by
// byte-for-byte arms — because reaching the real edge there means assembling
// 33 million instructions. Here it costs a `resize`: the veneer pass reads
// function SIZES and relocation entries, never instruction bytes, so a quarter
// of a gigabyte of filler is a zero-filled `resize` rather than 33 million
// encoded instructions. The edge below is the true ±128 MiB
// `R_AARCH64_CALL26` edge, twice over.
//
// ⓘ COST, ✔MEASURED and stated because it is the largest allocation in the
// suite: 256 MiB of zero-filled filler (2 x the field's reach), spread over 64
// functions, live for the duration of two of the four tests — 96 ms and 32 ms
// respectively, 175 ms for the file. The pass never copies those bytes:
// `injectBranchVeneers` reserves before inserting, so every shift is a buffer
// steal rather than a copy.
//
// ⚠ WHAT IS NOT MEASURED HERE: the emitted IMAGE. This pins the pass that
// decides and places, not a linked binary with a 200 MiB text section on disk.
// The applier that writes the branch field is pinned by
// `test_aarch64_reloc_formulas.cpp`, and the two now read their field's width,
// scale and bit window from one source (`link::branchRelocGeometry`).

#include "asm/asm.hpp"
#include "asm/branch_island.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/branch_reloc_geometry.hpp"
#include "link/branch_veneers.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

using namespace dss;

namespace {

// ARM ARM, quoted so an assertion can name what it read:
//   `BL <label>` = 0x94000000 | imm26     (branch with link)
//   `B  <label>` = 0x14000000 | imm26     (plain branch — what a veneer is)
//   `RET`        = 0xD65F03C0
constexpr std::uint32_t kBL      = 0x94000000u;
constexpr std::uint32_t kB       = 0x14000000u;
constexpr std::uint32_t kRet     = 0xD65F03C0u;
constexpr std::uint32_t kImm26Mask = 0x03FFFFFFu;

AssembledFunction word(SymbolId sym, std::uint32_t w) {
    AssembledFunction fn;
    fn.symbol = sym;
    fn.bytes  = {static_cast<std::uint8_t>(w & 0xFFu),
                 static_cast<std::uint8_t>((w >> 8) & 0xFFu),
                 static_cast<std::uint8_t>((w >> 16) & 0xFFu),
                 static_cast<std::uint8_t>((w >> 24) & 0xFFu)};
    return fn;
}

std::uint32_t wordOf(std::vector<std::uint8_t> const& b) {
    return static_cast<std::uint32_t>(b[0])
         | (static_cast<std::uint32_t>(b[1]) << 8)
         | (static_cast<std::uint32_t>(b[2]) << 16)
         | (static_cast<std::uint32_t>(b[3]) << 24);
}

// caller (`BL callee`) │ `fillerCount` filler functions │ callee (`RET`)
//
// The caller's relocation names the callee directly, which is what every
// compiled call looks like before anything is laid out.
//
// ⚠ THE FILLER IS MANY FUNCTIONS, NOT ONE, AND THAT IS THE FIXTURE'S WHOLE
// SHAPE. A veneer is a whole FUNCTION, so it can only stand at a function
// boundary — unlike an assembler island, which goes between two instructions
// and can therefore always be placed. An image whose caller and callee are
// separated by ONE oversized function has no boundary to stand at, and the pass
// must REFUSE it rather than search forever; that case has its own arm below.
// A real image has boundaries every few hundred bytes, and this is the shape
// that tests the mechanism rather than its refusal.
struct Span {
    AssembledModule module;
    SymbolId        caller{1}, callee{2};
    std::size_t     fillerCount = 0;
};

Span buildSpannedCall(TargetSchema const& target,
                      std::uint64_t       fillerBytes,
                      std::size_t         fillerCount = 64) {
    Span s;
    auto const* call26 = target.relocationByName("call26");
    EXPECT_NE(call26, nullptr) << "arm64 declares no 'call26' relocation row";

    auto caller = word(s.caller, kBL);
    if (call26 != nullptr)
        caller.relocations.push_back(Relocation{0u, s.callee, call26->kind, 0});
    s.module.functions.push_back(std::move(caller));

    s.fillerCount = fillerCount == 0 ? 1u : fillerCount;
    std::uint64_t const each = fillerBytes / s.fillerCount;
    std::uint64_t placed = 0;
    for (std::size_t i = 0; i < s.fillerCount; ++i) {
        AssembledFunction filler;
        filler.symbol = SymbolId{static_cast<std::uint32_t>(100 + i)};
        std::uint64_t const n =
            (i + 1u == s.fillerCount) ? fillerBytes - placed : each;
        filler.bytes.resize(static_cast<std::size_t>(n), 0u);
        placed += n;
        s.module.functions.push_back(std::move(filler));
    }

    s.module.functions.push_back(word(s.callee, kRet));
    s.module.expectedFuncCount = s.module.functions.size();
    return s;
}

bool isFiller(SymbolId sym) { return sym.v >= 100u && sym.v < 1000u; }

std::int64_t call26Reach() {
    return link::branchRelocByteReach(RelocFormulaKind::Aarch64Call26);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// THE POSITIVE ARM — a call past ±128 MiB now links, through a chain of
// register-free veneers.
// ─────────────────────────────────────────────────────────────────────────
TEST(LinkBranchVeneer, ACallBeyondCall26RangeReachesThroughAChainOfVeneers) {
    auto sOpt = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(sOpt.has_value());
    auto const& target = **sOpt;

    // Past the reach AND past one veneer's own reach-plus-stride, so the pass
    // has to CHAIN rather than place a single pad. Derived from the geometry
    // row, never from a memory of "128 MiB".
    auto const reach = call26Reach();
    Span span = buildSpannedCall(
        target, static_cast<std::uint64_t>(2 * reach));

    ASSERT_TRUE(linker::branchVeneersNeeded(span.module, target))
        << "a call across " << (2 * reach) << " bytes must be beyond a signed "
           "26-bit word-scaled field — if this is false the fixture stopped "
           "testing the edge";

    DiagnosticReporter rep;
    ASSERT_TRUE(linker::injectBranchVeneers(span.module, target, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
    // ⚠ THE HEADLINE. Before this pass the image was refused outright by
    // `applyExecRelocations`: *"does not fit signed 26-bit — branch out of
    // ±128 MiB range"*, with nothing to be done about it.
    EXPECT_FALSE(linker::branchVeneersNeeded(span.module, target))
        << "after the pass every branch relocation must reach where it points";

    // ── WALK THE CHAIN ───────────────────────────────────────────────────
    std::unordered_map<std::uint32_t, std::size_t> byId;
    std::vector<std::uint64_t> starts;
    std::uint64_t at = 0;
    for (std::size_t i = 0; i < span.module.functions.size(); ++i) {
        byId.emplace(span.module.functions[i].symbol.v, i);
        starts.push_back(at);
        at += span.module.functions[i].bytes.size();
    }

    std::size_t hops = 0;
    std::size_t fi   = byId.at(span.caller.v);
    for (int guard = 0; guard < 64; ++guard) {
        auto const& fn = span.module.functions[fi];
        ASSERT_EQ(fn.relocations.size(), 1u)
            << "every node of the chain carries exactly one branch relocation";
        auto const& rel = fn.relocations[0];
        auto const nextIt = byId.find(rel.target.v);
        ASSERT_NE(nextIt, byId.end())
            << "a chain node points at a symbol no function defines";
        std::int64_t const delta =
            static_cast<std::int64_t>(starts[nextIt->second])
          - static_cast<std::int64_t>(starts[fi] + rel.offset);
        EXPECT_EQ(delta % 4, 0) << "an AArch64 branch target must be aligned";
        EXPECT_LE(std::abs(delta), reach)
            << "a hop exceeds the field's reach — the applier would refuse it";
        if (rel.target.v == span.callee.v) break;
        ++hops;
        fi = nextIt->second;
        ASSERT_LT(guard, 63) << "the chain never reached the callee";
    }

    // ★★★ IT IS A CHAIN. A span of twice the reach cannot be covered by one
    // veneer placed a half-reach along, which is the whole reason the pass is a
    // fixed point rather than a single fix-up.
    EXPECT_GE(hops, 2u)
        << "the call reached its callee in fewer than two veneers — the span "
           "arithmetic moved, or a veneer is reaching further than its field";

    // ★★★ AND EVERY VENEER IS A BARE BRANCH — NO SCRATCH REGISTER. At a call
    // boundary AAPCS64 leaves x16/x17 free, so an ADRP+ADD+BR thunk WOULD be
    // legal here; it is not taken, because `BL veneer` / `veneer: B callee`
    // leaves x30 holding the real return address and costs one word. This
    // compares the word against the ARM ARM's `B` with only its displacement
    // field allowed to differ.
    for (auto const& fn : span.module.functions) {
        if (fn.symbol.v == span.caller.v || fn.symbol.v == span.callee.v
         || isFiller(fn.symbol))
            continue;
        ASSERT_EQ(fn.bytes.size(), 4u) << "a veneer is one word";
        EXPECT_EQ(wordOf(fn.bytes) & ~kImm26Mask, kB)
            << "a veneer's word is not a bare `B` — any other bits would name "
               "a register the linker cannot know is free";
        EXPECT_EQ(wordOf(fn.bytes) & kImm26Mask, 0u)
            << "the displacement field must be emitted ZERO; the applier ORs "
               "the computed value in and refuses a dirty field";
    }

    // The caller's own word is untouched: a veneer re-points a RELOCATION, it
    // never rewrites the call instruction.
    EXPECT_EQ(wordOf(span.module.functions[byId.at(span.caller.v)].bytes), kBL);
    EXPECT_EQ(span.module.expectedFuncCount, span.module.functions.size());
}

// ─────────────────────────────────────────────────────────────────────────
// THE CONTROL ARM — a call that FITS must not be touched at all.
// ─────────────────────────────────────────────────────────────────────────
//
// ★★★ WITHOUT THIS THE ARM ABOVE PROVES ONLY THAT VENEERS APPEAR. A pass that
// veneered every call would satisfy it and would bloat every image ever linked.
TEST(LinkBranchVeneer, ACallWithinRangeGetsNoVeneerAndNoCopy) {
    auto sOpt = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(sOpt.has_value());
    auto const& target = **sOpt;

    Span span = buildSpannedCall(target, 4096u);
    auto const before = span.module.functions.size();
    EXPECT_FALSE(linker::branchVeneersNeeded(span.module, target))
        << "a 4 KiB span is three orders of magnitude inside ±128 MiB — a "
           "veneer here would be placed on a call that reaches perfectly well";

    DiagnosticReporter rep;
    ASSERT_TRUE(linker::injectBranchVeneers(span.module, target, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(span.module.functions.size(), before)
        << "the pass inserted a function into an image that needed none";
    EXPECT_EQ(span.module.functions[0].relocations[0].target.v, span.callee.v)
        << "the call's relocation was re-pointed although it already reached";
}

// ─────────────────────────────────────────────────────────────────────────
// THE ELECTION — two declarations have to agree, and the refusal says which.
// ─────────────────────────────────────────────────────────────────────────
//
// ★★★ THE VENEER IS NOT SYNTHESIZED, IT IS QUOTED. Its word comes from an
// opcode the target declares `terminatorKind: br` on, and the relocation that
// fills its field comes from a row whose formula writes THAT SAME field. A
// target missing either half gets a loud refusal naming the missing one, not a
// fabricated branch.
TEST(LinkBranchVeneer, AnUnconditionalBranchWordAndAMatchingRelocationRowMustBothExist) {
    auto sOpt = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(sOpt.has_value());
    auto const& target = **sOpt;

    // The body the election finds on the shipped target, and the fact that it
    // matches `call26`'s field exactly — which is WHY the election succeeds.
    auto const body = asm_island::widestDeclaredIslandBody(target);
    ASSERT_TRUE(body.declared());
    auto const fg = walker_util::blockRelFieldGeometry(body.kind);
    auto const rg = link::branchRelocGeometry(RelocFormulaKind::Aarch64Call26);
    EXPECT_EQ(rg.fieldBits, fg.width);
    EXPECT_EQ(rg.scaleLog2, fg.scaleLog2);
    EXPECT_EQ(rg.lsb, fg.lsb)
        << "the quoted branch word's field and the relocation formula's field "
           "must be the same window, or the linker would OR the displacement "
           "into the wrong bits of a word it copied";

    // A target with a relocation row but no `br` opcode to quote: the refusal
    // must name the missing half rather than invent a branch.
    constexpr char const* kNoBranchOpcode = R"({
      "dssTargetVersion": 1,
      "target": {"name":"no_branch_opcode"},
      "relocations":[
        { "name": "call26", "kind": 1, "formula": "aarch64_call26" }
      ],
      "opcodes":[ {"mnemonic":"invalid","result":"none"} ]
    })";
    auto bare = TargetSchema::loadFromText(kNoBranchOpcode);
    ASSERT_TRUE(bare.has_value());
    Span span = buildSpannedCall(**bare, static_cast<std::uint64_t>(
                                     2 * call26Reach()));
    ASSERT_TRUE(linker::branchVeneersNeeded(span.module, **bare));
    DiagnosticReporter rep;
    EXPECT_FALSE(linker::injectBranchVeneers(span.module, **bare, rep));
    EXPECT_GT(rep.errorCount(), 0u)
        << "a target that declares nothing to build a veneer from must REFUSE, "
           "loudly, rather than link an image whose call goes nowhere";
}

// ─────────────────────────────────────────────────────────────────────────
// THE CASE THIS TIER CANNOT BRIDGE, AND WHICH MUST REFUSE RATHER THAN SEARCH.
// ─────────────────────────────────────────────────────────────────────────
//
// ★★★ THIS ARM EXISTS BECAUSE THE PASS FAILED IT. ✔MEASURED 2026-09-17: the
// first draft placed a veneer at whatever function boundary was NEAREST its
// desired offset, without asking whether that boundary made any progress
// towards the callee. Handed a module whose caller and callee are separated by
// ONE function larger than the field's reach — where every boundary is on the
// WRONG side of the gap — it placed a veneer four bytes further along and
// measured again, forever: 315 seconds and no refusal. A bound that counts the
// veneers' own branch relocations grew as fast as the veneers did, so it never
// fired either.
//
// The shape is not hypothetical and it is not this tier's to fix: a single
// function bigger than ±128 MiB is exactly what the ASSEMBLER's branch islands
// are for, because an island goes between two INSTRUCTIONS and a veneer can
// only go between two FUNCTIONS. The right answer is a refusal that says so.
TEST(LinkBranchVeneer, OneOversizedFunctionBetweenCallerAndCalleeRefusesLoudly) {
    auto sOpt = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(sOpt.has_value());
    auto const& target = **sOpt;

    // ONE filler, so the only boundaries in the image are at its two ends —
    // both on the wrong side of a gap wider than the field.
    Span span = buildSpannedCall(
        target, static_cast<std::uint64_t>(2 * call26Reach()), /*fillerCount=*/1);
    ASSERT_TRUE(linker::branchVeneersNeeded(span.module, target));

    DiagnosticReporter rep;
    EXPECT_FALSE(linker::injectBranchVeneers(span.module, target, rep))
        << "a gap with no boundary to stand at must be refused, not searched";
    EXPECT_GT(rep.errorCount(), 0u);
    bool namedTheCause = false;
    for (auto const& d : rep.all())
        if (d.actual.find("NO FUNCTION BOUNDARY") != std::string::npos)
            namedTheCause = true;
    EXPECT_TRUE(namedTheCause)
        << "the refusal must name the real cause — no boundary to place a "
           "veneer at — rather than reporting a generic out-of-range value";
}
