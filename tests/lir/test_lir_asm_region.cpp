// P68 round 8 part 4 — THE INLINE-ASM BUNDLE (`lir/lir_asm_region.hpp`).
//
// ★★★ WHAT THIS FILE PINS: one inline-asm statement is ONE instruction while
// registers are allocated, and its template becomes real instructions only once
// the allocation is final. The defect it closes — D-LIR-ASM-TEMPLATE-SPILL-CODE-LANDS-BETWEEN-TEMPLATE-INSTRUCTIONS
// — was the allocator's spill and reload code landing BETWEEN a template's
// lines (✔MEASURED 2026-09-21: x86_64, a 7-line template, six inputs, release:
// 29 instructions and 15 memory accesses between its two `nop` delimiters),
// which an `ldaxr`/`stlxr` pair does not survive.
//
// The arms:
//   (A) the bundle's shape: one instruction per statement, one SLOT per operand
//       register, a ROLE per slot, the template in the body;
//   (B) ★ the pin the defect is stated by: under pressure, NOTHING the allocator
//       writes lies between a template's first and last line, on both targets;
//   (C) a statement's output avoids the statement's clobbered registers;
//   (D) capacity: a statement that needs more registers of one class at once
//       than the target has is refused BY NAME — exactly where gcc 13.3.0 and
//       clang 18.1.3 refuse it — and the largest one either reference
//       allocates compiles (arm64 Linux: 31, the link register included);
//   (E) a one-block template expands with nothing added;
//   (F) the verifier refuses a malformed bundle;
//   (G) the `.dsslir` codec round-trips a bundle byte for byte.

#include "asm_region_test_support.hpp"
#include "lowered_lir_fixture.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_asm_region.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_text.hpp"
#include "lir/lir_verifier.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::test_support;

namespace {

[[nodiscard]] std::string summarize(LoweredLir const& r) {
    std::string s;
    for (auto const* rep : {&r.hirReporter, &r.mirReporter, &r.lirReporter}) {
        for (auto const& d : rep->all()) s += "\n  " + d.actual;
    }
    return s.empty() ? std::string{"<no diagnostics>"} : s;
}

[[nodiscard]] std::uint16_t opOf(TargetSchema const& t, std::string_view m) {
    auto const i = t.opcodeByMnemonic(m);
    EXPECT_TRUE(i.has_value()) << "target declares no '" << m << "'";
    return i.has_value() ? *i : std::uint16_t{0};
}

// Every instruction of the module, blocks in function order.
[[nodiscard]] std::vector<LirInstId> allInsts(Lir const& lir) {
    std::vector<LirInstId> out;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const f = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(f); ++bi) {
            LirBlockId const b = lir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(b); ++ii) {
                out.push_back(lir.blockInstAt(b, ii));
            }
        }
    }
    return out;
}

// The pressure probe: ONE statement with `n` register inputs and one
// earlyclobber output, whose template is `nop`, a first line, one line per
// input, and `nop`; every input also stays live AFTER the statement, so the
// function holds 2n values around it. The two `nop`s delimit the template in
// the emitted code — the compiler emits none of its own.
[[nodiscard]] std::string pressureProbe(std::size_t n, std::string_view first,
                                        std::string_view addFmt) {
    std::string params, ins, tail, lines = "nop\\n\\t";
    lines += first;
    for (std::size_t i = 0; i < n; ++i) {
        if (i > 0) { params += ", "; ins += ", "; tail += " + "; }
        params += "long a" + std::to_string(i);
        ins += "\"r\"(a" + std::to_string(i) + ")";
        tail += "a" + std::to_string(i);
        std::string line{addFmt};
        line.replace(line.find('#'), 1, std::to_string(i + 1));
        lines += "\\n\\t" + line;
    }
    lines += "\\n\\tnop";
    return "long f(" + params + ") {\n  long r;\n  __asm__ volatile (\"" + lines +
           "\" : \"=&r\"(r) : " + ins + ");\n  return r - (" + tail + ");\n}\n";
}

