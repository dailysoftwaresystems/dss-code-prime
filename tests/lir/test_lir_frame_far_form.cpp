// D-LK10-ENTRY-ARM64-WIDE-IMMEDIATE (the frame-offset arm) — a frame access
// BEYOND every form the target declares for it.
//
// AArch64 reaches a frame slot two ways: the unscaled LDUR/STUR imm9 (±256) and
// the scaled LDR/STR imm12 (4095 × the access size). Until this file's subject
// landed, an offset past BOTH kept the unscaled op at the frame chokepoint
// (`lir_callconv.cpp` `selectFrameMemOp`) and the encoder refused it — a correct
// C program refused at the DEFAULT pipeline. ✔MEASURED 2026-09-18, both through
// the shipped CLI: a function reading its 9th integer param above a 40000-byte
// local (`opcode 'load': memory offset 40000 … 'imm9'`), and a 2174-spill
// function (`opcode 'store': memory offset 32768 …`), each of which
// aarch64-linux-gnu-gcc 13.3.0 compiles and RUNS under qemu-aarch64.
//
// The access is now FAR: `lea A, [base + offset]`, then the access at `[A + 0]`.
// A load into the base register's class addresses through its own destination;
// a store, or a load into the SIMD&FP file, goes through the calling
// convention's declared `frameAddressScratch` — x30 on arm64 — which the
// function saves (a leaf has it folded in only if its body needs it).
//
// ★ THE PINS ARE STRUCTURAL AND BYTE-EXACT, AND THE BYTES ARE A REFERENCE'S.
// Every word asserted below was assembled by aarch64-linux-gnu-as 2.42 AND by
// clang 18.1.3's integrated assembler, SEPARATELY, and the two agree:
//   add x30, sp, #0x340          = 0x910D03FE
//   add x30, x30, #0x8, lsl #12  = 0x914023DE
//   stur x9, [x30]               = 0xF80003C9
//   add x9, sp, #0x340           = 0x910D03E9
//   add x9, x9, #0x8, lsl #12    = 0x91402129
//   ldur x9, [x9]                = 0xF8400129
//   add x30, sp, #0x680          = 0x911A03FE
//   add x30, x30, #0x10, lsl #12 = 0x914043DE
//   ldur q16, [x30]              = 0x3CC003D0
// (0x8340 = 33600 is a spill slot past the 8-byte scaled reach of 32760;
// 0x10680 = 67200 is one past the 16-byte Q reach of 65520 — 33600 is NOT far
// for a Q-width access, which scales by 16, and the first draft of the FP pin
// asserted it was: it ran green on the scaled form and the pin was wrong.)
// The RUN witness is `examples/c/frame_far_reach_arm64`, where a wrong
// displacement changes the exit code.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_2addr_legalize.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"
#include "lowered_lir_fixture.hpp"
#include "mutate_target_schema.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

constexpr std::uint32_t kSymbol = 77;

// The words a reference assembler emits for the far sequences pinned below.
constexpr std::uint32_t kAddX30SpLo     = 0x910D03FEu;  // add x30, sp, #0x340
constexpr std::uint32_t kAddX30X30Hi    = 0x914023DEu;  // add x30, x30, #8, lsl #12
constexpr std::uint32_t kSturX9AtX30    = 0xF80003C9u;  // stur x9, [x30]
constexpr std::uint32_t kAddX9SpLo      = 0x910D03E9u;  // add x9, sp, #0x340
constexpr std::uint32_t kAddX9X9Hi      = 0x91402129u;  // add x9, x9, #8, lsl #12
constexpr std::uint32_t kLdurX9AtX9     = 0xF8400129u;  // ldur x9, [x9]
constexpr std::uint32_t kAddX30SpLoQ    = 0x911A03FEu;  // add x30, sp, #0x680
constexpr std::uint32_t kAddX30X30HiQ   = 0x914043DEu;  // add x30, x30, #0x10, lsl #12
constexpr std::uint32_t kLdurQ16AtX30   = 0x3CC003D0u;  // ldur q16, [x30]

[[nodiscard]] std::shared_ptr<TargetSchema> shipped(std::string_view name) {
    auto s = TargetSchema::loadShipped(name);
    if (!s.has_value()) {
        for (auto const& d : s.error()) ADD_FAILURE() << d.path << ": " << d.message;
        return nullptr;
    }
    return *s;
}

