// THE `.s` CALL-FRAME PRODUCER — D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED.
//
// ★★★ WHAT WAS WRONG. All eighteen `.cfi_*` spellings the AT&T dialect declared
// were `ignoredAnnotation`: parsed, validated, and DROPPED. A `.s` assembled,
// ran correctly, and could not be unwound — no debugger backtrace, and an
// exception thrown through the frame terminating instead of propagating. The
// arm64 dialect declared NONE, so `.cfi_startproc` failed loud there instead;
// the two ports sat on opposite sides of one row, which is why every test here
// runs over BOTH shipped dialects UNMUTATED.
//
// ★★★ WHAT THIS FILE PINS THAT THE CORPUS EXAMPLES CANNOT. The examples
// `asm_x86_64_cfi_unwind_annotations` / `asm_arm64_cfi_unwind_annotations` are
// the end-to-end witnesses (✔MEASURED 2026-08-17: gdb 15.1 walks a 3-frame
// stack out of the x86_64 one, and the arm64 one's `.eh_frame` is rule-for-rule
// and delta-for-delta identical to `aarch64-linux-gnu-as` 2.42's own output
// from the same source). But an example can only say "the exit code was 42",
// and a `.s` that DESCRIBED saves it never performed would exit 42 too. So the
// claims that need a byte or a field are here, on every host, including the
// ones that can execute neither binary.
//
// ★★ AND ONE CHOICE OF WITNESS IS DELIBERATE, BECAUSE THIS PROJECT HAS BEEN
// BITTEN BY THE OPPOSITE. `D-ASM-ARM64-CONDITION-AS-OPERAND-UNMODELLED` was
// once closed on `eq`/`ne` — the two spellings where the substrate and gas
// vocabularies HAPPEN TO COINCIDE — and the close was false for the other ten.
// The same trap exists here: on x86_64 the DWARF return-address column is 16, a
// SYNTHETIC column no register carries, while on aarch64 it is 30, which IS
// x30's ordinary register number. A producer that resolved a number against the
// register table first and the RA column second would look correct on x86_64 and
// be wrong on aarch64 — and vice versa. `TheReturnAddressColumnResolvesAsTheRa
// ColumnOnBothPorts` therefore pins the case where the two DISAGREE.

#include "asm_text_fixture.hpp"
#include "asm/asm.hpp"
#include "asm/asm_cfi.hpp"
#include "core/types/cfi.hpp"
#include "link/format/dwarf_cfi.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::test_support::asm_text;

