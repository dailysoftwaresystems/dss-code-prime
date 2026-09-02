// ────────────────────────────────────────────────────────────────────────────
// ── D-LIR-CALLEE-SAVED-AND-SPILL-STORES-DEFAULT-TO-WIDTH-64-BELOW-THE-SLOT ──
// ────────────────────────────────────────────────────────────────────────────
//
// THE DEFECT THIS FILE PINS. `emitFrameStore` / `emitFrameLoad` took their
// access width as `std::uint8_t widthFlags = 0`, and `lirInstWidthBits(0)` is
// 64. So every frame move that did not think about width silently moved EIGHT
// BYTES — for every register class, on every target — and there was nothing to
// distinguish the site that had never been asked from the site that had been
// asked and answered 64.
//
// ✔MEASURED at HEAD (2026-08-31) on the shipped `pe64-x86_64-windows-exec`
// target, whose `ms_x64` convention names the SIXTEEN-byte `xmm6..xmm15` in
// `calleeSaved`, `objdump -d` of the real CLI's artifact:
//     14000101d:  f2 44 0f 11 bc 24 20 00 00 00   movsd  %xmm15,0x20(%rsp)
//     14000112e:  f2 44 0f 10 bc 24 20 00 00 00   movsd  0x20(%rsp),%xmm15
// The slot is SIXTEEN bytes — `frameSlotStride` floors at max(widest GPR,
// widest FPR) and this target's `fpr` rows are 16 — so the prologue saved half
// of the register it claimed to preserve. No diagnostic, no refusal, no
// encoder complaint, because width 64 is a perfectly encodable `movsd`. AFTER:
//     14000101d:  44 0f 11 bc 24 20 00 00 00      movups %xmm15,0x20(%rsp)
//
// ★★★ AND THE SAME DEFAULT WAS SIMULTANEOUSLY **RIGHT** ON THE OTHER SHIPPED
// TARGET, WHICH IS WHY THIS FILE IS ABOUT A DECLARATION AND NOT ABOUT A WIDTH.
// AAPCS64 §6.1.2 preserves only the LOW 64 BITS of `v8..v15`; Win64 preserves
// `xmm6..xmm15` in full (✔MEASURED against the reference: MSVC 14.44.35207
// `/O2 /FAs` emits `movaps XMMWORD PTR [rax-24], xmm6` — a sixteen-byte save —
// and Win64's unwind vocabulary spells the save `UWOP_SAVE_XMM128` with no
// code for a partial one). One hardcoded fallback cannot serve two ABIs that
// disagree: it made arm64 accidentally correct and Win64 silently wrong. The
// fix is that each convention DECLARES its own number
// (`calleeSavedPreservedBits`) and silence means the whole register, so the
// width now comes from config on both targets and from a default on neither.
//
// ★ WHAT EACH ARM ASSERTS, AND WHY NONE OF THEM CAN PASS BY ASSERTING A
// CONSTANT:
//   (A) THE WHOLE-REGISTER DERIVATION — `wholeRegisterAccessFlags` answers the
//       class's own declared full width, re-derived here from the public
//       register table rather than by calling the same expression. CONTROL: a
//       class whose full width IS 64 (both targets' `gpr`) must answer 64, so
//       an implementation that returned 128 unconditionally fails.
//   (B) THE ABI DERIVATION IS NOT THE REGISTER DERIVATION — the SAME class
//       (`fpr`, 16-byte registers on both targets) answers 128 under `ms_x64`
//       and 64 under `aapcs64`, and the whole difference is one JSON key. A
//       pin asserting either number as a constant fails on the other target.
//   (C) THE EMITTED ENCODING, ms_x64 — the prologue's saved-`xmm` store is the
//       MOVUPS m128 form, byte-for-byte, and the MOVSD m64 form is ABSENT from
//       the same slot. CONTROL in the same function: the saved-GPR stores stay
//       width-64, so "everything got wider" also fails.
//   (D) THE EMITTED ENCODING, aapcs64 — the saved-`v` store is STUR Dt
//       (0xFC000000) and NOT STUR Qt (0x3C800000). This is the arm that goes
//       red when the arm64 declaration is DELETED, which is what makes (C) a
//       measurement of config rather than of a new constant.
//   (E) THE SPILL PATH — the second site the row names, and the one it calls
//       naked. A spill slot is DSS's own memory, so it round-trips the WHOLE
//       register regardless of ABI; pinned on the emitted encoding.
//
// ⚠ EVERY EMISSION ARM ASSERTS A **POSITIVE COUNT** BEFORE IT ASSERTS A SHAPE,
// and it stays that way now that the row below has closed. An "assert no bad
// instruction appears" pin reads green over an empty instruction list whatever
// produced the emptiness, so each arm first proves the saved register it is
// talking about EXISTS in the layout and that the function assembled to bytes.
// D-LIR-TEST-FRONT-END-LOWERS-A-MANY-ARG-CALL-TO-NOTHING-SO-PINS-MEASURE-ZERO
// (closed P49) removed ONE way to reach that state; the discipline is cheap and
// covers the others.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_2addr_legalize.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_reg.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace dss;