[[nodiscard]] std::string x86Probe(std::size_t n) {
    return pressureProbe(n, "xorq %0, %0", "addq %#, %0");
}
[[nodiscard]] std::string a64Probe(std::size_t n) {
    return pressureProbe(n, "mov %0, #0", "add %0, %0, %#");
}

[[nodiscard]] bool anyCode(LoweredLir const& r, DiagnosticReporter const& extra,
                           DiagnosticCode code) {
    for (auto const* rep : {&r.hirReporter, &r.mirReporter, &r.lirReporter, &extra}) {
        for (auto const& d : rep->all()) {
            if (d.code == code) return true;
        }
    }
    return false;
}

} // namespace

// ── (A) ONE STATEMENT, ONE BUNDLE, ONE SLOT PER OPERAND REGISTER ─────────────
TEST(LirAsmRegion, EachStatementIsOneBundleAndEachOperandRegisterOneSlotWithItsRole) {
    auto L = lowerCToLir(
        "long f(long p, long q, long *m) {\n"
        "    long a, b, c = q;\n"
        "    __asm__(\"movq %3, %0\\n\\tmovq %3, %1\\n\\taddq %4, %2\\n\\taddq %5, %2\"\n"
        "            : \"=r\"(a), \"=&r\"(b), \"+r\"(c) : \"r\"(p), \"r\"(q), \"m\"(*m));\n"
        "    return a + b + c;\n"
        "}\n",
        "x86_64");
    ASSERT_TRUE(L.lir.ok) << summarize(L);
    Lir const& lir = L.lir.lir;
    auto const bundle = onlyAsmRegion(lir);
    ASSERT_TRUE(bundle.has_value());
    LirAsmRegion const& r = *bundle->region;
    EXPECT_EQ(lir.instOpcode(bundle->bundle), opOf(*L.target, "asm_region"));
    EXPECT_FALSE(lir.instResult(bundle->bundle).valid())
        << "a bundle defines through its slots, never through a result";

    // Six registers: three outputs (`+r` is ONE register read and written),
    // two register inputs, and the ADDRESS a memory operand is read through.
    auto const ops = lir.instOperands(bundle->bundle);
    ASSERT_EQ(ops.size(), 6u);
    ASSERT_EQ(r.roles.size(), 6u);
    for (std::size_t k = 0; k < ops.size(); ++k) {
        ASSERT_EQ(ops[k].kind, LirOperandKind::Reg);
        EXPECT_EQ(ops[k].reg.isPhysical, 0u) << "a slot is allocated";
        EXPECT_EQ(ops[k].reg, r.bodyRegs[k])
            << "before allocation a slot's operand IS the register its body names";
    }
    auto const count = [&](LirAsmOperandRole role) {
        return std::count(r.roles.begin(), r.roles.end(), role);
    };
    EXPECT_EQ(count(LirAsmOperandRole::Def), 1) << "`=r`";
    EXPECT_EQ(count(LirAsmOperandRole::EarlyDef), 1) << "`=&r`";
    EXPECT_EQ(count(LirAsmOperandRole::UseDef), 1) << "`+r`, one register";
    EXPECT_EQ(count(LirAsmOperandRole::Use), 3)
        << "two register inputs and the memory operand's address";

    // The template is the BODY — four lines, the last two the template's
    // `addq`s, which read a slot and write the `+r` slot.
    std::size_t lines = 0;
    std::size_t addsOnTheTie = 0;
    LirReg tie = InvalidLirReg;
    for (std::size_t k = 0; k < r.roles.size(); ++k) {
        if (r.roles[k] == LirAsmOperandRole::UseDef) tie = r.bodyRegs[k];
    }
    for (LirInstId const i : asmRegionBodyInsts(r)) {
        if (L.target->isTerminator(r.body.instOpcode(i))) continue;
        ++lines;
        if (r.body.instOpcode(i) == opOf(*L.target, "add")
            && r.body.instResult(i) == tie) {
            ++addsOnTheTie;
        }
    }
    EXPECT_EQ(lines, 4u);
    EXPECT_EQ(addsOnTheTie, 2u) << "`addq %4, %2` and `addq %5, %2`";
}