[[nodiscard]] LirReg phys(TargetSchema const& s, std::string_view name,
                          LirRegClass cls) {
    auto const ord = s.registerByName(name);
    EXPECT_TRUE(ord.has_value()) << "register " << name;
    return makePhysicalReg(ord.value_or(0), cls);
}

// A post-rewrite function: `body` fills its one block, then `ret`.
template <typename Body>
[[nodiscard]] Lir oneFunction(TargetSchema const& s, Body&& body) {
    LirBuilder b{s};
    b.addFunction(SymbolId{kSymbol});
    LirBlockId const blk = b.createBlock();
    b.beginBlock(blk);
    body(b);
    auto const retOp = s.opcodeByMnemonic("ret");
    EXPECT_TRUE(retOp.has_value());
    b.addInst(*retOp, InvalidLirReg, std::span<LirOperand const>{});
    return std::move(b).finish();
}

// The allocation `materializeCallingConvention` reads for that function: cc 0
// (aapcs64 on arm64, sysv_amd64 on x86_64) and `spillSlots` spill slots.
[[nodiscard]] LirAllocation allocationWith(std::uint32_t spillSlots) {
    LirAllocation alloc;
    alloc.perFunc.emplace_back();
    alloc.perFunc.back().ok                     = true;
    alloc.perFunc.back().originalSymbol         = SymbolId{kSymbol};
    alloc.perFunc.back().callingConventionIndex = 0;
    alloc.perFunc.back().numSpillSlots          = spillSlots;
    return alloc;
}

void addFrameStore(LirBuilder& b, TargetSchema const& s, LirReg value,
                   std::uint32_t slot) {
    auto const op = s.opcodeByMnemonic("frame_store");
    ASSERT_TRUE(op.has_value());
    std::array<LirOperand, 1> ops{LirOperand::makeReg(value)};
    b.addInst(*op, InvalidLirReg, ops, /*payload=*/slot);
}

void addFrameLoad(LirBuilder& b, TargetSchema const& s, LirReg result,
                  std::uint32_t slot) {
    auto const op = s.opcodeByMnemonic("frame_load");
    ASSERT_TRUE(op.has_value());
    b.addInst(*op, result, std::span<LirOperand const>{}, /*payload=*/slot);
}

[[nodiscard]] std::vector<std::uint32_t> wordsOf(std::vector<std::uint8_t> const& bytes) {
    std::vector<std::uint32_t> ws;
    for (std::size_t i = 0; i + 3 < bytes.size(); i += 4) {
        ws.push_back(static_cast<std::uint32_t>(bytes[i])
                     | (static_cast<std::uint32_t>(bytes[i + 1]) << 8)
                     | (static_cast<std::uint32_t>(bytes[i + 2]) << 16)
                     | (static_cast<std::uint32_t>(bytes[i + 3]) << 24));
    }
    return ws;
}

// Does `ws` contain `seq` as a contiguous run?
[[nodiscard]] bool containsRun(std::vector<std::uint32_t> const& ws,
                               std::vector<std::uint32_t> const& seq) {
    if (seq.empty() || ws.size() < seq.size()) return false;
    for (std::size_t i = 0; i + seq.size() <= ws.size(); ++i) {
        bool all = true;
        for (std::size_t k = 0; k < seq.size() && all; ++k) all = ws[i + k] == seq[k];
        if (all) return true;
    }
    return false;
}

[[nodiscard]] bool savedContains(FrameLayout const& layout, LirReg r) {
    for (LirReg const& s : layout.savedRegs)
        if (s.isPhysical != 0 && s.id == r.id) return true;
    return false;
}

[[nodiscard]] std::string allMessages(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) out += "\n  " + d.actual;
    return out.empty() ? std::string{"<no diagnostics>"} : out;
}

struct Materialized {
    LirCallconvResult         cc;
    DiagnosticReporter        ccRep;
    std::vector<std::uint8_t> bytes;
    DiagnosticReporter        asmRep;
};

// materialize → assemble, keeping both verdicts for the caller to read.
[[nodiscard]] std::unique_ptr<Materialized>
materializeAndAssemble(Lir const& lir, TargetSchema const& s,
                       LirAllocation const& alloc) {
    auto m = std::make_unique<Materialized>();
    m->cc = materializeCallingConvention(lir, s, alloc, m->ccRep);
    if (!m->cc.ok()) return m;
    std::vector<MirInstId> lirToMir(m->cc.lir.instCount(), InvalidMirInst);
    auto assembled = assemble(m->cc.lir, s, lirToMir, m->asmRep);
    if (!assembled.functions.empty()) m->bytes = assembled.functions[0].bytes;
    return m;
}

} // namespace