namespace {

// ── THE FIXTURE, AND WHY IT IS HAND-BUILT MIR RATHER THAN c ────────────
//
// ⚠⚠ THE OBVIOUS FIXTURE FOR THIS TIER MEASURES NOTHING, ✔MEASURED RATHER THAN
// ASSUMED. `lowered_lir_fixture.hpp`'s `lowerCToLir` is the natural way to
// write these arms, and the natural source is four doubles each born from a
// call and read after the calls that follow it. Driven through the CLI that
// source spills — `dsscp --compile` on it reports
// `R_SpilledDueToCrossCallExhaustion: func 1 spilled 1 vreg(s)` and the
// `ms_x64` build saves a callee-saved xmm. Driven through THIS fixture the same
// source lowered to SEVEN LIR instructions with `lowerOk` true, `savedRegs`
// EMPTY and `spillAreaSize` 0 — so every arm below would have been a claim about
// saved-register stores in a function that has none, and would have passed. That
// was D-LIR-TEST-FRONT-END-LOWERS-A-MANY-ARG-CALL-TO-NOTHING-SO-PINS-MEASURE-ZERO.
//
// ⚠⚠ THE "AND ZERO DIAGNOSTICS" HALF OF THAT SENTENCE WAS WRONG, AND THE ROW
// INHERITED IT. ✔RE-MEASURED (lane `lt`, P49): the fixture's `mirReporter`
// carried TWO `H_UnsupportedLoweringForKind` errors and `HirToMirResult.ok` was
// FALSE on that very source. `lowerOk`, `allocOk` and `rewriteOk` — the three
// flags this file and the row both read — are the LIR and regalloc verdicts, and
// none of them is the MIR one. The cause was the fixture passing `ffiMap =
// nullptr`, so the `double k(double);` PROTOTYPE was refused and its four calls
// were dropped: not argument pressure, not "shapes the front end cannot build",
// just a prototype. Both are fixed (P49) and the row is CLOSED.
//
// The arms below still do NOT use that fixture, for the reason stated next — a
// hand-built `FAdd` across a `Call` makes preservation an ABI REQUIREMENT rather
// than an allocator preference — which was always the better reason and is now
// the only one.
//
// ★ SO THE MIR IS BUILT DIRECTLY, which is what `tests/asm/test_asm_x86_sse.cpp`
// already does for the sibling claim about this same prologue: an `FAdd` whose
// result is read AFTER a `Call`, so preserving it is an ABI REQUIREMENT rather
// than an allocation preference no future allocator change can dissolve.

struct PipelineOut {
    DiagnosticReporter        rep;
    LirCallconvResult         cc;
    std::vector<std::uint8_t> bytes;
    bool                      ok = false;
};

// `i32 f(f64 a, f64 b)`: `s = a + b`, then a call, then `s` is READ. `s` must
// therefore survive the call — under a convention with callee-saved FP
// registers that is a saved-register store, and under one without it is a
// SPILL, which is exactly the two paths this row names.
[[nodiscard]] Mir buildFpLiveAcrossCall(TypeInterner& interner, CallConv conv) {
    auto const f64 = interner.primitive(TypeKind::F64);
    auto const i32 = interner.primitive(TypeKind::I32);
    TypeId const params[] = {f64, f64};
    auto const sig = interner.fnSig(params, i32, conv);
    MirBuilder mb;
    mb.addFunction(sig, SymbolId{1});
    MirBlockId const bb = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(bb);
    MirInstId const a = mb.addArg(0, f64);
    MirInstId const b = mb.addArg(1, f64);
    MirInstId const addOps[] = {a, b};
    MirInstId const s = mb.addInst(MirOpcode::FAdd, addOps, f64);
    TypeId const ptrT = interner.pointer(interner.primitive(TypeKind::Void));
    MirInstId const callee = mb.addGlobalAddr(SymbolId{2}, ptrT);
    MirInstId const callOps[] = {callee};
    MirInstId const t = mb.addInst(MirOpcode::Call, callOps, i32);
    MirInstId const cvtOps[] = {s};      // `s` is read AFTER the call
    MirInstId const c = mb.addInst(MirOpcode::FPToSI, cvtOps, i32);
    MirInstId const sumOps[] = {c, t};
    MirInstId const r = mb.addInst(MirOpcode::Add, sumOps, i32);
    mb.addReturn(r);
    return std::move(mb).finish();
}

// MIR → LIR → liveness → regalloc → rewrite → 2-addr → callconv → assemble:
// the exact stage order `compile_pipeline.cpp` runs. The WHOLE chain, because
// the subject is the bytes at the end of it — a pin that stopped at the LIR
// would be asserting a flag rather than an access.
void runToBytes(Mir& mir, TypeInterner const& interner,
                TargetSchema const& target, std::uint16_t ccIndex,
                PipelineOut& out) {
    auto lir = lowerToLir(mir, target, interner, out.rep);
    ASSERT_TRUE(lir.ok) << "MIR->LIR failed";
    auto const liveness = analyzeLiveness(lir.lir);
    auto const alloc = allocateRegisters(lir.lir, target, liveness, ccIndex,
                                         out.rep);
    ASSERT_TRUE(alloc.ok()) << "regalloc failed";
    auto rewritten = rewriteWithAllocation(lir.lir, target, alloc, out.rep);
    ASSERT_TRUE(rewritten.ok) << "rewrite failed";
    auto legal = legalizeTwoAddress(rewritten.lir, target, out.rep);
    ASSERT_TRUE(legal.ok()) << "2-addr legalize failed";
    out.cc = materializeCallingConvention(legal.lir, target, alloc, out.rep);
    ASSERT_TRUE(out.cc.ok()) << "callconv failed";
    std::vector<MirInstId> lirToMir(out.cc.lir.instCount(), InvalidMirInst);
    auto assembled = assemble(out.cc.lir, target, lirToMir, out.rep);
    ASSERT_TRUE(assembled.ok()) << "assemble failed";
    ASSERT_EQ(assembled.functions.size(), 1u);
    out.bytes = assembled.functions[0].bytes;
    out.ok = true;
}

// The widest FULL register (a row with no `subOf`) `schema` declares in `cls`,
// in bits; 0 when it declares none. Deliberately RE-DERIVED from the public
// register table rather than routed through `registerClassNaturalWidthBits`,
// so arm (A) does not assert an expression against itself.
[[nodiscard]] std::uint32_t naturalBitsIn(TargetSchema const& schema,
                                          TargetRegClass cls) {
    std::uint32_t bits = 0;
    for (auto const& r : schema.registers()) {
        if (r.regClass != cls || !r.subOf.empty()) continue;
        bits = std::max(bits, static_cast<std::uint32_t>(r.widthBytes) * 8u);
    }
    return bits;
}

// The instruction ids of this function's SAVED-REGISTER STORES, taken from the
// CFI stream rather than guessed from position: a `RegAtCfaOffset` op IS "the
// instruction that put register R in its frame slot", which is exactly the
// population this row is about. Prologue ops only (`prologueOpCount`).
[[nodiscard]] std::vector<std::pair<LirInstId, std::uint16_t>>
savedRegStores(LirFuncCfi const& cfi) {
    std::vector<std::pair<LirInstId, std::uint16_t>> out;
    std::uint32_t const n =
        std::min<std::uint32_t>(cfi.prologueOpCount,
                                static_cast<std::uint32_t>(cfi.ops.size()));
    for (std::uint32_t i = 0; i < n; ++i) {
        if (cfi.ops[i].kind != CfiOpKind::RegAtCfaOffset) continue;
        out.emplace_back(cfi.ops[i].inst, cfi.ops[i].reg.ordinal);
    }
    return out;
}

[[nodiscard]] bool containsBytes(std::vector<std::uint8_t> const& hay,
                                 std::vector<std::uint8_t> const& needle) {
    if (needle.empty() || needle.size() > hay.size()) return false;
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end())
           != hay.end();
}