// ── (B) ★ NOTHING THE ALLOCATOR WRITES LIES BETWEEN A TEMPLATE'S LINES ───────
//
// The defect's own shape, on both processors: a statement under enough
// pressure that the function SPILLS (asserted — the pin proves nothing
// otherwise), taken through the pipeline to the point callconv receives, where
// the template has become real instructions. Between the two `nop`s there must
// be exactly the template's other lines: no reload, no spill, no copy.
namespace {
void expectTemplateContiguous(std::string const& target, std::string const& src,
                              std::size_t templateLines) {
    SCOPED_TRACE(target);
    auto L = lowerCToLir(src, target);
    ASSERT_TRUE(L.lir.ok) << summarize(L);
    auto const expanded = lirThroughAsmExpansion(L.lir.lir, *L.target);
    ASSERT_TRUE(expanded.has_value());
    auto const nop  = opOf(*L.target, "nop");
    auto const load = opOf(*L.target, "frame_load");
    auto const stor = opOf(*L.target, "frame_store");
    auto const insts = allInsts(*expanded);
    std::vector<std::size_t> nops;
    std::size_t frameOps = 0;
    for (std::size_t i = 0; i < insts.size(); ++i) {
        auto const op = expanded->instOpcode(insts[i]);
        if (op == nop) nops.push_back(i);
        if (op == load || op == stor) ++frameOps;
    }
    ASSERT_EQ(nops.size(), 2u) << "the template's two delimiters";
    EXPECT_GT(frameOps, 0u)
        << "anti-vacuity: the function must SPILL, or nothing could have "
           "landed between the template's lines in the first place";
    EXPECT_EQ(nops[1] - nops[0] - 1, templateLines - 2)
        << "exactly the template's own lines lie between its delimiters";
    for (std::size_t i = nops[0] + 1; i < nops[1]; ++i) {
        auto const op = expanded->instOpcode(insts[i]);
        EXPECT_NE(op, load) << "a reload inside the template";
        EXPECT_NE(op, stor) << "a spill inside the template";
    }
}
} // namespace

TEST(LirAsmRegion, NothingTheAllocatorWritesLandsBetweenATemplatesLines) {
    // x86_64: 14 inputs + the earlyclobber output = 15 registers, every one of
    // the 15 the target allocates; 14 more values live across.
    expectTemplateContiguous("x86_64", x86Probe(14), 14 + 3);
    // arm64: 26 inputs + the output, 26 more values live across.
    expectTemplateContiguous("arm64", a64Probe(26), 26 + 3);
}

// ── (C) A STATEMENT'S OUTPUT AVOIDS THE STATEMENT'S CLOBBERED REGISTERS ──────
//
// GCC forbids an operand in a clobbered register: the template may write a
// clobbered register AFTER it wrote the output. An output is defined at the
// bundle's LATE slot, where the covering-range rule (which consults the clobber
// at the EARLY slot) does not reach it, so the allocator needs the rule stated
// for the output itself. With every caller-saved register clobbered, the output
// has to land in a callee-saved one.
TEST(LirAsmRegion, AStatementsOutputAvoidsItsClobberedRegisters) {
    auto L = lowerCToLir(
        "long f(void) {\n"
        "    long o;\n"
        "    __asm__(\"movq $1, %0\\n\\tmovq $99, %%r11\" : \"=r\"(o) : :\n"
        "            \"rax\", \"rcx\", \"rdx\", \"rsi\", \"rdi\", \"r8\", \"r9\", \"r10\", \"r11\");\n"
        "    return o;\n"
        "}\n",
        "x86_64");
    ASSERT_TRUE(L.lir.ok) << summarize(L);
    auto const bundle = onlyAsmRegion(L.lir.lir);
    ASSERT_TRUE(bundle.has_value());
    auto const ops = L.lir.lir.instOperands(bundle->bundle);
    ASSERT_EQ(ops.size(), 1u);
    LirReg const out = ops[0].reg;

    DiagnosticReporter rep;
    auto const lv = analyzeLiveness(L.lir.lir);
    auto const alloc = allocateRegisters(L.lir.lir, *L.target, lv, 0, rep);
    ASSERT_TRUE(alloc.ok());
    auto const* a = alloc.perFunc.at(0).forVReg(out.id);
    ASSERT_NE(a, nullptr);
    ASSERT_FALSE(a->isSpilled());
    std::string const name = L.target->registerInfo(
        static_cast<std::uint16_t>(a->physReg().id))->name;
    for (char const* clobbered :
         {"rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"}) {
        EXPECT_NE(name, clobbered)
            << "the output was allocated a register the statement clobbers";
    }
}

