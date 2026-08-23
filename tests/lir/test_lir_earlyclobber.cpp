// `"=&r"` — EARLYCLOBBER ON AN ALLOCATOR-CHOSEN REGISTER.
// D-LIR-EARLYCLOBBER-FLAG-UNSETTABLE-AFTER-EMISSION.
//
// ★★★ THE CLAIM, AND WHY IT TAKES **TWO** ARMS TO STATE IT.
// GNU's `&` means "this output is written before every input has been read",
// so it may not share a register with any input. The only way to test that is
// against the behaviour it forbids — and that behaviour is REAL:
// ✔MEASURED on gcc and clang, a PLAIN `"=r"` output shares its register with
// an input (`%0=%eax %1=%eax` on x86_64, `x0 x0` on aarch64). DSS's linear
// scan does the same, for the same reason: an ordinary instruction reads every
// operand before it writes anything, so the input's live range ends exactly
// where the result's def begins, nothing overlaps, and the register is reused.
//
// ⇒ a test that only checked "`&` yields distinct registers" would pass on an
// allocator that never shares ANY register — it would assert nothing at all.
// Every behavioural test below is therefore a MATCHED PAIR over one bit of
// difference: `EXPECT_EQ` on the plain arm (sharing really happens) and
// `EXPECT_NE` on the `&` arm (the flag really prevents it). The control is the
// load-bearing half, and it runs on BOTH shipped targets because the
// reference-compiler measurement it mirrors was taken on both.
//
// ★★ THE FLAG IS SET **AFTER** THE INSTRUCTION IS APPENDED, WHICH IS THE WHOLE
// FIX. Both ends of the mechanism already existed — `kLirInstFlagEarlyClobber
// Result` and its `lir_liveness.cpp` consumer — and could not be connected:
// the instruction that writes an unpinned `"=&r"` output is emitted by the
// SHARED assembly engine, and `LirBuilder::addInst` takes `flags` BY VALUE with
// no `setInstFlags` sibling to `setInstRegConstraints`. So the tests below
// never pass the flag to `addInst`. They discard the returned id, then recover
// the defining instruction by scanning the id range and matching the result
// vreg — byte-for-byte the walk `mir_to_lir.cpp`'s asm expansion performs over
// the ids the engine minted.
//
// ★★ THE CLAIM IS MADE TWICE, ON TWO DIFFERENT INPUTS, AND BOTH ARMS ARE HERE
// ON PURPOSE.
//   (1) a HAND-BUILT LIR module — no dialect, no grammar, no config. It states
//       the allocator property in the smallest terms that can state it, and it
//       stays exercisable when a dialect document is mid-edit (which is not
//       hypothetical: the shipped dialects did not load for part of the cycle
//       that wrote this file).
//   (2) the REAL EMBEDDED PATH — the shipped dialect, parsed through
//       `parseAsmTemplateText` on the EXTENDED surface with genuine `%0`/`%1`
//       placeholders, lowered by the shared engine. This is the arm that would
//       notice if the placeholder surface, the binding table or the engine
//       stopped producing the single-instruction shape the flag is aimed at.
// ⚠ A hand-built module can only ever re-state its author's belief about what
// the compiler emits — this repo has shipped false rules exactly that way — so
// (1) alone would not be enough, and (2) alone would couple the allocator claim
// to config another lane owns. Neither is redundant.
//
// ⚠ SINGLE-INSTRUCTION SHAPE ON PURPOSE. A multi-instruction template is
// already safe without the flag: an output written at instruction 1 and an
// input read at instruction 3 have OVERLAPPING ranges, so the allocator
// separates them anyway. The single-instruction shape is the one where the
// input's range ends exactly at the result's def, and it is what a real
// `__asm__("op %1, %0" : "=&r"(o) : "r"(i))` produces.
//
// ⚠ CONFIG-LEVEL: `dss_add_test` sets `DSS_CONFIG_ROOT`, so this file must run
// through ctest and never as a bare `.exe`
// (D-TEST-CONFIG-RED-ON-DISABLE-READS-THE-WRONG-TREE).