[[nodiscard]] std::string hexOf(std::vector<std::uint8_t> const& v) {
    std::string s;
    for (auto const b : v) s += std::format("{:02x} ", static_cast<unsigned>(b));
    return s;
}

// ── (A) THE WHOLE-REGISTER DERIVATION, WITH ITS WIDTH-64 CONTROL ────────────
TEST(LirFrameAccessWidth, WholeRegisterWidthIsTheClassesOwnDeclaredWidth) {
    for (char const* targetName : {"x86_64", "arm64"}) {
        auto target = TargetSchema::loadShipped(targetName);
        ASSERT_TRUE(target.has_value()) << targetName;
        TargetSchema const& s = **target;

        struct Row { LirRegClass cls; TargetRegClass tcls; char const* name; };
        for (auto const& row : {Row{LirRegClass::GPR, TargetRegClass::GPR, "gpr"},
                                Row{LirRegClass::FPR, TargetRegClass::FPR, "fpr"}}) {
            std::uint32_t const expectBits = naturalBitsIn(s, row.tcls);
            ASSERT_GT(expectBits, 0u)
                << targetName << " must declare full registers in '" << row.name
                << "' — with none, this arm would be asserting over an empty set";
            DiagnosticReporter rep;
            auto const flags = wholeRegisterAccessFlags(s, row.cls,
                                                        "test", rep);
            ASSERT_TRUE(flags.has_value())
                << targetName << '/' << row.name
                << ": the whole-register width must resolve for a class the "
                   "target declares registers in";
            EXPECT_EQ(rep.errorCount(), 0u);
            EXPECT_EQ(static_cast<std::uint32_t>(lirInstWidthBits(*flags)),
                      expectBits)
                << targetName << '/' << row.name
                << ": a frame move carrying a whole register must access "
                   "exactly the width that class's full registers declare";
        }

        // ★ THE CONTROL, AND IT IS WHAT STOPS THIS ARM PASSING ON A CONSTANT.
        // Both shipped targets declare a 64-bit `gpr` and a 128-bit `fpr`, so
        // an implementation answering either number unconditionally fails one
        // of the two. Stated as a DISCRIMINATION rather than as two more
        // equalities: the claim is that the answer TRACKS the class.
        DiagnosticReporter rep;
        auto const gpr = wholeRegisterAccessFlags(s, LirRegClass::GPR, "t", rep);
        auto const fpr = wholeRegisterAccessFlags(s, LirRegClass::FPR, "t", rep);
        ASSERT_TRUE(gpr.has_value() && fpr.has_value());
        EXPECT_EQ(lirInstWidthBits(*gpr), 64)
            << targetName << ": the gpr control class is 64 bits wide";
        EXPECT_GT(lirInstWidthBits(*fpr), lirInstWidthBits(*gpr))
            << targetName << ": `fpr` registers are wider than `gpr` ones on "
               "both shipped targets — if these came out equal the arms below "
               "would be pinning nothing";
    }
}