// ── (D) CAPACITY: REFUSED BY NAME EXACTLY WHERE THE REFERENCES REFUSE ────────
//
// ✔MEASURED 2026-09-22, gcc 13.3.0 and clang 18.1.3, each at -O0 and -O2:
//   x86_64 — 14 inputs + an earlyclobber output (15 registers) compiles and
//     runs at -O2 in both (at -O0 both refuse: they keep %rbp as the frame
//     pointer); 15 + 1 (16 registers) is refused by both at every level
//     ("'asm' operand has impossible constraints" / "inline assembly requires
//     more registers than available").
//   arm64 Linux — 29 + 1 (30) compiles in both; 30 + 1 (31: x0–x30) compiles
//     and runs in clang (gcc refuses it only for its own 30-operand limit).
// DSS compiles every statement a reference allocates, at debug as well as
// release (it keeps no frame pointer on x86_64 unless a VLA needs one), and
// refuses the rest with `L_AsmRegionOperandUnallocatable`.
namespace {
[[nodiscard]] bool allocatesAndRewrites(std::string const& target,
                                            std::string const& src,
                                            bool& refusedByName) {
    auto L = lowerCToLir(src, target);
    EXPECT_TRUE(L.lir.ok) << summarize(L);
    if (!L.lir.ok) return false;
    DiagnosticReporter rep;
    auto wide = lowerWideCallArgs(L.lir.lir, *L.target, 0, rep);
    auto const lv = analyzeLiveness(wide.lir);
    auto const alloc = allocateRegisters(wide.lir, *L.target, lv, 0, rep);
    auto rewritten = rewriteWithAllocation(wide.lir, *L.target, alloc, rep);
    refusedByName = anyCode(L, rep, DiagnosticCode::L_AsmRegionOperandUnallocatable);
    return rewritten.ok;
}
} // namespace

TEST(LirAsmRegion, TooManyRegistersAtOnceIsRefusedByNameWhereTheReferencesRefuse) {
    bool named = false;
    EXPECT_TRUE(allocatesAndRewrites("x86_64", x86Probe(14), named))
        << "15 registers: gcc and clang allocate it";
    EXPECT_FALSE(named);
    EXPECT_FALSE(allocatesAndRewrites("x86_64", x86Probe(15), named))
        << "16 registers: every reference refuses it";
    EXPECT_TRUE(named) << "refused BY NAME, never by a generic exhaustion";
    EXPECT_TRUE(allocatesAndRewrites("arm64", a64Probe(30), named))
        << "31 registers on Linux arm64: clang allocates it, x30 included";
    EXPECT_FALSE(named);
    EXPECT_FALSE(allocatesAndRewrites("arm64", a64Probe(31), named))
        << "32 registers: arm64 has 31";
    EXPECT_TRUE(named);
}