// ── THE REACH OF EVERY FORM IS READ FROM THE ENCODING ───────────────────────
//
// `frameMemAccessForm` is the chokepoint's own decision. These are the boundary
// offsets on both sides of each reach, for both register files and at the
// narrowest and widest access. ⚠ Every one of these numbers used to be a
// LITERAL in `selectFrameMemOp` (`-256..255`, `/ access <= 4095`); a pin here
// that moved would mean the config and the chokepoint had come apart.
TEST(FrameFarForm, EveryArm64ReachBoundaryIsTheEncodingsOwn) {
    auto s = shipped("arm64");
    ASSERT_NE(s, nullptr);
    auto const op = [&](std::string_view m) {
        auto const o = s->opcodeByMnemonic(m);
        EXPECT_TRUE(o.has_value()) << m;
        return o.value_or(0);
    };
    auto const w = [](std::uint32_t bits) { return *lirInstWidthFlagForBits(bits); };
    struct Case {
        char const*   base;
        bool          isStore;
        std::uint32_t widthBits;
        std::int32_t  offset;
        char const*   expectOp;
        bool          expectFar;
    };
    std::array const cases{
        // the integer file, 8-byte access
        Case{"store", true, 64,    255, "store",   false},  // imm9 max
        Case{"store", true, 64,    256, "store_u", false},  // scaled, 256/8
        Case{"store", true, 64,  32760, "store_u", false},  // scaled max 4095*8
        Case{"store", true, 64,  32768, "store",   true },  // past both
        Case{"store", true, 64,   -256, "store",   false},  // imm9 min
        Case{"store", true, 64,   -257, "store",   true },  // negative past imm9
        Case{"store", true, 64,    260, "store",   true },  // unaligned past imm9
        Case{"load",  false, 64, 40000, "load",    true },  // T1's 9th param
        // the integer file, 1-byte access: the scaled reach is 4095
        Case{"load",  false, 8,   4095, "load_u",  false},
        Case{"load",  false, 8,   4096, "load",    true },
        // the SIMD&FP file: 16-byte Q reach 65520, 8-byte D reach 32760
        Case{"fstur", true, 128, 65520, "fstr_u",  false},
        Case{"fstur", true, 128, 65536, "fstur",   true },
        Case{"fldur", false, 64, 32760, "fldr_u",  false},
        Case{"fldur", false, 64, 32768, "fldur",   true },
    };
    for (auto const& c : cases) {
        SCOPED_TRACE(std::string{c.base} + " @" + std::to_string(c.offset)
                     + " w" + std::to_string(c.widthBits));
        auto const f = frameMemAccessForm(*s, op(c.base), c.isStore, c.offset,
                                          w(c.widthBits));
        EXPECT_EQ(f.op, op(c.expectOp));
        EXPECT_EQ(f.far, c.expectFar);
    }
}

// AGNOSTIC CORROBORATION: x86_64's frame forms carry a disp32, so no int32
// frame offset is ever far there and no scaled twin is ever chosen.
TEST(FrameFarForm, X8664FrameAccessesAreNeverFar) {
    auto s = shipped("x86_64");
    ASSERT_NE(s, nullptr);
    auto const store = s->opcodeByMnemonic("store");
    auto const load  = s->opcodeByMnemonic("load");
    ASSERT_TRUE(store.has_value() && load.has_value());
    for (std::int32_t const off : {0, 255, 256, 32768, 40000, 0x7FFFFFFF, -4096}) {
        SCOPED_TRACE(off);
        auto const st = frameMemAccessForm(*s, *store, true, off, 0);
        EXPECT_EQ(st.op, *store);
        EXPECT_FALSE(st.far);
        auto const ld = frameMemAccessForm(*s, *load, false, off, 0);
        EXPECT_EQ(ld.op, *load);
        EXPECT_FALSE(ld.far);
    }
    EXPECT_FALSE(s->callingConvention(0)->frameAddressScratch.has_value())
        << "x86_64 needs no frame-address scratch — declaring one would be a "
           "register taken for a form that can never be reached";
}