// ── (B) THE ABI DERIVATION IS A DIFFERENT QUESTION FROM THE REGISTER ONE ────
TEST(LirFrameAccessWidth, CalleeSavedWidthComesFromTheConventionNotTheRegister) {
    auto x86 = TargetSchema::loadShipped("x86_64");
    auto arm = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(x86.has_value());
    ASSERT_TRUE(arm.has_value());

    auto const* msX64   = (*x86)->callingConventionByName("ms_x64");
    auto const* aapcs64 = (*arm)->callingConventionByName("aapcs64");
    ASSERT_NE(msX64, nullptr);
    ASSERT_NE(aapcs64, nullptr);

    // The premise both halves rest on: BOTH conventions name FPR-class
    // registers in `calleeSaved`, and on BOTH targets those registers are 128
    // bits wide. Without this the split below would be about two different
    // register files rather than about two ABIs.
    EXPECT_EQ(naturalBitsIn(**x86, TargetRegClass::FPR), 128u);
    EXPECT_EQ(naturalBitsIn(**arm, TargetRegClass::FPR), 128u);

    DiagnosticReporter rep;
    auto const msFpr =
        calleeSavedAccessFlags(**x86, *msX64, LirRegClass::FPR, "t", rep);
    auto const armFpr =
        calleeSavedAccessFlags(**arm, *aapcs64, LirRegClass::FPR, "t", rep);
    ASSERT_TRUE(msFpr.has_value());
    ASSERT_TRUE(armFpr.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);

    // Win64 declares NOTHING, so the answer is the whole register — the
    // fail-safe direction, and the one that fixes the measured defect.
    EXPECT_EQ(lirInstWidthBits(*msFpr), 128)
        << "`ms_x64` declares no `calleeSavedPreservedBits` for `fpr`, and an "
           "ABI that preserves everything has nothing to declare — so the save "
           "must cover the whole 16-byte xmm. MSVC agrees (movaps XMMWORD PTR).";
    // AAPCS64 DOES declare, and the declaration is what makes the narrower
    // save a decision rather than a leftover default.
    EXPECT_EQ(lirInstWidthBits(*armFpr), 64)
        << "`aapcs64` declares `calleeSavedPreservedBits` fpr=64 (AAPCS64 "
           "§6.1.2 preserves only the low 64 bits of v8-v15) — deleting that "
           "key must change this answer, which is what makes it a measurement "
           "of config rather than of a constant";
    EXPECT_NE(lirInstWidthBits(*msFpr), lirInstWidthBits(*armFpr))
        << "the SAME register class, the same 16-byte registers, two different "
           "answers — the whole content of this row is that the width is the "
           "ABI's number and not the register's";

    // CONTROL: a class whose full width IS 64. Neither convention declares a
    // `gpr` row, so both fall through to the whole register and agree — an
    // implementation that read the FPR declaration for every class would
    // narrow this too.
    auto const msGpr =
        calleeSavedAccessFlags(**x86, *msX64, LirRegClass::GPR, "t", rep);
    auto const armGpr =
        calleeSavedAccessFlags(**arm, *aapcs64, LirRegClass::GPR, "t", rep);
    ASSERT_TRUE(msGpr.has_value() && armGpr.has_value());
    EXPECT_EQ(lirInstWidthBits(*msGpr), 64);
    EXPECT_EQ(lirInstWidthBits(*armGpr), 64);
}