// ── (D′) THE LINK REGISTER: THE LAST RESORT, AND SAVED WHEN TAKEN ────────────
TEST(LirAsmRegion, TheLinkRegisterHoldsAnOperandOnlyWhenNothingElseIsLeft) {
    auto const lrOf = [](TargetSchema const& t) {
        return static_cast<std::uint32_t>(*t.registerByName("x30"));
    };
    for (std::size_t n : {std::size_t{20}, std::size_t{30}}) {
        SCOPED_TRACE(n);
        auto L = lowerCToLir(a64Probe(n), "arm64");
        ASSERT_TRUE(L.lir.ok) << summarize(L);
        DiagnosticReporter rep;
        auto wide = lowerWideCallArgs(L.lir.lir, *L.target, 0, rep);
        auto const lv = analyzeLiveness(wide.lir);
        auto const alloc = allocateRegisters(wide.lir, *L.target, lv, 0, rep);
        auto rewritten = rewriteWithAllocation(wide.lir, *L.target, alloc, rep);
        ASSERT_TRUE(rewritten.ok);
        auto const bundle = onlyAsmRegion(rewritten.lir);
        ASSERT_TRUE(bundle.has_value());
        std::size_t inLr = 0;
        for (auto const& o : rewritten.lir.instOperands(bundle->bundle)) {
            if (o.kind == LirOperandKind::Reg && o.reg.isPhysical != 0
                && o.reg.id == lrOf(*L.target)) {
                ++inLr;
            }
        }
        EXPECT_EQ(inLr, n == 30 ? 1u : 0u)
            << "x30 holds an operand only when all 30 allocatable registers "
               "are already holding the statement's other operands";
    }
}

// ── (E) A ONE-BLOCK TEMPLATE EXPANDS WITH NOTHING ADDED ──────────────────────
TEST(LirAsmRegion, AOneBlockTemplateExpandsWithNothingAdded) {
    for (auto const& target : {std::string{"x86_64"}, std::string{"arm64"}}) {
        SCOPED_TRACE(target);
        auto L = lowerCToLir("void f(long x) { __asm__ volatile(\"nop\\n\\tnop\" : : \"r\"(x)); }\n",
                             target);
        ASSERT_TRUE(L.lir.ok) << summarize(L);
        std::size_t const blocksBefore = L.lir.lir.blockCount();
        auto const expanded = lirThroughAsmExpansion(L.lir.lir, *L.target);
        ASSERT_TRUE(expanded.has_value());
        std::size_t nops = 0, jmps = 0;
        for (LirInstId const i : allInsts(*expanded)) {
            if (expanded->instOpcode(i) == opOf(*L.target, "nop")) ++nops;
            if (expanded->instOpcode(i) == opOf(*L.target, "jmp")) ++jmps;
        }
        EXPECT_EQ(nops, 2u);
        EXPECT_EQ(jmps, 0u)
            << "a template that falls off its end continues in place — the "
               "expansion adds no branch";
        EXPECT_EQ(expanded->blockCount(), blocksBefore)
            << "and no block";
    }
}

// ── (F) THE VERIFIER REFUSES A MALFORMED BUNDLE ──────────────────────────────
namespace {
[[nodiscard]] std::shared_ptr<LirAsmRegion>
oneSlotRegion(TargetSchema const& t, LirReg slot) {
    LirBuilder body{t};
    (void)body.addFunction(SymbolId{});
    LirBlockId const entry = body.createBlock();
    LirBlockId const exit  = body.createBlock();
    body.beginBlock(entry);
    (void)body.addInst(*t.opcodeByMnemonic("nop"), InvalidLirReg, {});
    (void)body.addBr(*t.opcodeByMnemonic("jmp"), exit);
    body.beginBlock(exit);
    (void)body.addBr(*t.opcodeByMnemonic("jmp"), exit);
    auto r = std::make_shared<LirAsmRegion>();
    r->roles    = {LirAsmOperandRole::Use};
    r->bodyRegs = {slot};
    r->body     = std::move(body).finish();
    r->exit     = exit;
    r->syntheticFallthrough = {1, 0};
    return r;
}

[[nodiscard]] bool verifierSays(Lir const& lir, TargetSchema const& t,
                                DiagnosticCode code) {
    DiagnosticReporter rep;
    (void)verifyLirText(lir, t, rep);
    return std::any_of(rep.all().begin(), rep.all().end(),
                       [&](auto const& d) { return d.code == code; });
}
} // namespace