// ── THE SPILL STORE `ee` MEASURED REFUSED ───────────────────────────────────
//
// A spill slot past the scaled reach, stored from x9 in a LEAF: the store goes
// through x30, and x30 is SAVED by this function because it has no call to
// have saved it already.
TEST(FrameFarForm, Arm64FarSpillStoreGoesThroughTheLinkRegisterWhichTheLeafSaves) {
    auto s = shipped("arm64");
    ASSERT_NE(s, nullptr);
    LirReg const x9  = phys(*s, "x9", LirRegClass::GPR);
    LirReg const x30 = phys(*s, "x30", LirRegClass::GPR);
    Lir const lir = oneFunction(*s, [&](LirBuilder& b) {
        addFrameStore(b, *s, x9, /*slot=*/2100);
    });
    auto const m = materializeAndAssemble(lir, *s, allocationWith(2200));
    ASSERT_TRUE(m->cc.ok()) << allMessages(m->ccRep);
    EXPECT_EQ(m->asmRep.errorCount(), 0u) << allMessages(m->asmRep);
    auto const* layout = m->cc.forFuncByIndex(0);
    ASSERT_NE(layout, nullptr);
    EXPECT_TRUE(savedContains(*layout, x30))
        << "a leaf that needs the frame-address scratch must save it — x30 is "
           "its return address";
    ASSERT_EQ(layout->spillAreaOffset() + 2099u * layout->slotSize, 33600u)
        << "the pinned words below assume the slot sits at sp+33600 (16 bytes "
           "of saved x30, then 2099 slots of 16)";
    auto const ws = wordsOf(m->bytes);
    EXPECT_TRUE(containsRun(ws, {kAddX30SpLo, kAddX30X30Hi, kSturX9AtX30}))
        << "the store must be `add x30, sp, #0x340; add x30, x30, #8, lsl #12; "
           "stur x9, [x30]` — byte-for-byte what gas and clang assemble";
}

// A far integer RELOAD needs no scratch — it addresses through its own
// destination — so a leaf whose ONLY far access is one keeps its frame: x30 is
// NOT folded in. This is the predicate's exactness, pinned from the side where
// a sloppy predicate would still pass every run-witness.
TEST(FrameFarForm, Arm64FarGprReloadAddressesThroughItsOwnDestinationAndFoldsNothing) {
    auto s = shipped("arm64");
    ASSERT_NE(s, nullptr);
    LirReg const x9  = phys(*s, "x9", LirRegClass::GPR);
    LirReg const x30 = phys(*s, "x30", LirRegClass::GPR);
    Lir const lir = oneFunction(*s, [&](LirBuilder& b) {
        addFrameLoad(b, *s, x9, /*slot=*/2101);
    });
    auto const m = materializeAndAssemble(lir, *s, allocationWith(2200));
    ASSERT_TRUE(m->cc.ok()) << allMessages(m->ccRep);
    EXPECT_EQ(m->asmRep.errorCount(), 0u) << allMessages(m->asmRep);
    auto const* layout = m->cc.forFuncByIndex(0);
    ASSERT_NE(layout, nullptr);
    EXPECT_FALSE(savedContains(*layout, x30))
        << "nothing in this leaf needs the frame-address scratch";
    ASSERT_EQ(layout->spillAreaOffset() + 2100u * layout->slotSize, 33600u);
    EXPECT_TRUE(containsRun(wordsOf(m->bytes),
                            {kAddX9SpLo, kAddX9X9Hi, kLdurX9AtX9}))
        << "the reload must be `add x9, sp, #0x340; add x9, x9, #8, lsl #12; "
           "ldur x9, [x9]`";
}