// ── (C) THE EMITTED ENCODING UNDER ms_x64, WITH ITS GPR CONTROL ─────────────
// ── (C) THE EMITTED ENCODING UNDER ms_x64, WITH ITS GPR CONTROL ─────────────
TEST(LirFrameAccessWidth, MsX64PrologueSavesTheWholeXmmIntoItsWholeSlot) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& s = **target;
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildFpLiveAcrossCall(interner, CallConv::CcMS64);
    PipelineOut out;
    runToBytes(mir, interner, s, /*ccIndex=*/1, out);
    ASSERT_TRUE(out.ok);
    EXPECT_EQ(out.rep.errorCount(), 0u)
        << "a ms_x64 function that saves an xmm must assemble clean — a target "
           "with no width-128 form for the `fpr` store fails HERE, by name "
           "(A_NoMatchingEncodingVariant), which is the REMOVE-direction red";
    ASSERT_GT(out.bytes.size(), 0u)
        << "the function must assemble to actual bytes";
    ASSERT_EQ(out.cc.perFunc.size(), 1u);
    ASSERT_EQ(out.cc.perFuncCfi.size(), 1u);
    FrameLayout const& layout = out.cc.perFunc[0];

    auto const stores = savedRegStores(out.cc.perFuncCfi[0]);
    // ⚠ THE POSITIVE COUNT COMES FIRST. Everything below is a claim ABOUT
    // saved-register stores, and this arm's first draft — driven through
    // `lowerCToLir` — produced NONE of them while passing; see the fixture
    // note at the top of this file.
    ASSERT_FALSE(stores.empty())
        << "the prologue must save at least one register — with none, every "
           "assertion below is vacuously true. savedRegs="
        << layout.savedRegs.size()
        << " spillAreaSize=" << layout.spillAreaSize;

    std::size_t fprSaves = 0;
    std::size_t gprSaves = 0;
    for (auto const& [inst, ord] : stores) {
        auto const* info = s.registerInfo(ord);
        ASSERT_NE(info, nullptr);
        std::uint32_t const declared = naturalBitsIn(s, info->regClass);
        std::uint32_t const emitted = static_cast<std::uint32_t>(
            lirInstWidthBits(out.cc.lir.instFlags(inst)));
        // ★ THE GENERAL LAW, stated over EVERY saved register rather than over
        // the interesting one: a callee-save writes exactly what `ms_x64`
        // preserves, and `ms_x64` declares nothing, so that is the whole
        // register. The GPR saves in this same function are the control — they
        // must come out 64, so "widen everything" fails here.
        EXPECT_EQ(emitted, declared)
            << "saved register '" << info->name << "' is " << declared
            << " bits wide and `ms_x64` preserves all of it, but its prologue "
               "store accesses " << emitted << " bits";
        if (info->regClass == TargetRegClass::FPR) {
            ++fprSaves;
            EXPECT_GT(emitted, 64u)
                << "the whole point of this arm is a class WIDER than the old "
                   "64-bit default";
            // The row's own closing ask: the stored byte count equals the slot
            // the frame reserved, so nothing in the slot is left unwritten.
            EXPECT_EQ(emitted / 8u, layout.slotSize)
                << "the saved-xmm store must fill its whole frame slot";

            // ★★ THE CLOSING ASSERTION IS ON THE ENCODING. Built from the
            // register's own `hwEncoding`, so it is DERIVED rather than copied
            // from a disassembly:
            //   [REX.R if hw>=8] 0F 11 /r  modrm(mod=10, reg=hw&7, rm=100=SIB)
            //   SIB(base=rsp)             — MOVUPS m128, xmm   (✔gas 2.42)
            // The MOVSD form is the same tail behind a 0xF2 mandatory prefix,
            // which is precisely the shape measured at HEAD. The disp32 is
            // deliberately NOT part of the needle: the pattern ends at the SIB
            // byte, so the pin survives a frame-layout change while still
            // discriminating the exact instruction and register.
            std::uint32_t const hw = info->hwEncoding;
            std::vector<std::uint8_t> tail{
                0x0F, 0x11,
                static_cast<std::uint8_t>(0x80u | ((hw & 7u) << 3) | 0x04u),
                0x24};
            std::vector<std::uint8_t> movups;
            if (hw >= 8) movups.push_back(0x44);   // REX.R for xmm8..xmm15
            movups.insert(movups.end(), tail.begin(), tail.end());
            std::vector<std::uint8_t> movsd{0xF2};
            if (hw >= 8) movsd.push_back(0x44);
            movsd.insert(movsd.end(), tail.begin(), tail.end());

            EXPECT_TRUE(containsBytes(out.bytes, movups))
                << "the saved-'" << info->name << "' store must be the MOVUPS "
                   "m128 form (" << hexOf(movups) << "…) — the sixteen-byte "
                   "access. Function bytes: " << hexOf(out.bytes);
            EXPECT_FALSE(containsBytes(out.bytes, movsd))
                << "the MOVSD m64 form of the SAME slot (" << hexOf(movsd)
                << "…) must be gone — that is the eight-byte save measured at "
                   "HEAD, and its absence is what distinguishes this fix from "
                   "an added-but-unused encoding";
        } else if (info->regClass == TargetRegClass::GPR) {
            ++gprSaves;
            EXPECT_EQ(emitted, 64u)
                << "the width-64 CONTROL: a GPR callee-save must NOT widen. It "
                   "owns 8 of its 16-byte slot and writing more would be this "
                   "defect with the sign flipped";
        }
    }
    EXPECT_GT(fprSaves, 0u)
        << "`ms_x64` names xmm6..xmm15 in `calleeSaved` and this fixture keeps "
           "an FAdd result live across a Call — if the allocator stopped "
           "reaching a callee-saved xmm, this arm measures nothing and must be "
           "re-aimed rather than left passing";
}