#include "asm/asm_template_to_lir.hpp"
#include "core/types/config_path_walk.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_regalloc.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] std::shared_ptr<TargetSchema> shipped(std::string_view name) {
    auto r = TargetSchema::loadShipped(name);
    if (!r.has_value()) {
        throw std::runtime_error{std::string{"cannot load shipped target "}
                                 + std::string{name}};
    }
    return *r;
}

[[nodiscard]] std::uint16_t op(TargetSchema const& t, std::string_view m) {
    auto const i = t.opcodeByMnemonic(m);
    if (!i.has_value()) {
        throw std::runtime_error{std::string{"target declares no "}
                                 + std::string{m}};
    }
    return *i;
}

struct Built {
    std::shared_ptr<TargetSchema> target;
    Lir                           lir;
    std::uint32_t                 inVReg  = 0;
    std::uint32_t                 outVReg = 0;
};

// `f() { vin = mov #7 ; vout = mov vin ; ret vout }`, with the middle
// instruction optionally marked earlyclobber AFTER it was appended.
//
// ★ THE SEEDING `mov` AND THE `ret` ARE BOTH LOAD-BEARING, not scaffolding.
// Without the seed, `vin` has no definition and its range starts at position 0
// — the malformed shape the liveness builder tolerates defensively, so the
// sharing question would be asked about a range that describes no real value.
// Without the `ret` reading `vout`, `vout` is dead and the allocator is free
// to do anything with it. The two arms are byte-identical apart from the flag.
[[nodiscard]] Built buildSingleInstructionAsmShape(std::string_view targetName,
                                                   bool earlyClobber) {
    Built out;
    out.target = shipped(targetName);
    auto const movOp = op(*out.target, "mov");
    auto const retOp = op(*out.target, "ret");

    LirBuilder b{*out.target};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);

    LirReg const vin  = b.newVReg(LirRegClass::GPR);
    LirReg const vout = b.newVReg(LirRegClass::GPR);
    out.inVReg  = vin.id;
    out.outVReg = vout.id;

    std::array<LirOperand, 1> const seed{LirOperand::makeImmInt32(7)};
    (void)b.addInst(movOp, vin, seed);

    // The id the "engine" is about to mint. `addInst` returns `size() - 1` and
    // the ids are contiguous, which is exactly why a producer that never held
    // the returned id can still reach the instruction.
    std::uint32_t const firstOpaque = b.lastInst().v + 1u;

    // ⚠ THE RETURNED ID IS DISCARDED ON PURPOSE. Keeping it would let this
    // test stamp the flag through a handle the real producer does not have,
    // and the code path under test — recover the def by scanning the range —
    // would never run.
    {
        std::array<LirOperand, 1> const src{LirOperand::makeReg(vin)};
        (void)b.addInst(movOp, vout, src);
    }

    if (earlyClobber) {
        std::uint32_t const end = b.lastInst().v + 1u;
        std::uint32_t stamped = 0;
        for (std::uint32_t v = firstOpaque; v < end; ++v) {
            LirInstId const li{v, b.id().v};
            if (!(b.instResult(li) == vout)) continue;
            b.orInstFlags(li, kLirInstFlagEarlyClobberResult);
            ++stamped;
        }
        if (stamped != 1) {
            throw std::runtime_error{
                "expected exactly one instruction to define the output vreg, "
                "stamped " + std::to_string(stamped)};
        }
    }

    std::array<LirOperand, 1> const ret{LirOperand::makeReg(vout)};
    (void)b.addReturn(retOp, ret);
    out.lir = std::move(b).finish();
    return out;
}

[[nodiscard]] std::optional<std::uint16_t>
physOrdinalOf(LirAllocation const& alloc, std::uint32_t vregId) {
    auto const* a = alloc.perFunc.at(0).forVReg(vregId);
    if (a == nullptr || a->isSpilled()) return std::nullopt;
    return static_cast<std::uint16_t>(a->physReg().id);
}

struct Pair {
    std::uint16_t in  = 0;
    std::uint16_t out = 0;
};