// A far SIMD&FP reload has no integer destination to borrow: it goes through
// x30, and the Q-form load is width-exact. ⚠ The slot is past the Q reach
// (65520), not merely past the integer one: a 16-byte access scales by 16, so a
// slot at 33600 is carried by the scaled `fldr_u` and is not far at all.
TEST(FrameFarForm, Arm64FarFpReloadGoesThroughTheScratchAtFullWidth) {
    auto s = shipped("arm64");
    ASSERT_NE(s, nullptr);
    LirReg const v16 = phys(*s, "v16", LirRegClass::FPR);
    LirReg const x30 = phys(*s, "x30", LirRegClass::GPR);
    Lir const lir = oneFunction(*s, [&](LirBuilder& b) {
        addFrameLoad(b, *s, v16, /*slot=*/4200);
    });
    auto const m = materializeAndAssemble(lir, *s, allocationWith(4300));
    ASSERT_TRUE(m->cc.ok()) << allMessages(m->ccRep);
    EXPECT_EQ(m->asmRep.errorCount(), 0u) << allMessages(m->asmRep);
    auto const* layout = m->cc.forFuncByIndex(0);
    ASSERT_NE(layout, nullptr);
    EXPECT_TRUE(savedContains(*layout, x30))
        << "the FP reload needs x30, so this leaf must save it";
    ASSERT_EQ(layout->spillAreaOffset() + 4199u * layout->slotSize, 67200u);
    EXPECT_TRUE(containsRun(wordsOf(m->bytes),
                            {kAddX30SpLoQ, kAddX30X30HiQ, kLdurQ16AtX30}))
        << "the reload must be `add x30, sp, #0x680; add x30, x30, #0x10, lsl "
           "#12; ldur q16, [x30]`";
}

// The same register file one slot INSIDE the Q reach keeps the scaled form and
// folds nothing — the control arm that proves the pin above is about the
// reach and not about an FP reload as such.
TEST(FrameFarForm, Arm64FpReloadInsideTheQReachStaysScaled) {
    auto s = shipped("arm64");
    ASSERT_NE(s, nullptr);
    LirReg const v16 = phys(*s, "v16", LirRegClass::FPR);
    LirReg const x30 = phys(*s, "x30", LirRegClass::GPR);
    auto const fldrU = s->opcodeByMnemonic("fldr_u");
    auto const lea   = s->opcodeByMnemonic("lea");
    ASSERT_TRUE(fldrU.has_value() && lea.has_value());
    Lir const lir = oneFunction(*s, [&](LirBuilder& b) {
        addFrameLoad(b, *s, v16, /*slot=*/2100);   // 33584: inside 65520
    });
    DiagnosticReporter rep;
    auto const cc = materializeCallingConvention(lir, *s, allocationWith(2200), rep);
    ASSERT_TRUE(cc.ok()) << allMessages(rep);
    auto const* layout = cc.forFuncByIndex(0);
    ASSERT_NE(layout, nullptr);
    EXPECT_FALSE(savedContains(*layout, x30));
    std::uint32_t scaledLoads = 0, leas = 0;
    LirFuncId const fn = cc.lir.funcAt(0);
    for (std::uint32_t bi = 0; bi < cc.lir.funcBlockCount(fn); ++bi) {
        LirBlockId const blk = cc.lir.funcBlockAt(fn, bi);
        for (std::uint32_t i = 0; i < cc.lir.blockInstCount(blk); ++i) {
            auto const op = cc.lir.instOpcode(cc.lir.blockInstAt(blk, i));
            if (op == *fldrU) ++scaledLoads;
            if (op == *lea) ++leas;
        }
    }
    EXPECT_EQ(scaledLoads, 1u);
    EXPECT_EQ(leas, 0u);
}

// ── NO BLAST RADIUS: A BIG FRAME IS NOT A FAR ACCESS ─────────────────────────
//
// The closed row's own witness: a 320000-byte local array, every element
// reached through GEP arithmetic in a register, and no frame access past the
// scaled reach. The frame must come out exactly as it did — no x30 save.
TEST(FrameFarForm, ALeafWithAHugeLocalButNoFarAccessKeepsItsFrame) {
    auto lowered = test_support::lowerCToLir(
        "int main(void) {\n"
        "    volatile long a[40000];\n"
        "    a[39999] = 42;\n"
        "    return (int)a[39999];\n"
        "}\n",
        "arm64", 0);
    ASSERT_TRUE(lowered.lir.ok);
    auto const liveness = analyzeLiveness(lowered.lir.lir);
    DiagnosticReporter raRep;
    auto const alloc = allocateRegisters(lowered.lir.lir, *lowered.target,
                                         liveness, 0, raRep);
    ASSERT_TRUE(alloc.ok());
    DiagnosticReporter rwRep;
    auto const rewritten = rewriteWithAllocation(lowered.lir.lir, *lowered.target,
                                                 alloc, rwRep);
    ASSERT_TRUE(rewritten.ok);
    DiagnosticReporter ccRep;
    auto const cc = materializeCallingConvention(rewritten.lir, *lowered.target,
                                                 alloc, ccRep);
    ASSERT_TRUE(cc.ok()) << allMessages(ccRep);
    auto const* layout = cc.forFuncByIndex(0);
    ASSERT_NE(layout, nullptr);
    EXPECT_GE(layout->totalFrameSize, 320000u);
    EXPECT_FALSE(savedContains(*layout,
                               phys(*lowered.target, "x30", LirRegClass::GPR)))
        << "a big frame with no far access must not pay for a scratch";
}