// ── (D) THE EMITTED ENCODING UNDER aapcs64 — THE ANTI-CONSTANT ARM ──────────
TEST(LirFrameAccessWidth, Aapcs64PrologueSavesExactlyTheDeclaredSixtyFourBits) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& s = **target;
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildFpLiveAcrossCall(interner, CallConv::CcAAPCS64);
    PipelineOut out;
    runToBytes(mir, interner, s, /*ccIndex=*/0, out);
    ASSERT_TRUE(out.ok);
    EXPECT_EQ(out.rep.errorCount(), 0u);
    ASSERT_GE(out.bytes.size(), 4u);
    ASSERT_EQ(out.cc.perFuncCfi.size(), 1u);

    auto const stores = savedRegStores(out.cc.perFuncCfi[0]);
    ASSERT_FALSE(stores.empty())
        << "savedRegs=" << out.cc.perFunc[0].savedRegs.size();

    std::size_t fprSaves = 0;
    for (auto const& [inst, ord] : stores) {
        auto const* info = s.registerInfo(ord);
        ASSERT_NE(info, nullptr);
        if (info->regClass != TargetRegClass::FPR) continue;
        ++fprSaves;
        EXPECT_EQ(lirInstWidthBits(out.cc.lir.instFlags(inst)), 64)
            << "AAPCS64 declares `calleeSavedPreservedBits` fpr=64 (§6.1.2 "
               "preserves only the low 64 bits of v8-v15), so the save of '"
            << info->name << "' is a 64-bit STUR Dt — DELETING that key makes "
               "it 128 and reddens this line, which is what proves the ms_x64 "
               "arm reads config rather than a constant";
    }
    ASSERT_GT(fprSaves, 0u)
        << "aapcs64 names v8..v15 in `calleeSaved` and this fixture keeps an "
           "FAdd result live across a Call — no FP callee-save means no "
           "measurement";

    // The byte tier, offset-independent: some word must be `STUR Dt, [sp,#imm]`
    // (fixed word 0xFC000000, Rn=31) and NO word may be its Q-form sibling
    // (0x3C800000), which is what a save widened past the ABI would emit.
    auto wordsOf = [](std::vector<std::uint8_t> const& v) {
        std::vector<std::uint32_t> w;
        for (std::size_t k = 0; k + 3 < v.size(); k += 4) {
            w.push_back(static_cast<std::uint32_t>(v[k])
                      | (static_cast<std::uint32_t>(v[k + 1]) << 8)
                      | (static_cast<std::uint32_t>(v[k + 2]) << 16)
                      | (static_cast<std::uint32_t>(v[k + 3]) << 24));
        }
        return w;
    };
    // Keep the opcode bits and Rn (bits 9:5 == 31 = sp); clear imm9 and Rt.
    constexpr std::uint32_t kMask    = 0xFFE003E0u;
    constexpr std::uint32_t kSturDSp = 0xFC000000u | (31u << 5);
    constexpr std::uint32_t kSturQSp = 0x3C800000u | (31u << 5);
    auto const words = wordsOf(out.bytes);
    ASSERT_FALSE(words.empty());
    bool sawD = false, sawQ = false;
    for (auto const w : words) {
        if ((w & kMask) == kSturDSp) sawD = true;
        if ((w & kMask) == kSturQSp) sawQ = true;
    }
    EXPECT_TRUE(sawD)
        << "the aapcs64 prologue must still STUR a D register through sp — "
           "byte-identical to before this row, because the number did not "
           "change, only where it comes from";
    EXPECT_FALSE(sawQ)
        << "a Q-form (128-bit) save through sp would preserve MORE than "
           "AAPCS64 §6.1.2 asks and would mean the declaration was ignored";
}

