// ONE SPELLING, TWO BINDING ROWS — REFUSED AT THE TIER THAT BUILDS THE ROWS.
// D-ASM-DUPLICATE-SYMBOLIC-NAME-BINDS-THE-WRONG-OPERAND.
//
// ★★★ THE CLAIM. `mir_to_lir`'s asm expansion publishes ONE binding row per
// SPELLING — that is what makes `%0` and `%[out]` two keys for one register with
// no change to the shared assembly engine. What "one row per spelling" does NOT
// give you is that the row set is a FUNCTION from spelling to binding, and both
// engine lookups (`TemplateHost::bindingFor` for an operand, `resolveBranchTarget`
// for a label) are FIRST-MATCH scans. So a spelling published twice does not
// fail: it silently discards one of the two rows and lowers the template against
// the other.
//
// ★★ THIS IS DEFENCE IN DEPTH, AND THE DEPTH IS THE POINT. The C front end now
// refuses a symbolic name used twice in one statement
// (`S_InlineAsmDuplicateSymbolicName`), so no c-subset source can reach these
// shapes — which is exactly why they are BUILT BY HAND here. The tier that mints
// the spellings and the tier that binds them are two tiers apart, and this one
// has producers that never run the C semantic pass at all: the LSP, the FFI
// header parser, the optimizer's rebuild carriage, a deserializer, and every
// hand-built descriptor in this suite. A guard nobody exercises is one nobody
// notices deleting — and deleting this one restores a MEASURED miscompile.
//
// ✔MEASURED 2026-08-19 through the shipped CLI, before the front-end refusal
// existed: `__asm__("movl %[v], %[out]" : [out] "=r"(r), [v] "=r"(d)
// : [v] "r"(a))` with `a == 20` compiled rc=0 for
// `x86_64:pe64-x86_64-windows-exec` at BOTH `--config=debug` and
// `--config=release`, and the program exited 0. The duplicate `%[v]` bound the
// OUTPUT, because outputs are pushed into the row list first.
//
// ★ EVERY ARM IS A MATCHED PAIR. A refusal that fires on everything asserts
// nothing, so each duplicate shape is run beside the SAME descriptor with the
// collision removed, through the same lowering on the same target, and the
// control must LOWER CLEAN. Without the controls this file is satisfied by a
// lowering that refuses every named operand.
//
// ⚠ CONFIG-LEVEL: `dss_add_test` sets `DSS_CONFIG_ROOT`, so this file must run
// through ctest and never as a bare `.exe`
// (D-TEST-CONFIG-RED-ON-DISABLE-READS-THE-WRONG-TREE).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "lir/lir.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_asm_descriptor.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

// Every diagnostic's text, concatenated — so a failure prints WHAT was reported
// rather than only that something was.
[[nodiscard]] std::string inventory(DiagnosticReporter const& r) {
    std::string out;
    for (auto const& d : r.all()) {
        out += diagnosticCodeName(d.code);
        out += ": ";
        out += d.actual;
        out += "\n";
    }
    return out.empty() ? std::string{"(nothing reported)"} : out;
}

[[nodiscard]] bool anyTextContains(DiagnosticReporter const& r,
                                   std::string_view needle) {
    for (auto const& d : r.all()) {
        if (d.actual.find(needle) != std::string::npos) return true;
    }
    return false;
}

// One GPR operand entry answering to the given spellings.
[[nodiscard]] MirAsmOperand gpr(std::string constraint,
                                std::vector<std::string> spellings) {
    MirAsmOperand o;
    o.constraint = std::move(constraint);
    o.regClass   = TargetRegClass::GPR;
    o.spellings  = std::move(spellings);
    return o;
}

// A non-terminator `__asm__` whose ONE output and ONE input carry the spellings
// given. `dupSymbolic` decides whether both answer to the same symbolic form.
//
// ★ THE TEMPLATE IS THE SAME IN BOTH ARMS AND WRITES ONLY THE POSITIONAL FORMS,
// so the pair differs in exactly one thing: whether a spelling is published
// twice. A template that WROTE the ambiguous form would make the control and the
// mutant differ in their instruction text as well, and the arm would no longer
// be about the row set.
struct AsmModule {
    TypeInterner interner{CompilationUnitId{1}};
    Mir          mir;
};

[[nodiscard]] Mir buildTwoOperandAsm(TypeInterner& interner, bool dupSymbolic) {
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirLiteralValue lit;
    lit.value = std::int64_t{7};
    lit.core  = TypeKind::I32;
    MirInstId const seed = mb.addConst(lit, i32);

    MirAsmDescriptor d;
    d.templateText = "movl %1, %0";
    d.isExtended   = true;
    d.outputs.push_back(gpr("=r", {"%0", "%[v]"}));
    d.inputs.push_back(gpr("r", dupSymbolic
                                    ? std::vector<std::string>{"%1", "%[v]"}
                                    : std::vector<std::string>{"%1", "%[w]"}));
    std::array<MirInstId, 1> const ops{seed};
    (void)mb.addInlineAsm(std::move(d), ops, i32);
    mb.addReturn();
    return std::move(mb).finish();
}