[[nodiscard]] Pair allocateAndRead(Built const& built) {
    DiagnosticReporter  rep;
    LirLiveness const   lv = analyzeLiveness(built.lir);
    LirAllocation const alloc =
        allocateRegisters(built.lir, *built.target, lv, /*ccIndex=*/0, rep);
    if (!alloc.ok()) throw std::runtime_error{"register allocation failed"};
    auto const in  = physOrdinalOf(alloc, built.inVReg);
    auto const out = physOrdinalOf(alloc, built.outVReg);
    if (!in.has_value() || !out.has_value()) {
        throw std::runtime_error{
            "a vreg spilled — the pair is only meaningful between two "
            "register-resident values"};
    }
    return Pair{*in, *out};
}

[[nodiscard]] std::string regName(TargetSchema const& t, std::uint16_t ord) {
    auto const* info = t.registerInfo(ord);
    return info == nullptr ? std::string{"<?>"} : info->name;
}

void expectEarlyClobberSeparatesWhatAPlainOutputShares(std::string_view target) {
    // ── THE CONTROL. Without the flag, the output takes the input's register,
    // which is what gcc and clang both do for a plain `"=r"`. If this ever
    // stops holding, the `&` arm below stops proving anything and must not be
    // read as a pass.
    Built const plain = buildSingleInstructionAsmShape(target, false);
    Pair  const pa    = allocateAndRead(plain);
    EXPECT_EQ(pa.in, pa.out)
        << target << ": a PLAIN output did NOT share its input's register ("
        << regName(*plain.target, pa.in) << " vs "
        << regName(*plain.target, pa.out)
        << "). The earlyclobber assertion below is vacuous unless sharing is "
           "the default — an allocator that never shares makes `&` a no-op "
           "that tests nothing.";

    // ── THE SUBJECT. One flag, on one instruction, set after emission.
    Built const early = buildSingleInstructionAsmShape(target, true);
    Pair  const eb    = allocateAndRead(early);
    EXPECT_NE(eb.in, eb.out)
        << target << ": `\"=&r\"` left the output in the input's register ("
        << regName(*early.target, eb.in)
        << ") — the template may write the output before it has finished "
           "reading the input, so sharing destroys the input with no "
           "diagnostic";
}

} // namespace

TEST(LirEarlyClobber, EarlyClobberSeparatesWhatAPlainOutputSharesOnX86_64) {
    expectEarlyClobberSeparatesWhatAPlainOutputShares("x86_64");
}

TEST(LirEarlyClobber, EarlyClobberSeparatesWhatAPlainOutputSharesOnArm64) {
    expectEarlyClobberSeparatesWhatAPlainOutputShares("arm64");
}

// The flag lands on exactly the stamped instruction and survives into the
// frozen module. The negative halves keep the positive one honest: a builder
// that set the bit on EVERY instruction would satisfy a positive-only
// assertion, and so would a `finish()` that re-derived flags from the opcode.
TEST(LirEarlyClobber, TheFlagLandsOnExactlyTheStampedInstruction) {
    Built const early = buildSingleInstructionAsmShape("x86_64", true);
    LirBlockId const bb = early.lir.funcBlockAt(early.lir.funcAt(0), 0);
    ASSERT_EQ(early.lir.blockInstCount(bb), 3u);

    EXPECT_FALSE(lirInstResultIsEarlyClobber(
        early.lir.instFlags(early.lir.blockInstAt(bb, 0))))
        << "the seeding `mov` was not stamped and must stay unflagged";
    EXPECT_TRUE(lirInstResultIsEarlyClobber(
        early.lir.instFlags(early.lir.blockInstAt(bb, 1))))
        << "the instruction defining the `&` output was stamped";
    EXPECT_FALSE(lirInstResultIsEarlyClobber(
        early.lir.instFlags(early.lir.blockInstAt(bb, 2))))
        << "the terminator was not stamped and must stay unflagged";

    // And the control build carries the bit NOWHERE — otherwise the pair above
    // differs by something other than the flag.
    Built const plain = buildSingleInstructionAsmShape("x86_64", false);
    LirBlockId const pb = plain.lir.funcBlockAt(plain.lir.funcAt(0), 0);
    for (std::uint32_t i = 0; i < plain.lir.blockInstCount(pb); ++i) {
        EXPECT_FALSE(lirInstResultIsEarlyClobber(
            plain.lir.instFlags(plain.lir.blockInstAt(pb, i))))
            << "instruction " << i << " of the CONTROL build is flagged";
    }
}