namespace {

// ══ THE TWO SHIPPED DIALECTS ═══════════════════════════════════════════════
//
// Everything below runs over both. A mechanism witnessed by one dialect is the
// weaker claim, and this row's own history is the reason: the defect it closes
// existed on x86_64 and did NOT exist on arm64 (where the spellings were simply
// absent), so a one-port test would have covered half the fix.
struct Port {
    std::string_view language;
    std::string_view target;
    std::string_view marker;      // the `.type` marker this dialect writes
    std::string_view spReg;       // the stack-pointer SPELLING
    std::uint64_t    raColumn;    // the target's declared DWARF RA column
    std::int64_t     entryCfa;    // callPushBytes: 8 on x86_64, 0 on aarch64
    std::string_view savedRegNum; // a DWARF number that IS an ordinary register
    std::string_view savedRegName;// the same register, spelled
};

constexpr Port kX86{"asm-x86_64-att", "x86_64", "@function", "%rsp", 16, 8,
                    "6", "%rbp"};
constexpr Port kArm{"asm-arm64-gas", "arm64", "%function", "sp", 30, 0,
                    "29", "x29"};

// ★★★ A TWO-PORT LOOP WITH AN `ASSERT` IN IT ONLY RUNS THE FIRST PORT, AND
// THAT IS NOT A STYLE POINT — IT IS THE VACUITY THIS FILE EXISTS TO AVOID.
// ✔MEASURED 2026-08-17 while red-on-disabling this very file: the mutant that
// looks the return-address column up AFTER the register table makes the x86_64
// half fail LOUD (column 16 belongs to no register, so the lookup finds
// nothing) — and a plain `for (p : {kX86, kArm})` then ABORTED THE WHOLE TEST
// at the first `ASSERT`, so the aarch64 half, where the same mutant resolves
// SILENTLY to physical x30, was never reached. The safe direction masked the
// dangerous one. Running each port through a `void` callable makes an `ASSERT`
// return from the BODY rather than from the test, so both ports always run.
template <typename Fn>
void forBothPorts(Fn&& body) {
    body(kX86);
    body(kArm);
}

// A one-instruction exported function, in THIS dialect's spelling, wrapped in
// whatever frame text the caller supplies.
[[nodiscard]] std::string fn(Port const& p, std::string_view name,
                             std::string_view body) {
    return std::string{"\t.globl "} + std::string{name} + "\n\t.type "
         + std::string{name} + ", " + std::string{p.marker} + "\n"
         + std::string{name} + ":\n" + std::string{body} + "\tret\n";
}

[[nodiscard]] std::unique_ptr<LoweringRun>
lowerPort(Port const& p, std::string const& source) {
    return lowerAsmText(shippedDialectDoc(p.language), source, p.target);
}

// The op list of function `fi`, after asserting the file lowered cleanly and
// that the slot is ENGAGED (described). ⚠ `ASSERT_*` on engagement BEFORE
// dereferencing: `.value()` on a dropped optional aborts the process instead of
// naming the site, which is not a test failure, it is a missing verdict.
[[nodiscard]] std::vector<LirCfiOp> const&
opsOf(LoweringRun const& run, std::size_t fi) {
    static std::vector<LirCfiOp> const kEmpty;
    if (!run.module.has_value()) return kEmpty;
    if (fi >= run.module->perFuncCfi.size()) return kEmpty;
    if (!run.module->perFuncCfi[fi].has_value()) return kEmpty;
    return run.module->perFuncCfi[fi]->ops;
}

// Did the lowering refuse, and does the message name `needle`?
void expectRefusedNaming(Port const& p, std::string const& source,
                         std::string_view needle) {
    auto const run = lowerPort(p, source);
    ASSERT_TRUE(parsedCleanly(*run))
        << p.language << ": the input did not PARSE, so this test cannot say "
        << "anything about the LOWERING's verdict: " << parseMessages(*run);
    EXPECT_FALSE(run->module.has_value())
        << p.language << ": expected a refusal, got a module";
    EXPECT_NE(messages(*run).find(needle), std::string::npos)
        << p.language << ": the refusal did not name '" << needle
        << "'. Actual: " << messages(*run);
}

// ══ ASSEMBLE + ATTACH: the real chain, so byte offsets are MEASURED ════════
//
// ★ THE PIN DRIVES THE SHIPPED PATH RATHER THAN RE-TYPING ITS DATA. A pin that
// hand-built a `CfiFunction` would be testing its own arithmetic; this one runs
// `lowerAsmTextToLir` → `assemble()` → `attachAssemblyCfi`, which is exactly
// what `assembleAsmUnit` runs, so the byte offsets are the encoder's own.
struct Attached {
    AssembledModule    module;
    DiagnosticReporter reporter;
    bool               ok = false;
};

[[nodiscard]] Attached attachOf(LoweringRun& run) {
    Attached out;
    if (!run.module.has_value()) return out;
    std::vector<MirInstId> lirToMir(run.module->lir.instCount(),
                                    InvalidMirInst);
    out.module = assemble(run.module->lir, *run.target, lirToMir,
                          out.reporter);
    if (!out.module.ok()) return out;
    if (!run.module->cfiInitial.has_value()) return out;
    out.ok = attachAssemblyCfi(out.module, run.module->perFuncCfi,
                               *run.module->cfiInitial, out.reporter);
    return out;
}

// ══════════════════════════════════════════════════════════════════════════
// 1. THE HONOURED SURFACE — every rule spelling reaches its `CfiOpKind`
// ══════════════════════════════════════════════════════════════════════════

TEST(AsmCfiProducer, EveryHonouredRuleSpellingReachesItsCfiOpKind) {
    forBothPorts([](Port const& p) {
        // Written in ONE string for both ports: the `.cfi_*` surface is the one
        // place the two dialects do NOT differ, and asserting that is part of
        // the claim. Only the `.type` marker and the register spellings vary.
        std::string const body =
            "\t.cfi_startproc\n"
            "\t.cfi_return_column " + std::to_string(p.raColumn) + "\n"
            "\t.cfi_def_cfa " + std::string{p.savedRegNum} + ", 16\n"
            "\t.cfi_def_cfa_register " + std::string{p.savedRegNum} + "\n"
            "\t.cfi_def_cfa_offset 32\n"
            "\t.cfi_adjust_cfa_offset 8\n"
            "\t.cfi_offset " + std::string{p.savedRegNum} + ", -16\n"
            "\t.cfi_rel_offset " + std::string{p.savedRegNum} + ", 8\n"
            "\t.cfi_val_offset " + std::string{p.savedRegNum} + ", -24\n"
            "\t.cfi_register " + std::string{p.savedRegNum} + ", "
                              + std::string{p.savedRegNum} + "\n"
            "\t.cfi_same_value " + std::string{p.savedRegNum} + "\n"
            "\t.cfi_undefined " + std::string{p.savedRegNum} + "\n"
            "\t.cfi_remember_state\n"
            "\t.cfi_restore_state\n"
            "\t.cfi_restore " + std::string{p.savedRegNum} + "\n";
        auto const run = lowerPort(p, fn(p, "main", body) + "\t.cfi_endproc\n");
        ASSERT_TRUE(parsedCleanly(*run))
            << p.language << " parse: " << parseMessages(*run);
        ASSERT_TRUE(run->module.has_value())
            << p.language << " refused: " << messages(*run);
        ASSERT_EQ(run->module->perFuncCfi.size(), 1u) << p.language;
        ASSERT_TRUE(run->module->perFuncCfi[0].has_value())
            << p.language << ": the frame-start directive did not ENGAGE the "
            << "function's slot, so nothing downstream would emit an FDE";

        // ★ THE EXPECTED SEQUENCE IS SPELLED OUT, IN ORDER, KIND BY KIND. A
        // count assertion would be satisfied by fifteen of the wrong rule; a
        // set assertion would be satisfied by the right rules in an order DWARF
        // cannot encode (`advance_loc` is a forward-only delta).
        // ⚠ `.cfi_return_column` produces NO op — it is a CIE-level check, not
        // a rule — and `.cfi_adjust_cfa_offset` produces `def_cfa_offset`,
        // FOLDED, because DWARF has no adjust opcode.
        std::vector<CfiOpKind> const want{
            CfiOpKind::DefCfa,            // .cfi_def_cfa
            CfiOpKind::DefCfaRegister,    // .cfi_def_cfa_register
            CfiOpKind::DefCfaOffset,      // .cfi_def_cfa_offset
            CfiOpKind::DefCfaOffset,      // .cfi_adjust_cfa_offset -> FOLDED
            CfiOpKind::RegAtCfaOffset,    // .cfi_offset
            CfiOpKind::RegAtCfaOffset,    // .cfi_rel_offset -> same rule
            CfiOpKind::RegValIsCfaOffset, // .cfi_val_offset
            CfiOpKind::RegInRegister,     // .cfi_register
            CfiOpKind::RegSameValue,      // .cfi_same_value
            CfiOpKind::RegUndefined,      // .cfi_undefined
            CfiOpKind::RememberState,     // .cfi_remember_state
            CfiOpKind::RestoreState,      // .cfi_restore_state
            CfiOpKind::RegRestoreInitial, // .cfi_restore
        };
        auto const& ops = opsOf(*run, 0);
        ASSERT_EQ(ops.size(), want.size())
            << p.language << ": the honoured rule count changed";
        for (std::size_t i = 0; i < want.size(); ++i) {
            EXPECT_EQ(ops[i].kind, want[i])
                << p.language << ": op #" << i << " is '"
                << cfiOpKindName(ops[i].kind) << "', wanted '"
                << cfiOpKindName(want[i]) << "'";
        }
    });
}

// ══════════════════════════════════════════════════════════════════════════
// 2. THE TWO FOLDS — the arithmetic, pinned to GNU as's MEASURED answer
// ══════════════════════════════════════════════════════════════════════════

// ★★★ ✔MEASURED 2026-08-17, GNU as 2.42 + `readelf --debug-dump=frames`: with
// the CFA offset at 48, `.cfi_rel_offset 6, 8` emits `DW_CFA_offset: r6 (rbp)
// at cfa-40`. 8 − 48 = −40. This asserts that exact number, not "some negative
// offset": a fold that subtracted the ENTRY offset instead of the RUNNING one
// would give 8 − 8 = 0, which is also plausible and also wrong.
TEST(AsmCfiProducer, RelOffsetIsFoldedAgainstTheRUNNINGCfaOffset) {
    forBothPorts([](Port const& p) {
        std::string const body =
            "\t.cfi_startproc\n"
            "\t.cfi_def_cfa_offset 48\n"
            "\t.cfi_rel_offset " + std::string{p.savedRegNum} + ", 8\n";
        auto const run = lowerPort(p, fn(p, "main", body) + "\t.cfi_endproc\n");
        ASSERT_TRUE(run->module.has_value())
            << p.language << ": " << messages(*run);
        auto const& ops = opsOf(*run, 0);
        ASSERT_EQ(ops.size(), 2u) << p.language;
        EXPECT_EQ(ops[1].kind, CfiOpKind::RegAtCfaOffset) << p.language;
        EXPECT_EQ(ops[1].offset, -40)
            << p.language << ": `.cfi_rel_offset " << p.savedRegNum
            << ", 8` at CFA offset 48 must fold to 8 - 48 = -40, which is what "
            << "GNU as 2.42 emits (DW_CFA_offset: at cfa-40)";
    });
}

// ★★★ ✔MEASURED 2026-08-17, GNU as 2.42: `.cfi_adjust_cfa_offset 16` with the
// CFA offset at 48 emits an ABSOLUTE `DW_CFA_def_cfa_offset: 64`, and
// `.cfi_adjust_cfa_offset 0` at the initial state emits the CURRENT value
// rather than 0. Both halves matter: the second is what catches a fold that
// forgot to seed itself from the entry state.
TEST(AsmCfiProducer, AdjustCfaOffsetFoldsToAnAbsoluteDefCfaOffset) {
    forBothPorts([](Port const& p) {
        auto const run = lowerPort(
            p, fn(p, "main",
                  "\t.cfi_startproc\n"
                  "\t.cfi_def_cfa_offset 48\n"
                  "\t.cfi_adjust_cfa_offset 16\n"
                  "\t.cfi_adjust_cfa_offset 0\n")
                   + "\t.cfi_endproc\n");
        ASSERT_TRUE(run->module.has_value())
            << p.language << ": " << messages(*run);
        auto const& ops = opsOf(*run, 0);
        ASSERT_EQ(ops.size(), 3u) << p.language;
        // ★ The KIND changed, and that is required rather than cosmetic:
        // `dwarf_cfi.hpp` REFUSES an `AdjustCfaOffset` that reaches it,
        // naming the producer as the tier that must fold.
        EXPECT_EQ(ops[1].kind, CfiOpKind::DefCfaOffset) << p.language;
        EXPECT_EQ(ops[1].offset, 64) << p.language << ": 48 + 16";
        EXPECT_EQ(ops[2].kind, CfiOpKind::DefCfaOffset) << p.language;
        EXPECT_EQ(ops[2].offset, 64)
            << p.language << ": a +0 adjust restates the RUNNING value";
    });
}

// The remember/restore stack must restore the running CFA offset, because the
// NEXT fold reads it. gcc brackets every epilogue with the pair (✔MEASURED
// 2026-08-17 on both ports), so this is the ordinary shape, not a corner.
TEST(AsmCfiProducer, RestoreStateRestoresTheRunningCfaOffsetForTheNextFold) {
    forBothPorts([](Port const& p) {
        auto const run = lowerPort(
            p, fn(p, "main",
                  "\t.cfi_startproc\n"
                  "\t.cfi_def_cfa_offset 48\n"
                  "\t.cfi_remember_state\n"
                  "\t.cfi_def_cfa_offset 96\n"
                  "\t.cfi_restore_state\n"
                  "\t.cfi_rel_offset " + std::string{p.savedRegNum} + ", 8\n")
                   + "\t.cfi_endproc\n");
        ASSERT_TRUE(run->module.has_value())
            << p.language << ": " << messages(*run);
        auto const& ops = opsOf(*run, 0);
        ASSERT_EQ(ops.size(), 5u) << p.language;
        EXPECT_EQ(ops[4].offset, -40)
            << p.language << ": after restore_state the running CFA offset is "
            << "48 again, so the rel_offset folds to 8 - 48; reading the "
            << "un-popped 96 would give -88";
    });
}

// ══════════════════════════════════════════════════════════════════════════
// 3. THE REGISTER OPERAND — two forms, one answer; and the RA column
// ══════════════════════════════════════════════════════════════════════════

// ✔MEASURED 2026-08-17, GNU as 2.42: `.cfi_offset 6, -16`, `.cfi_offset
// %rbp, -16` and `.cfi_offset rbp, -16` all assemble rc=0 and produce
// BYTE-IDENTICAL `DW_CFA_offset: r6 (rbp) at cfa-16`; likewise `29` / `x29` /
// `%x29` on aarch64. gcc emits the NUMBER exclusively (both ports), so the
// numeric form is the one real input uses and the spelled form is what
// hand-written files reach for. Both must land on one `CfiRegRef`.
TEST(AsmCfiProducer, ADwarfNumberAndARegisterSpellingResolveToTheSameRegister) {
    forBothPorts([](Port const& p) {
        auto const byNum = lowerPort(
            p, fn(p, "main",
                  "\t.cfi_startproc\n\t.cfi_offset "
                      + std::string{p.savedRegNum} + ", -16\n")
                   + "\t.cfi_endproc\n");
        auto const byName = lowerPort(
            p, fn(p, "main",
                  "\t.cfi_startproc\n\t.cfi_offset "
                      + std::string{p.savedRegName} + ", -16\n")
                   + "\t.cfi_endproc\n");
        ASSERT_TRUE(byNum->module.has_value())
            << p.language << " numeric form: " << messages(*byNum);
        ASSERT_TRUE(byName->module.has_value())
            << p.language << " spelled form (" << p.savedRegName << "): "
            << messages(*byName);
        auto const& a = opsOf(*byNum, 0);
        auto const& b = opsOf(*byName, 0);
        ASSERT_EQ(a.size(), 1u) << p.language;
        ASSERT_EQ(b.size(), 1u) << p.language;
        EXPECT_TRUE(a[0].reg == b[0].reg)
            << p.language << ": DWARF number '" << p.savedRegNum
            << "' and spelling '" << p.savedRegName << "' must name ONE "
            << "register — GNU as produces byte-identical output for the two";
        EXPECT_FALSE(a[0].reg.isReturnAddress)
            << p.language << ": " << p.savedRegNum
            << " is an ordinary register, not the return-address column";
    });
}

// ★★★ THE WITNESS PICKED WHERE THE TWO VOCABULARIES **DISAGREE**. On x86_64 the
// RA column is 16 — synthetic, owned by no register, so a register-table lookup
// finds nothing and MUST fall to the column. On aarch64 it is 30, which IS x30's
// ordinary DWARF number, so a register-table-first lookup SUCCEEDS and silently
// produces a physical-register rule where the source named the return address.
// Neither port alone can catch both errors; this is why the pin is two-port and
// why it asserts `isReturnAddress` rather than "it resolved".
TEST(AsmCfiProducer, TheReturnAddressColumnResolvesAsTheRaColumnOnBothPorts) {
    forBothPorts([](Port const& p) {
        auto const run = lowerPort(
            p, fn(p, "main",
                  "\t.cfi_startproc\n\t.cfi_offset "
                      + std::to_string(p.raColumn) + ", -8\n")
                   + "\t.cfi_endproc\n");
        ASSERT_TRUE(run->module.has_value())
            << p.language << ": " << messages(*run);
        auto const& ops = opsOf(*run, 0);
        ASSERT_EQ(ops.size(), 1u) << p.language;
        EXPECT_TRUE(ops[0].reg.isReturnAddress)
            << p.language << ": DWARF column " << p.raColumn
            << " is this target's declared return-address column, so a rule "
            << "naming it is a rule about the RETURN ADDRESS — on aarch64 that "
            << "number is ALSO x30's, which is exactly the coincidence a "
            << "register-table-first lookup hides";
    });
}

// A number the target declares for no register and that is not the RA column is
// a frame slot this build cannot resolve — refused, never mapped onto the
// nearest thing. (255 is beyond both shipped tables.)
TEST(AsmCfiProducer, AnUnknownDwarfRegisterNumberIsRefusedByName) {
    forBothPorts([](Port const& p) {
        expectRefusedNaming(
            p, fn(p, "main", "\t.cfi_startproc\n\t.cfi_offset 255, -8\n")
                   + "\t.cfi_endproc\n",
            "declares no register with DWARF number 255");
    });
}

// ══════════════════════════════════════════════════════════════════════════
// 4. THE ANCHOR — which instruction a rule follows
// ══════════════════════════════════════════════════════════════════════════

// ★★★ A RULE ABOVE THE FIRST INSTRUCTION ANCHORS TO FUNCTION ENTRY, and the
// carrier for that is an INVALID `LirCfiOp::inst`. It is a state the producer
// STATES, not one the resolver infers from a lookup miss — a genuine miss is a
// producer/assembler disagreement and still fails loud. ✔MEASURED 2026-08-17:
// GNU as encodes the same state by HOISTING such rules into the CIE, which
// folds to identical per-PC rows (`frames-interp` agrees); DSS emits one shared
// CIE per module, so the FDE at offset 0 is where the rule has to go.
TEST(AsmCfiProducer, ARuleAboveTheFirstInstructionAnchorsToFunctionEntry) {
    forBothPorts([](Port const& p) {
        auto const run = lowerPort(
            p, fn(p, "main",
                  "\t.cfi_startproc\n\t.cfi_def_cfa_offset 16\n")
                   + "\t.cfi_endproc\n");
        ASSERT_TRUE(run->module.has_value())
            << p.language << ": " << messages(*run);
        auto const& ops = opsOf(*run, 0);
        ASSERT_EQ(ops.size(), 1u) << p.language;
        EXPECT_FALSE(ops[0].inst.valid())
            << p.language << ": a rule with no instruction above it must carry "
            << "the function-entry anchor (an invalid inst), which the resolver "
            << "turns into byte offset 0";
        EXPECT_FALSE(ops[0].atBlockEnd) << p.language;
    });
}

// And a rule BELOW an instruction anchors to it. The pair is what makes the
// emitted table PC-keyed rather than a frame shape.
TEST(AsmCfiProducer, ARuleBelowAnInstructionAnchorsToThatInstruction) {
    forBothPorts([](Port const& p) {
        // `.cfi_startproc`, then the dialect's own `ret` is the instruction,
        // then a rule under it. `fn()` supplies the `ret`, so the rule text
        // goes after it via a second body line.
        std::string const src =
            std::string{"\t.globl main\n\t.type main, "} + std::string{p.marker}
            + "\nmain:\n\t.cfi_startproc\n\t.cfi_def_cfa_offset 16\n\tret\n"
              "\t.cfi_def_cfa_offset 32\n\t.cfi_endproc\n";
        auto const run = lowerPort(p, src);
        ASSERT_TRUE(run->module.has_value())
            << p.language << ": " << messages(*run);
        auto const& ops = opsOf(*run, 0);
        ASSERT_EQ(ops.size(), 2u) << p.language;
        EXPECT_FALSE(ops[0].inst.valid())
            << p.language << ": the rule ABOVE the ret is entry-anchored";
        EXPECT_TRUE(ops[1].inst.valid())
            << p.language << ": the rule BELOW the ret must anchor to it — an "
            << "unanchored second rule would collapse both to offset 0 and "
            << "describe a frame that never changes";
    });
}

// ══════════════════════════════════════════════════════════════════════════
// 5. DESCRIBED vs UNDESCRIBED — and why "ops.empty()" is the wrong predicate
// ══════════════════════════════════════════════════════════════════════════

// ★★★ ✔MEASURED 2026-08-17: gcc 13.3.0 emits `.cfi_startproc` / `ret` /
// `.cfi_endproc` around a LEAF function — zero rules — and GNU as emits a real
// FDE for it. So a bracketed function with no rules is DESCRIBED, and a
// producer that engaged its slot on the first RULE would leave every leaf
// silently table-less. This pin and the next are a matched pair: one function
// bracketed and rule-free, one not bracketed at all, in ONE file.
TEST(AsmCfiProducer, ABracketedFunctionWithNoRulesIsStillDescribed) {
    forBothPorts([](Port const& p) {
        std::string const src =
            fn(p, "described", "\t.cfi_startproc\n") + "\t.cfi_endproc\n"
            + fn(p, "undescribed", "");
        auto const run = lowerPort(p, src);
        ASSERT_TRUE(parsedCleanly(*run))
            << p.language << " parse: " << parseMessages(*run);
        ASSERT_TRUE(run->module.has_value())
            << p.language << ": " << messages(*run);
        ASSERT_EQ(run->module->perFuncCfi.size(), 2u) << p.language;
        EXPECT_TRUE(run->module->perFuncCfi[0].has_value())
            << p.language << ": a frame-start/frame-end pair with no rules "
            << "between them DESCRIBES the function (gcc emits exactly that "
            << "for a leaf) and must get an unwind entry";
        EXPECT_TRUE(run->module->perFuncCfi[0]->ops.empty())
            << p.language << ": ...and it must have no rules";
        EXPECT_FALSE(run->module->perFuncCfi[1].has_value())
            << p.language << ": a function the source never bracketed is "
            << "UNDESCRIBED and must get no entry — an empty-but-present one "
            << "would make the pe writer demand a SizeOfProlog for a frame "
            << "nobody described, and the ELF writer emit an FDE over a "
            << "function that established none of its rules";
    });
}

// ══════════════════════════════════════════════════════════════════════════
// 6. THE FULL CHAIN — MEASURED byte offsets, and DWARF bytes out the far end
// ══════════════════════════════════════════════════════════════════════════

// ★★★ THE HOST-INDEPENDENT SIBLING OF THE gdb WITNESS. The corpus example
// proves a real unwinder walks the stack; it only runs on a host that can
// execute the binary. This drives the SAME chain — lower, `assemble()`,
// `attachAssemblyCfi`, `buildEhFrame` — and asserts the DWARF opcode bytes on
// every leg, so an encoding regression is caught where no binary can run.
TEST(AsmCfiProducer, TheProducerReachesRealDwarfBytesThroughTheSharedResolver) {
    forBothPorts([](Port const& p) {
        std::string const src =
            std::string{"\t.globl main\n\t.type main, "} + std::string{p.marker}
            + "\nmain:\n\t.cfi_startproc\n\tret\n\t.cfi_def_cfa_offset 64\n"
              "\t.cfi_endproc\n";
        auto run = lowerPort(p, src);
        ASSERT_TRUE(run->module.has_value())
            << p.language << ": " << messages(*run);
        auto attached = attachOf(*run);
        ASSERT_TRUE(attached.ok)
            << p.language << ": the shared resolver refused: "
            << [&] {
                   std::string m;
                   for (auto const& d : attached.reporter.all()) {
                       m += d.actual; m += '\n';
                   }
                   return m;
               }();
        ASSERT_EQ(attached.module.functions.size(), 1u) << p.language;
        auto const& cfi = attached.module.functions[0].cfi;
        ASSERT_TRUE(cfi.has_value())
            << p.language << ": the resolver produced no CfiFunction";

        // The entry state is DERIVED from the target's calling conventions and
        // is the one fact a `.s` cannot state. ✔MEASURED 2026-08-17: x86_64's
        // `callPushBytes` is 8 (the CALL pushes the return address) and
        // aarch64's is 0 (it lands in x30).
        EXPECT_EQ(cfi->initial.cfaOffset, p.entryCfa)
            << p.language << ": the entry CFA offset must be callPushBytes";
        EXPECT_EQ(cfi->initial.returnAddressAtCfaOffset.has_value(),
                  p.entryCfa != 0)
            << p.language << ": a pushed return address lives at a CFA offset; "
            << "a link-register ABI's lives in a register";

        // ★★★ THE FIELD THAT MAKES pe64 REFUSE, ASSERTED DIRECTLY. GNU `.cfi_*`
        // has NO prologue-end verb (✔MEASURED over gcc 13.3.0's complete
        // emitted spelling set on both ports), and Win64 `UNWIND_INFO` REQUIRES
        // `SizeOfProlog`. So this MUST stay nullopt: `pe.cpp` then refuses by
        // name instead of inventing a number that makes RtlVirtualUnwind
        // classify body PCs as mid-prologue and rebuild a frame that is not
        // there. ⚠ A derivation ("the last CFA-growing rule") gets every gcc
        // shape right and is wrong on hand-written code that allocates stack
        // twice — which is the code this tier exists for.
        EXPECT_FALSE(cfi->prologueEndPc.has_value())
            << p.language << ": an assembly producer cannot state where a "
            << "prologue ends, and a guessed SizeOfProlog is a silent Win64 "
            << "unwind miscompile";

        // The rule's PC is the byte offset PAST the `ret` the encoder emitted —
        // MEASURED, never assumed. On x86_64 `ret` is 1 byte; on aarch64 4.
        ASSERT_EQ(cfi->ops.size(), 1u) << p.language;
        EXPECT_EQ(cfi->ops[0].pcOffset,
                  static_cast<std::uint32_t>(
                      attached.module.functions[0].bytes.size()))
            << p.language << ": the rule sits under the function's only "
            << "instruction, so it takes effect at the end of it";

        // ── and out the far end as DWARF ──
        std::vector<std::optional<CfiFunction>> perFunc{cfi};
        DiagnosticReporter ehReporter;
        auto const sec = link::format::buildEhFrame(
            perFunc, link::format::dwarfRegisterMappingOf(*run->target), 8,
            ehReporter);
        ASSERT_TRUE(sec.has_value())
            << p.language << ": buildEhFrame refused the resolved stream";
        ASSERT_FALSE(sec->bytes.empty()) << p.language;
        ASSERT_EQ(sec->fdeOffsets.size(), 1u) << p.language;

        // The FDE's CFA program starts after: length(4) + cie_ptr(4) +
        // initial_location(4) + address_range(4) + aug-data-length(1).
        std::size_t const prog = sec->fdeOffsets[0] + 17;
        ASSERT_LT(prog + 2, sec->bytes.size()) << p.language;
        auto const advance = sec->bytes[prog];
        // `DW_CFA_advance_loc` (0x40 | delta) — the narrow form, because the
        // delta is one `ret`'s worth of bytes.
        EXPECT_EQ(advance & 0xC0u, link::format::kDwCfaAdvanceLocHi)
            << p.language << ": the first FDE opcode must advance the PC";
        EXPECT_EQ(advance & 0x3Fu,
                  attached.module.functions[0].bytes.size())
            << p.language << ": ...by exactly the function's byte length";
        EXPECT_EQ(sec->bytes[prog + 1], link::format::kDwCfaDefCfaOffset)
            << p.language << ": followed by DW_CFA_def_cfa_offset";
        // The offset is UNFACTORED because DSS's data-alignment factor is the
        // identity (-1) — `dwarf_cfi.hpp` documents why, and a factored 64/-8
        // would read as 8 here.
        EXPECT_EQ(sec->bytes[prog + 2], 64u)
            << p.language << ": ...with the offset the source stated";
    });
}

// ══════════════════════════════════════════════════════════════════════════
// 7. THE REFUSALS — everything this build cannot honour, by name
// ══════════════════════════════════════════════════════════════════════════

// ★★★ `.cfi_escape` MUST STAY A REFUSAL, and the anchor says so because
// accepting opaque bytes re-creates the row's own defect inside its fix.
// ✔MEASURED 2026-08-17 and it is stronger than the anchor claimed: GNU as
// validates NOTHING. `.cfi_escape 0x10, 0x06, 0x02` declares a 2-byte DWARF
// expression and supplies zero bytes, so the reader eats the NEXT TWO BYTES of
// the CFI program as the expression body — the following `.cfi_restore 6`
// VANISHES from the unwind program, at rc=0, with no diagnostic. That exact
// line used to sit in this repo's own corpus file.
TEST(AsmCfiProducer, EscapeIsRefusedByNameOnBothDialects) {
    forBothPorts([](Port const& p) {
        expectRefusedNaming(
            p, fn(p, "main",
                  "\t.cfi_startproc\n\t.cfi_escape 0x10, 0x06, 0x02\n")
                   + "\t.cfi_endproc\n",
            "cfi_escape");
        expectRefusedNaming(
            p, fn(p, "main",
                  "\t.cfi_startproc\n\t.cfi_escape 0x10, 0x06, 0x02\n")
                   + "\t.cfi_endproc\n",
            "UNREPRESENTABLE");
    });
}

// ✔MEASURED 2026-08-17: `.cfi_signal_frame` changes the CIE augmentation from
// "zR" to "zRS", which is a CIE-level property — assembling this repo's own
// corpus file produced TWO CIEs in one object because a signal frame and an
// ordinary frame cannot share one. DSS emits a single shared "zR" CIE per
// module, so the directive cannot be honoured and must not be swallowed.
TEST(AsmCfiProducer, SignalFrameIsRefusedByNameOnBothDialects) {
    forBothPorts([](Port const& p) {
        expectRefusedNaming(
            p, fn(p, "main", "\t.cfi_startproc\n\t.cfi_signal_frame\n")
                   + "\t.cfi_endproc\n",
            "cfi_signal_frame");
    });
}

// The column is a property of the whole image's shared CIE. Restating the
// target's own value is the no-op gcc emits; naming a different one asks for a
// per-function override that cannot be expressed, so it is refused with BOTH
// numbers in the message. ✔MEASURED: gas happily writes any value here, so
// accepting it silently would let a `.s` redefine where the return address
// lives and have no effect at all.
TEST(AsmCfiProducer, AReturnColumnThatDisagreesWithTheTargetIsRefused) {
    forBothPorts([](Port const& p) {
        // A column that is neither port's answer, so one string serves both.
        expectRefusedNaming(
            p, fn(p, "main", "\t.cfi_startproc\n\t.cfi_return_column 3\n")
                   + "\t.cfi_endproc\n",
            "return-address column 3");
        // ...and the matching POSITIVE control, so the refusal cannot degenerate
        // into "every return_column is refused".
        auto const ok = lowerPort(
            p, fn(p, "main",
                  "\t.cfi_startproc\n\t.cfi_return_column "
                      + std::to_string(p.raColumn) + "\n")
                   + "\t.cfi_endproc\n");
        EXPECT_TRUE(ok->module.has_value())
            << p.language << ": restating the target's own column "
            << p.raColumn << " is the no-op gcc emits and must be ACCEPTED: "
            << messages(*ok);
    });
}

TEST(AsmCfiProducer, StructuralRefusalsAreAllLoudAndNamed) {
    forBothPorts([](Port const& p) {
        // A rule with no frame description to belong to.
        expectRefusedNaming(
            p, fn(p, "main", "\t.cfi_def_cfa_offset 16\n"),
            "outside any frame description");
        // A description that is never closed — refusing rather than closing it
        // at the function boundary, which would invent an extent.
        expectRefusedNaming(
            p, fn(p, "main", "\t.cfi_startproc\n"),
            "never closed");
        // ...and one closed twice.
        expectRefusedNaming(
            p, fn(p, "main", "\t.cfi_startproc\n") + "\t.cfi_endproc\n"
                   + "\t.cfi_endproc\n",
            "never opened");
        // Descriptions do not nest.
        expectRefusedNaming(
            p, fn(p, "main", "\t.cfi_startproc\n\t.cfi_startproc\n")
                   + "\t.cfi_endproc\n",
            "while one is already open");
        // An unbalanced restore_state — the producer's own model of the frame
        // is inconsistent, and `foldCfiOps` returns nullopt on it downstream.
        expectRefusedNaming(
            p, fn(p, "main", "\t.cfi_startproc\n\t.cfi_restore_state\n")
                   + "\t.cfi_endproc\n",
            "never remembered");
        // ...and a remember that is never restored, caught at the close.
        expectRefusedNaming(
            p, fn(p, "main", "\t.cfi_startproc\n\t.cfi_remember_state\n")
                   + "\t.cfi_endproc\n",
            "never restored");
        // The wrong operand count. `.cfi_offset` takes a register AND an
        // offset; one operand is not "close enough".
        expectRefusedNaming(
            p, fn(p, "main",
                  "\t.cfi_startproc\n\t.cfi_offset "
                      + std::string{p.savedRegNum} + "\n")
                   + "\t.cfi_endproc\n",
            "takes 2 operand(s); 1 were written");
        // A frame description with no function to describe.
        expectRefusedNaming(p, "\t.text\n\t.cfi_startproc\n",
                            "with no function open");
    });
}

// ══════════════════════════════════════════════════════════════════════════
// 8. THE SHIPPED DOCUMENTS — the per-ROW guard, on every host
// ══════════════════════════════════════════════════════════════════════════

// ★★★ THE ANTI-REGRESSION GUARD THE ANCHOR ASKED FOR IN WORDS. The row's
// closing note says a cycle that declared these spellings as
// `ignoredAnnotation` — to make the two dialects "consistent" — would be
// propagating the row's own defect into a second dialect in the name of
// tidiness. This is that sentence as a test: NO `cfi_*` row in EITHER shipped
// dialect may carry `ignoredAnnotation`, ever. It also covers the spellings
// nobody has declared yet: the day `.cfi_personality` arrives, adding it as an
// annotation reds here.
TEST(AsmCfiProducer, NoShippedCfiRowIsAnIgnoredAnnotation) {
    forBothPorts([](Port const& p) {
        auto const doc  = shippedDialectDoc(p.language);
        auto const& dirs = doc.at("assembly").at("directives");
        std::size_t cfiRows = 0;
        for (auto const& row : dirs) {
            if (!row.contains("spelling")) continue;
            auto const sp = row.at("spelling").get<std::string>();
            if (!sp.starts_with("cfi_")) continue;
            ++cfiRows;
            ASSERT_TRUE(row.contains("verb")) << p.language << " " << sp;
            auto const verb = row.at("verb").get<std::string>();
            EXPECT_NE(verb, "ignoredAnnotation")
                << p.language << ": '." << sp << "' is declared as an "
                << "ignoredAnnotation, which means ACCEPTED AND DROPPED. That "
                << "is D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED itself: the file "
                << "assembles, the program runs, and the frame cannot be "
                << "unwound. Declare it as a frameRule/frameStart/frameEnd/"
                << "frameReturnColumn, or as 'unrepresentable' with a $comment "
                << "stating the fact this build cannot carry";
        }
        // Anti-vacuous: a document that declared NO cfi rows would pass the
        // loop above trivially, and on arm64 that was the state until
        // 2026-08-17. Assert the family is actually present.
        EXPECT_GE(cfiRows, 18u)
            << p.language << ": the shipped dialect declares only " << cfiRows
            << " `.cfi_*` rows, so the loop above asserted almost nothing";
    });
}

// The per-ROW mapping, spelled out. Deleting a row from either shipped document
// reds HERE as well as in the corpus example, so the guard survives on hosts
// that cannot run either binary.
TEST(AsmCfiProducer, BothShippedDialectsBindTheSameSpellingsToTheSameRules) {
    struct Want { std::string_view spelling, verb, rule; bool fromCfa; };
    constexpr std::array<Want, 18> kWant{{
        {"cfi_startproc",        "frameStart",        "",                  false},
        {"cfi_endproc",          "frameEnd",          "",                  false},
        {"cfi_def_cfa",          "frameRule",         "def_cfa",           false},
        {"cfi_def_cfa_offset",   "frameRule",         "def_cfa_offset",    false},
        {"cfi_def_cfa_register", "frameRule",         "def_cfa_register",  false},
        {"cfi_adjust_cfa_offset","frameRule",         "adjust_cfa_offset", false},
        {"cfi_offset",           "frameRule",         "offset",            false},
        {"cfi_rel_offset",       "frameRule",         "offset",            true},
        {"cfi_val_offset",       "frameRule",         "val_offset",        false},
        {"cfi_register",         "frameRule",         "register",          false},
        {"cfi_restore",          "frameRule",         "restore",           false},
        {"cfi_undefined",        "frameRule",         "undefined",         false},
        {"cfi_same_value",       "frameRule",         "same_value",        false},
        {"cfi_return_column",    "frameReturnColumn", "",                  false},
        {"cfi_remember_state",   "frameRule",         "remember_state",    false},
        {"cfi_restore_state",    "frameRule",         "restore_state",     false},
        {"cfi_signal_frame",     "unrepresentable",   "",                  false},
        {"cfi_escape",           "unrepresentable",   "",                  false},
    }};
    forBothPorts([&kWant](Port const& p) {
        auto const doc = shippedDialectDoc(p.language);
        auto const& dirs = doc.at("assembly").at("directives");
        for (auto const& w : kWant) {
            nlohmann::json const* found = nullptr;
            for (auto const& row : dirs) {
                if (!row.contains("spelling")) continue;
                if (row.at("spelling").get<std::string>() != w.spelling) {
                    continue;
                }
                found = &row;
                break;
            }
            ASSERT_NE(found, nullptr)
                << p.language << ": '." << w.spelling << "' is not declared";
            EXPECT_EQ(found->at("verb").get<std::string>(), w.verb)
                << p.language << " '." << w.spelling << "'";
            if (w.rule.empty()) {
                EXPECT_FALSE(found->contains("rule"))
                    << p.language << " '." << w.spelling
                    << "' must carry no rule";
            } else {
                ASSERT_TRUE(found->contains("rule"))
                    << p.language << " '." << w.spelling << "'";
                EXPECT_EQ(found->at("rule").get<std::string>(), w.rule)
                    << p.language << " '." << w.spelling << "'";
            }
            bool const fromCfa = found->contains("offsetFromCfa")
                              && found->at("offsetFromCfa").get<bool>();
            EXPECT_EQ(fromCfa, w.fromCfa)
                << p.language << " '." << w.spelling
                << "': `.cfi_rel_offset` is the ONLY spelling whose offset is "
                << "measured from the running CFA (8 - 48 = -40, ✔MEASURED on "
                << "GNU as 2.42)";
        }
    });
}

// ══════════════════════════════════════════════════════════════════════════
// 9. THE LOADER — a malformed frame row is a LOAD error, not a live surprise
// ══════════════════════════════════════════════════════════════════════════

// The engine-tier fixture, whose `setDirectives` writes exactly the rows a test
// needs. ⚠ These are CONFIG-level, so this file MUST run through `ctest`:
// `dss_add_test` sets `DSS_CONFIG_ROOT`, while a bare `.exe` walks the cwd and
// would read whichever tree the shell stands in
// (D-TEST-CONFIG-RED-ON-DISABLE-READS-THE-WRONG-TREE).
[[nodiscard]] std::unique_ptr<LoweringRun>
loadWithDirectives(std::vector<DirRow> rows) {
    auto doc = baseDialectDoc();
    setDirectives(doc, std::move(rows));
    return lowerAsmText(doc, "\t.text\n");
}

void expectLoadErrorNaming(std::vector<DirRow> rows, std::string_view needle) {
    auto const run = loadWithDirectives(std::move(rows));
    ASSERT_FALSE(run->loadErrors.empty())
        << "the document LOADED; it should have been refused";
    std::string joined;
    for (auto const& e : run->loadErrors) { joined += e; joined += '\n'; }
    EXPECT_NE(joined.find(needle), std::string::npos)
        << "the load error did not name '" << needle << "'. Actual: " << joined;
}

TEST(AsmCfiProducer, MalformedFrameRowsAreLoadErrors) {
    auto base = baseDirectives();

    // A `frameRule` with no rule: the walker could not know what it states.
    {
        auto rows = base;
        rows.push_back({"cfi_startproc", "frameStart", ""});
        rows.push_back({"cfi_endproc",   "frameEnd",   ""});
        rows.push_back({"cfi_offset",    "frameRule",  ""});
        expectLoadErrorNaming(rows, "'rule' is required");
    }
    // An unknown rule name — refused naming the closed set, never defaulted.
    {
        auto rows = base;
        rows.push_back({"cfi_startproc", "frameStart", ""});
        rows.push_back({"cfi_endproc",   "frameEnd",   ""});
        DirRow bad{"cfi_odd", "frameRule", ""};
        bad.rule = "def_cfa_sideways";
        rows.push_back(bad);
        expectLoadErrorNaming(rows, "unknown call-frame rule");
    }
    // `rule` on a verb that is not `frameRule`.
    {
        auto rows = base;
        DirRow bad{"nonsense", "ignoredAnnotation", ""};
        bad.rule = "def_cfa";
        rows.push_back(bad);
        expectLoadErrorNaming(rows, "'rule' is only meaningful");
    }
    // `offsetFromCfa` on a rule that carries no register save — the key would
    // re-base a number that does not exist.
    {
        auto rows = base;
        rows.push_back({"cfi_startproc", "frameStart", ""});
        rows.push_back({"cfi_endproc",   "frameEnd",   ""});
        DirRow bad{"cfi_rem", "frameRule", ""};
        bad.rule          = "remember_state";
        bad.offsetFromCfa = true;
        rows.push_back(bad);
        expectLoadErrorNaming(rows, "'offsetFromCfa' is only meaningful");
    }
    // A frame BODY with nothing that can open a frame — rows nothing can reach.
    {
        auto rows = base;
        DirRow orphan{"cfi_offset", "frameRule", ""};
        orphan.rule = "offset";
        rows.push_back(orphan);
        expectLoadErrorNaming(rows, "declares no directive with verb "
                                    "'frameStart'");
    }
    // A frame that can be opened and never closed.
    {
        auto rows = base;
        rows.push_back({"cfi_startproc", "frameStart", ""});
        expectLoadErrorNaming(rows, "with no 'frameEnd' counterpart");
    }
    // ...and the POSITIVE control, so the cross-row checks cannot degenerate
    // into "any frame row is refused".
    {
        auto rows = base;
        rows.push_back({"cfi_startproc", "frameStart", ""});
        rows.push_back({"cfi_endproc",   "frameEnd",   ""});
        DirRow good{"cfi_offset", "frameRule", ""};
        good.rule = "offset";
        rows.push_back(good);
        auto const run = loadWithDirectives(rows);
        std::string joined;
        for (auto const& e : run->loadErrors) { joined += e; joined += '\n'; }
        EXPECT_TRUE(run->loadErrors.empty())
            << "a well-formed frame row set was refused: " << joined;
    }
}

} // namespace