// An `asm goto` with TWO labels. `dupSymbolic` decides whether both label
// entries publish the same bracketed form.
//
// ⚠ BOTH LABEL BLOCKS ARE DISTINCT HERE, which is the stronger shape: from
// c-subset a repeated label name resolves to ONE block through
// `getOrCreateLabelBlock`, so the two rows would name the same MIR block. A
// direct producer is under no such constraint, and the row set is what this tier
// can see.
[[nodiscard]] Mir buildTwoLabelAsmGoto(TypeInterner& interner,
                                       bool dupSymbolic) {
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const fnSig = interner.fnSig({}, voidT, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const one   = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const two   = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    MirLiteralValue lit;
    lit.value = std::int64_t{7};
    lit.core  = TypeKind::I32;
    MirInstId const seed = mb.addConst(lit, i32);

    MirAsmDescriptor d;
    d.templateText = "jmp %l2";
    d.isExtended   = true;
    d.inputs.push_back(gpr("r", {"%0"}));
    d.labelSpellings.push_back({"%l[hit]", "%l1"});
    d.labelSpellings.push_back(
        dupSymbolic ? std::vector<std::string>{"%l[hit]", "%l2"}
                    : std::vector<std::string>{"%l[miss]", "%l2"});
    std::array<MirInstId, 1> const ops{seed};
    std::array<MirBlockId, 2> const labels{one, two};
    auto const res = mb.addInlineAsmGoto(std::move(d), ops, labels);
    for (auto const& e : res.edges) {
        if (!e.split) continue;
        mb.beginBlock(e.successor);
        mb.addBr(e.onward);
    }
    mb.beginBlock(one);
    mb.addReturn();
    mb.beginBlock(two);
    mb.addReturn();
    mb.beginBlock(res.continuation());
    mb.addReturn();
    return std::move(mb).finish();
}

} // namespace

// ── ARM 1: TWO OPERAND ENTRIES PUBLISHING ONE SPELLING ───────────────────────
TEST(LirAsmBindingCollision, TwoOperandsAnsweringToOneSpellingAreRefused) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());

    TypeInterner       badInterner{CompilationUnitId{1}};
    Mir const          bad = buildTwoOperandAsm(badInterner, /*dupSymbolic=*/true);
    DiagnosticReporter badReporter;
    MirToLirResult const badResult =
        lowerToLir(bad, **target, badInterner, badReporter);

    EXPECT_FALSE(badResult.ok)
        << "publishing `%[v]` at two registers lets a first-match lookup keep "
           "whichever row it saw first — accepting it is a silent miscompile "
           "with a correct-looking rc=0: " << inventory(badReporter);
    EXPECT_TRUE(anyTextContains(badReporter, "the operand spelling '%[v]' is "
                                             "bound TWICE"))
        << "the refusal must NAME the colliding spelling — a generic 'bad "
           "descriptor' would be true and useless: " << inventory(badReporter);
    EXPECT_TRUE(anyTextContains(
        badReporter, "D-ASM-DUPLICATE-SYMBOLIC-NAME-BINDS-THE-WRONG-OPERAND"))
        << inventory(badReporter);
    EXPECT_TRUE(anyTextContains(badReporter, "S_InlineAsmDuplicateSymbolicName"))
        << "the message must point a reader at the front-end refusal, so a "
           "descriptor arriving here is understood as coming from a producer "
           "that does not run that pass: " << inventory(badReporter);

    // THE CONTROL — the same descriptor with the second symbolic form renamed.
    TypeInterner       okInterner{CompilationUnitId{1}};
    Mir const          ok = buildTwoOperandAsm(okInterner, /*dupSymbolic=*/false);
    DiagnosticReporter okReporter;
    MirToLirResult const okResult =
        lowerToLir(ok, **target, okInterner, okReporter);
    EXPECT_TRUE(okResult.ok)
        << "two DISTINCT symbolic names on the same statement are the ordinary "
           "case and must lower: " << inventory(okReporter);
}

// ── ARM 2: TWO LABEL ENTRIES PUBLISHING ONE SPELLING ─────────────────────────
//
// ★ THE LABEL HALF IS NOT REDUNDANT WITH THE OPERAND HALF. They are two separate
// row sets, built at two different points in the expansion, consumed by two
// different host methods — and the label check must run AFTER
// `createAsmCaptureBlocks`, so it owes the orphan-block seal that the operand
// check does not. A fix applied to only one list leaves the other's first-match
// lookup unguarded, and this arm is what says which one broke.
TEST(LirAsmBindingCollision, TwoAsmGotoLabelsAnsweringToOneSpellingAreRefused) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());

    TypeInterner       badInterner{CompilationUnitId{1}};
    Mir const          bad = buildTwoLabelAsmGoto(badInterner, true);
    DiagnosticReporter badReporter;
    MirToLirResult const badResult =
        lowerToLir(bad, **target, badInterner, badReporter);

    EXPECT_FALSE(badResult.ok)
        << "two labels answering to `%l[hit]` leave `resolveBranchTarget` free "
           "to take either edge: " << inventory(badReporter);
    EXPECT_TRUE(anyTextContains(
        badReporter, "the `asm goto` label spelling '%l[hit]' is bound TWICE"))
        << "the refusal must name the LABEL role, not the operand one — the two "
           "row sets have different remedies: " << inventory(badReporter);

    // THE CONTROL — the same two-label `asm goto` with distinct label names.
    TypeInterner       okInterner{CompilationUnitId{1}};
    Mir const          ok = buildTwoLabelAsmGoto(okInterner, false);
    DiagnosticReporter okReporter;
    MirToLirResult const okResult =
        lowerToLir(ok, **target, okInterner, okReporter);
    EXPECT_TRUE(okResult.ok)
        << "two DISTINCT label names on one `asm goto` must lower: "
        << inventory(okReporter);
}