// ── STACKED PARAMETERS ABOVE A FRAME PAST THE REACH ─────────────────────────
//
// The minimal general trigger: the 9th/10th params of each file read from above
// a 40000-byte local. The integer ones address through their destinations; the
// FP ones need x30, and this leaf saves it.
TEST(FrameFarForm, Arm64StackedParamsAboveAFrameBeyondReachAssemble) {
    auto lowered = test_support::lowerCToLir(
        "double f(long a, long b, long c, long d, long e, long g, long h,\n"
        "         long i, long j, long k,\n"
        "         double p, double q, double r, double s, double t,\n"
        "         double u, double v, double w, double x, double y) {\n"
        "    volatile char buf[40000];\n"
        "    buf[0] = (char)a;\n"
        "    return (double)(j + k) + x + y + buf[0];\n"
        "}\n",
        "arm64", 0);
    ASSERT_TRUE(lowered.lir.ok);
    auto const liveness = analyzeLiveness(lowered.lir.lir);
    DiagnosticReporter raRep;
    auto const alloc = allocateRegisters(lowered.lir.lir, *lowered.target,
                                         liveness, 0, raRep);
    ASSERT_TRUE(alloc.ok());
    DiagnosticReporter rwRep;
    auto const rewritten = rewriteWithAllocation(lowered.lir.lir, *lowered.target,
                                                 alloc, rwRep);
    ASSERT_TRUE(rewritten.ok);
    DiagnosticReporter legRep;
    auto const legal = legalizeTwoAddress(rewritten.lir, *lowered.target, legRep);
    ASSERT_TRUE(legal.ok());
    DiagnosticReporter ccRep;
    auto const cc = materializeCallingConvention(legal.lir, *lowered.target,
                                                 alloc, ccRep);
    ASSERT_TRUE(cc.ok()) << allMessages(ccRep);
    auto const* layout = cc.forFuncByIndex(0);
    ASSERT_NE(layout, nullptr);
    EXPECT_TRUE(savedContains(*layout,
                              phys(*lowered.target, "x30", LirRegClass::GPR)))
        << "the stacked double params are read FAR into SIMD&FP registers, which "
           "need x30; this leaf must save it";
    std::vector<MirInstId> lirToMir(cc.lir.instCount(), InvalidMirInst);
    DiagnosticReporter asmRep;
    (void)assemble(cc.lir, *lowered.target, lirToMir, asmRep);
    EXPECT_EQ(asmRep.errorCount(), 0u)
        << "every stacked param above the 40000-byte local must encode:"
        << allMessages(asmRep);
}

// ── THE REFUSALS THAT REMAIN, EACH BY NAME ──────────────────────────────────

// A body that names the scratch itself may be holding a value in it, so a far
// store there is REFUSED — never routed through a register the body uses.
TEST(FrameFarForm, AScratchTheBodyNamesIsNeverBorrowed) {
    auto s = shipped("arm64");
    ASSERT_NE(s, nullptr);
    LirReg const x9  = phys(*s, "x9", LirRegClass::GPR);
    LirReg const x10 = phys(*s, "x10", LirRegClass::GPR);
    LirReg const x30 = phys(*s, "x30", LirRegClass::GPR);
    auto const mov = s->opcodeByMnemonic("mov");
    ASSERT_TRUE(mov.has_value());
    Lir const lir = oneFunction(*s, [&](LirBuilder& b) {
        std::array<LirOperand, 1> ops{LirOperand::makeReg(x30)};
        b.addInst(*mov, x10, ops);                  // the body reads x30 …
        addFrameStore(b, *s, x9, /*slot=*/2100);    // … and has a far store
    });
    DiagnosticReporter rep;
    auto const cc = materializeCallingConvention(lir, *s, allocationWith(2200), rep);
    EXPECT_FALSE(cc.ok());
    EXPECT_NE(allMessages(rep).find("named explicitly by this function"),
              std::string::npos) << allMessages(rep);
}