// ── ARM (2): THE REAL EMBEDDED PATH ──────────────────────────────────
//
// Same claim, but the subject instruction now comes out of the SHIPPED dialect
// and the SHARED text→LIR engine, from a template written the way a C
// programmer writes one — `%0` and `%1`, on the EXTENDED surface. This is the
// shape `mir_to_lir.cpp`'s expansion produces, and the flag is stamped exactly
// as it stamps it: by scanning the ids the engine minted for the one whose
// result is the `&` output's vreg.
//
// ⚠ `AsmTemplateSurface::Extended` IS NOT A DEFAULT AND MUST NOT BECOME ONE.
// ✔MEASURED on gcc 13.3.0 and clang 18.1.3: `%0` is an operand only in an
// EXTENDED template; in a BASIC one `%` is literal and `%eax` is what the
// assembler sees. A template carrying operands is extended by definition, which
// is why this is the right value HERE and the wrong value for a basic template.

namespace {

struct DialectPair {
    std::string_view dialectStem;
    std::string_view target;
    std::string_view templateText;   // writes `%0`, reads `%1`
};

// AT&T is destination-LAST, arm64 gas destination-FIRST. Both spell the
// placeholders identically, because `%N` is GNU INLINE-ASM vocabulary rather
// than dialect vocabulary — which is exactly the property that let one shared
// grammar carry the surface for both.
constexpr DialectPair kX86Pair{"asm-x86_64-att", "x86_64", "movq %1, %0"};
constexpr DialectPair kArmPair{"asm-arm64-gas", "arm64", "mov %0, %1"};

[[nodiscard]] std::shared_ptr<GrammarSchema> loadShippedDialect(
    std::string_view stem) {
    auto pathR = findShippedConfig(
        ShippedConfigLocator{stem, "sources", ".lang.json", "language",
                             DiagnosticCode::C_InvalidTargetName});
    if (!pathR.has_value()) {
        throw std::runtime_error{std::string{"cannot locate dialect "}
                                 + std::string{stem}};
    }
    std::ifstream in{*pathR};
    if (!in) throw std::runtime_error{"cannot open dialect document"};
    std::ostringstream buf;
    buf << in.rdbuf();
    auto g = GrammarSchema::loadFromText(buf.str(), std::string{stem});
    if (!g.has_value()) {
        std::string why;
        for (auto const& e : g.error()) why += e.path + ": " + e.message + "\n";
        throw std::runtime_error{"dialect did not load: " + why};
    }
    return *g;
}

[[nodiscard]] Built buildThroughTheRealEngine(DialectPair const& p,
                                              bool earlyClobber) {
    Built out;
    out.target = shipped(p.target);
    auto const movOp = op(*out.target, "mov");
    auto const retOp = op(*out.target, "ret");
    std::shared_ptr<GrammarSchema> const dialect =
        loadShippedDialect(p.dialectStem);

    DiagnosticReporter rep;
    std::optional<Tree> const tree = parseAsmTemplateText(
        std::string{p.templateText}, "<inline asm>", dialect,
        AsmTemplateSurface::Extended, DiagnosticBudget::libraryDefault(), rep);
    if (!tree.has_value()) {
        std::string why;
        for (auto const& d : rep.all()) { why += d.actual; why += '\n'; }
        throw std::runtime_error{"template did not parse: " + why};
    }

    LirBuilder b{*out.target};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);

    LirReg const vin  = b.newVReg(LirRegClass::GPR);
    LirReg const vout = b.newVReg(LirRegClass::GPR);
    out.inVReg  = vin.id;
    out.outVReg = vout.id;

    std::array<LirOperand, 1> const seed{LirOperand::makeImmInt32(7)};
    (void)b.addInst(movOp, vin, seed);

    // GNU order: outputs first, then inputs — so `%0` is the output.
    std::array<AsmOperandBinding, 2> bindings{};
    bindings[0].spelling  = "%0";
    bindings[0].reg       = vout;
    bindings[0].regClass  = LirRegClass::GPR;
    bindings[0].widthBits = 64;
    bindings[1].spelling  = "%1";
    bindings[1].reg       = vin;
    bindings[1].regClass  = LirRegClass::GPR;
    bindings[1].widthBits = 64;