// ── (E) THE SPILL PATH — DSS'S OWN MEMORY, SO THE WHOLE REGISTER ────────────
TEST(LirFrameAccessWidth, FprSpillRoundTripsTheWholeRegisterNotHalfOfIt) {
    // SysV x86_64 declares NO callee-saved xmm, so an FP value live across a
    // call has nowhere to be preserved but a SPILL SLOT — which isolates the
    // spill path from the saved-register path arm (C) covers.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& s = **target;

    auto const* sysv = s.callingConventionByName("sysv_amd64");
    ASSERT_NE(sysv, nullptr);
    // The precondition that makes this the SPILL path and not arm (C) again.
    for (auto const& name : sysv->calleeSaved) {
        auto const ord = s.registerByName(name);
        ASSERT_TRUE(ord.has_value());
        auto const* info = s.registerInfo(*ord);
        ASSERT_NE(info, nullptr);
        ASSERT_NE(info->regClass, TargetRegClass::FPR)
            << "sysv_amd64 must declare no callee-saved FPR — if it gained one, "
               "this arm stops isolating the spill path";
    }
    std::uint32_t const full = naturalBitsIn(s, TargetRegClass::FPR);
    ASSERT_GT(full, 64u)
        << "this arm needs an FPR class WIDER than the old default to say "
           "anything";

    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildFpLiveAcrossCall(interner, CallConv::CcSysV);
    PipelineOut out;
    runToBytes(mir, interner, s, /*ccIndex=*/0, out);
    ASSERT_TRUE(out.ok);
    EXPECT_EQ(out.rep.errorCount(), 0u);
    ASSERT_EQ(out.cc.perFunc.size(), 1u);
    FrameLayout const& layout = out.cc.perFunc[0];
    ASSERT_GT(layout.spillAreaSize, 0u)
        << "the fixture must actually SPILL — with a zero-size spill area this "
           "arm is a claim about instructions the function does not contain";

    auto const storeOp = s.regClassOpOpcode(TargetRegClass::FPR,
                                            RegClassOp::Store);
    auto const loadOp = s.regClassOpOpcode(TargetRegClass::FPR,
                                           RegClassOp::Load);
    ASSERT_TRUE(storeOp.has_value());
    ASSERT_TRUE(loadOp.has_value());
    ASSERT_TRUE(sysv->stackPointer.has_value());
    std::uint16_t const spOrd = sysv->stackPointer->ordinal;

    std::size_t wideStores = 0, wideLoads = 0, narrowFrameMoves = 0;
    Lir const& lir = out.cc.lir;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
            LirBlockId const blk = lir.funcBlockAt(fn, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(blk); ++ii) {
                LirInstId const inst = lir.blockInstAt(blk, ii);
                std::uint16_t const op = lir.instOpcode(inst);
                if (op != *storeOp && op != *loadOp) continue;
                // A FRAME move is the one whose memory base is SP. A type-exact
                // value store through a computed address is NOT this row's
                // subject and must keep its own narrow width — which is why
                // this walk discriminates on the base register instead of
                // counting every FP memory op in the function.
                bool throughSp = false;
                for (auto const& o : lir.instOperands(inst)) {
                    if (o.kind == LirOperandKind::Reg && o.reg.isPhysical != 0
                        && o.reg.id == spOrd) {
                        throughSp = true;
                    }
                }
                if (!throughSp) continue;
                auto const w = static_cast<std::uint32_t>(
                    lirInstWidthBits(lir.instFlags(inst)));
                if (w == full) { if (op == *storeOp) ++wideStores; else ++wideLoads; }
                else           { ++narrowFrameMoves; }
            }
        }
    }
    EXPECT_GT(wideStores, 0u)
        << "an FP value live across a call under sysv_amd64 must be SPILLED, "
           "and a spill slot is DSS's own memory — so the store covers the "
           "whole " << full << "-bit register";
    EXPECT_GT(wideLoads, 0u)
        << "every spill store has a reload, and a reload NARROWER than its "
           "store is the same silent wrong answer from the other direction";
    EXPECT_EQ(narrowFrameMoves, 0u)
        << "no SP-relative whole-register FP frame move may be narrower than "
           "the register: that is exactly the 64-bit default this row removed";
    EXPECT_EQ(full / 8u, layout.slotSize)
        << "the whole-register access must fill the slot `frameSlotStride` "
           "reserved for it — the two derivations read the same register table "
           "and must agree";
}

} // namespace