// Without a declared scratch, a far STORE has nowhere to put its address and is
// refused naming the missing key — while a far integer RELOAD, which needs no
// scratch, still materializes (the control arm).
TEST(FrameFarForm, WithoutADeclaredScratchOnlyTheFarStoreIsRefused) {
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "arm64", [](nlohmann::json& doc) {
            for (auto& cc : doc["callingConventions"]) cc.erase("frameAddressScratch");
        });
    ASSERT_TRUE(mutated.has_value());
    TargetSchema const& s = **mutated;
    LirReg const x9 = phys(s, "x9", LirRegClass::GPR);

    Lir const storeLir = oneFunction(s, [&](LirBuilder& b) {
        addFrameStore(b, s, x9, /*slot=*/2100);
    });
    DiagnosticReporter storeRep;
    auto const storeCc =
        materializeCallingConvention(storeLir, s, allocationWith(2200), storeRep);
    EXPECT_FALSE(storeCc.ok());
    EXPECT_NE(allMessages(storeRep).find("declares no `frameAddressScratch`"),
              std::string::npos) << allMessages(storeRep);

    Lir const loadLir = oneFunction(s, [&](LirBuilder& b) {
        addFrameLoad(b, s, x9, /*slot=*/2101);
    });
    DiagnosticReporter loadRep;
    auto const loadCc =
        materializeCallingConvention(loadLir, s, allocationWith(2200), loadRep);
    EXPECT_TRUE(loadCc.ok()) << "a far integer reload needs no scratch:"
                             << allMessages(loadRep);
}

// ── THE LOAD-TIME CONTRACT ON THE SCRATCH ───────────────────────────────────
//
// Each synthesizes the negative on the shipped document: a register the
// allocator can hand out, and a register with a live role.
TEST(FrameFarForm, AnAllocatableScratchIsRefusedAtLoad) {
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "arm64", [](nlohmann::json& doc) {
            doc["callingConventions"][0]["frameAddressScratch"] = "x16";
        });
    ASSERT_FALSE(mutated.has_value())
        << "x16 is in aapcs64's callerSaved — the allocator hands it to live "
           "values, so it must never be accepted as the frame-address scratch";
    bool named = false;
    for (auto const& d : mutated.error()) {
        if (d.message.find("frameAddressScratch") != std::string::npos
            && d.message.find("callerSaved") != std::string::npos)
            named = true;
    }
    EXPECT_TRUE(named) << "the refusal must name the key and the list";
}

TEST(FrameFarForm, AScratchHoldingALiveRoleIsRefusedAtLoad) {
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "arm64", [](nlohmann::json& doc) {
            doc["callingConventions"][0]["frameAddressScratch"] = "sp";
        });
    ASSERT_FALSE(mutated.has_value());
    bool named = false;
    for (auto const& d : mutated.error()) {
        if (d.message.find("stackPointer") != std::string::npos
            && d.message.find("frameAddressScratch") != std::string::npos)
            named = true;
    }
    EXPECT_TRUE(named) << "the stack pointer is live across every body";
}

// ── THE ENCODER'S REFUSAL NO LONGER CLAIMS A SUPPORTED FORM IS MISSING ──────
//
// An unscaled store written at an offset its field cannot hold is still refused
// — it is one instruction, and that instruction cannot carry the offset — but
// the sentence used to end "a larger frame needs the scaled LDR/STR imm12 form,
// not yet supported", about a form that was supported. A diagnostic is this
// compiler's output; this pins the one it prints now.
TEST(FrameFarForm, TheImm9RefusalStatesWhatIsTrue) {
    auto s = shipped("arm64");
    ASSERT_NE(s, nullptr);
    auto const store = s->opcodeByMnemonic("store");
    ASSERT_TRUE(store.has_value());
    Lir const lir = oneFunction(*s, [&](LirBuilder& b) {
        std::array<LirOperand, 4> ops{
            LirOperand::makeReg(phys(*s, "x1", LirRegClass::GPR)),
            LirOperand::makeReg(phys(*s, "sp", LirRegClass::GPR)),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(32768)};
        b.addInst(*store, InvalidLirReg, ops);
    });
    std::vector<MirInstId> lirToMir(lir.instCount(), InvalidMirInst);
    DiagnosticReporter rep;
    (void)assemble(lir, *s, lirToMir, rep);
    std::string const all = allMessages(rep);
    EXPECT_NE(all.find("this unscaled form cannot carry it"), std::string::npos) << all;
    EXPECT_EQ(all.find("not yet supported"), std::string::npos) << all;
}