    std::uint32_t const firstEngineInst = b.lastInst().v + 1u;
    if (!lowerAsmTemplateToLirRun(*tree, *dialect, *out.target, bindings, b,
                                  rep)) {
        std::string why;
        for (auto const& d : rep.all()) { why += d.actual; why += '\n'; }
        throw std::runtime_error{"engine refused the template: " + why};
    }

    if (earlyClobber) {
        std::uint32_t const end = b.lastInst().v + 1u;
        std::uint32_t stamped = 0;
        for (std::uint32_t v = firstEngineInst; v < end; ++v) {
            LirInstId const li{v, b.id().v};
            if (!(b.instResult(li) == vout)) continue;
            b.orInstFlags(li, kLirInstFlagEarlyClobberResult);
            ++stamped;
        }
        if (stamped != 1) {
            throw std::runtime_error{
                "the engine emitted " + std::to_string(stamped) +
                " instructions defining the output vreg; the single-"
                "instruction shape this flag is aimed at needs exactly one"};
        }
    }

    std::array<LirOperand, 1> const ret{LirOperand::makeReg(vout)};
    (void)b.addReturn(retOp, ret);
    out.lir = std::move(b).finish();
    return out;
}

void expectTheSameThroughTheEngine(DialectPair const& p) {
    Built const plain = buildThroughTheRealEngine(p, false);
    Pair  const pa    = allocateAndRead(plain);
    EXPECT_EQ(pa.in, pa.out)
        << p.target << " (engine): a PLAIN `%0` output did NOT share its "
        << "input's register (" << regName(*plain.target, pa.in) << " vs "
        << regName(*plain.target, pa.out)
        << ") — the `&` assertion below is vacuous without this";

    Built const early = buildThroughTheRealEngine(p, true);
    Pair  const eb    = allocateAndRead(early);
    EXPECT_NE(eb.in, eb.out)
        << p.target << " (engine): `\"=&r\"` left the output in the input's "
        << "register (" << regName(*early.target, eb.in) << ")";
}

} // namespace

TEST(LirEarlyClobber, TheRealEngineProducesTheSamePairOnX86_64) {
    expectTheSameThroughTheEngine(kX86Pair);
}

TEST(LirEarlyClobber, TheRealEngineProducesTheSamePairOnArm64) {
    expectTheSameThroughTheEngine(kArmPair);
}

// ── the setter's own contract ────────────────────────────────────────
//
// ★★ OR-IN, NEVER ASSIGN. `flags` also carries the WIDTH selector, which the
// asm engine sets from the operand's register spelling. A `setInstFlags` that
// ASSIGNED would let a caller adding one annotation bit silently clear the
// width and re-run a 32-bit operation at 64 bits — a miscompile with no
// diagnostic, invisible to any test that only looks at the bit it just set. So
// the pin is that the PRE-EXISTING bits survive, not merely that the new one
// lands.
TEST(LirEarlyClobber, OrInstFlagsPreservesTheWidthBitsAlreadyOnTheInstruction) {
    auto const t = shipped("x86_64");
    LirBuilder b{*t};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const v = b.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 1> const ops{LirOperand::makeImmInt32(3)};
    LirInstId const inst =
        b.addInst(op(*t, "mov"), v, ops, /*payload=*/0, kLirInstFlagWidth32);
    b.orInstFlags(inst, kLirInstFlagEarlyClobberResult);
    std::array<LirOperand, 1> const ret{LirOperand::makeReg(v)};
    (void)b.addReturn(op(*t, "ret"), ret);
    Lir const lir = std::move(b).finish();

    std::uint8_t const flags =
        lir.instFlags(lir.blockInstAt(lir.funcBlockAt(lir.funcAt(0), 0), 0));
    EXPECT_TRUE(lirInstResultIsEarlyClobber(flags));
    EXPECT_EQ(lirInstWidthBits(flags), 32)
        << "the width selector already on the instruction was destroyed by an "
           "annotation bit — `orInstFlags` must OR, never assign";
}