TEST(LirAsmRegion, TheVerifierRefusesABundleWithoutItsRegionAndARegionOnAnotherOpcode) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& t = **target;
    auto const build = [&](bool bundleWithoutRegion, bool regionOnMov) {
        LirBuilder b{t};
        (void)b.addFunction(SymbolId{1});
        LirBlockId const entry = b.createBlock();
        b.beginBlock(entry);
        LirReg const v = b.newVReg(LirRegClass::GPR);
        std::array<LirOperand, 1> const imm{LirOperand::makeImmInt32(1)};
        (void)b.addInst(*t.opcodeByMnemonic("mov"), v, imm);
        std::array<LirOperand, 1> const use{LirOperand::makeReg(v)};
        std::uint32_t const idx = b.asmRegionPoolAdd(oneSlotRegion(t, v));
        if (regionOnMov) {
            LirReg const w = b.newVReg(LirRegClass::GPR);
            LirInstId const mov = b.addInst(*t.opcodeByMnemonic("mov"), w, use);
            b.setInstAsmRegion(mov, idx);
        } else {
            LirInstId const bundle =
                b.addInst(*t.opcodeByMnemonic("asm_region"), InvalidLirReg, use);
            if (!bundleWithoutRegion) b.setInstAsmRegion(bundle, idx);
        }
        (void)b.addReturn(*t.opcodeByMnemonic("ret"), {});
        return std::move(b).finish();
    };
    EXPECT_FALSE(verifierSays(build(false, false), t, DiagnosticCode::L_AsmRegionMalformed))
        << "control: a well-formed bundle verifies";
    EXPECT_TRUE(verifierSays(build(true, false), t, DiagnosticCode::L_AsmRegionMalformed))
        << "a bundle opcode with no region is a statement with no template";
    EXPECT_TRUE(verifierSays(build(true, false), t, DiagnosticCode::L_SideStructureReferenceLost))
        << "…and its region is referenced by nothing";
    EXPECT_TRUE(verifierSays(build(false, true), t, DiagnosticCode::L_AsmRegionMalformed))
        << "a region on an ordinary instruction gives it roles it never had";
}

// ── (G) THE `.dsslir` CODEC ROUND-TRIPS A BUNDLE ─────────────────────────────
TEST(LirAsmRegion, TheTextCodecRoundTripsABundleByteForByte) {
    for (auto const& [target, src] :
         {std::pair<std::string, std::string>{
              "x86_64",
              "long f(long p, long q) { long a, c = q;\n"
              "  __asm__(\"movq %2, %0\\n\\taddq %3, %1\" : \"=&r\"(a), \"+r\"(c) : \"r\"(p), \"r\"(q));\n"
              "  return a + c; }\n"},
          std::pair<std::string, std::string>{
              "arm64",
              "int f(int x) { int r = 0;\n"
              "  __asm__ goto(\"cbz %w1, %l2\\n\\tmov %w0, #1\" : \"+r\"(r) : \"r\"(x) : : hit);\n"
              "  return r;\n"
              "hit: return 7; }\n"}}) {
        SCOPED_TRACE(target);
        auto L = lowerCToLir(src, target);
        ASSERT_TRUE(L.lir.ok) << summarize(L);
        ASSERT_FALSE(asmRegionBundles(L.lir.lir).empty());
        LirTextContext const ctx{};
        DiagnosticReporter rep;
        std::string const text = emitLir(L.lir.lir, *L.target, ctx, rep);
        EXPECT_NE(text.find("asm_regions {"), std::string::npos);
        EXPECT_NE(text.find(" ar=1"), std::string::npos);
        auto parsed = parseLir(text, *L.target, rep);
        ASSERT_TRUE(parsed != nullptr && parsed->ok)
            << (rep.all().empty() ? std::string{} : rep.all().back().actual);
        EXPECT_EQ(emitLir(parsed->lir, *L.target, ctx, rep), text);
        EXPECT_EQ(asmRegionBundles(parsed->lir).size(),
                  asmRegionBundles(L.lir.lir).size());
    }
}
