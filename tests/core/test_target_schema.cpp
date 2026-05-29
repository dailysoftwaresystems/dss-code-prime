#include "core/types/target_schema.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <string_view>

// Negative-path tests for the `TargetSchema` JSON loader. Mirrors the
// shape of `test_grammar_schema.cpp` since the two loaders are parallel
// by design — every rejection branch the JSON loader can hit should
// have one test that pins the diagnostic code so a future regression
// (e.g. silently accepting a malformed config) fails the build.
//
// The happy-path smoke check + substrate integration live in
// `tests/lir/test_lir.cpp` (which loads the shipped x86_64.target.json
// and exercises the LIR builder against it). This file only covers
// the negative branches.

namespace {

using ::dss::DiagnosticCode;
using ::dss::TargetRegClass;
using ::dss::TargetSchema;

bool anyHasCode(auto const& diags, DiagnosticCode code) {
    return std::ranges::any_of(diags, [code](auto const& d) {
        return d.code == code;
    });
}

}  // namespace

TEST(TargetSchema, MalformedJsonReportsCode) {
    auto r = TargetSchema::loadFromText("not valid json {{{ ", "<inline>");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, NonObjectTopLevelRejected) {
    auto r = TargetSchema::loadFromText("[]", "<inline>");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, MissingDssTargetVersionReportsCode) {
    auto r = TargetSchema::loadFromText(
        R"({"target":{"name":"X"},"opcodes":[{"mnemonic":"invalid","result":"none"}]})",
        "<inline>");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_VersionMismatch));
}

TEST(TargetSchema, UnsupportedVersionRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":99,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}]})",
        "<inline>");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_VersionMismatch));
}

TEST(TargetSchema, MissingTargetObjectRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,
            "opcodes":[{"mnemonic":"invalid","result":"none"}]})",
        "<inline>");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MissingField));
}

TEST(TargetSchema, MissingOpcodesArrayRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"}})",
        "<inline>");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MissingField));
}

TEST(TargetSchema, EmptyOpcodesArrayRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},"opcodes":[]})",
        "<inline>");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MissingField));
}

TEST(TargetSchema, Slot0MustBeInvalidSentinel) {
    // Slot 0 with a real mnemonic — must be rejected, because the LIR
    // substrate treats opcode 0 as the unconditionally-invalid sentinel.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"mov","result":"value"}]})",
        "<inline>");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, DuplicateMnemonicRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"mov","result":"value"},
              {"mnemonic":"mov","result":"value"}
            ]})",
        "<inline>");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, InvalidResultRuleRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"mov","result":"banana"}
            ]})",
        "<inline>");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, ArityFieldOutOfRangeIsSkippedAndDiagnosed) {
    // minOperands=300 cannot fit in uint8 — loader must diagnose and
    // skip the assignment rather than silently truncating to 44.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"mov","result":"value","minOperands":300}
            ]})",
        "<inline>");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, LoadShippedRejectsPathLikeNames) {
    for (auto name : {std::string_view{""},
                      std::string_view{".hidden"},
                      std::string_view{"a/b"},
                      std::string_view{"a\\b"}}) {
        auto r = TargetSchema::loadShipped(name);
        ASSERT_FALSE(r.has_value()) << "expected rejection for: " << name;
        EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_InvalidLanguageName))
            << "name='" << name << "'";
    }
}

TEST(TargetSchema, LoadShippedReportsNotFoundForUnknownName) {
    auto r = TargetSchema::loadShipped("definitely_not_a_real_target_xyz");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_InvalidLanguageName));
}

TEST(TargetSchema, EachLoadMintsDistinctSchemaId) {
    auto a = TargetSchema::loadShipped("x86_64");
    auto b = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(a.has_value()) << "x86_64 must be a shipped target";
    ASSERT_TRUE(b.has_value());
    EXPECT_NE((*a)->id(), (*b)->id())
        << "two independent loads must produce distinct TargetSchemaIds — "
           "otherwise the substrate cross-check between Lir::targetId and "
           "the schema reference would silently alias unrelated builders";
}

TEST(TargetSchema, LoadFromTextDefaultsSourceLabel) {
    auto r = TargetSchema::loadFromText("not json");
    ASSERT_FALSE(r.has_value());
    // Default sourceLabel should appear in the diagnostic path field — pins
    // the GrammarSchema API parity (cycle 2a had no default; cycle 2b adds it).
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// ─── cycle 2b — registers section ────────────────────────────────────────

TEST(TargetSchema, RegistersOptionalForCycle2aShape) {
    // A cycle-2a-shape config (no `registers`, no `callingConventions`) must
    // still load — the loader treats both as optional for now so existing
    // shipped configs / round-trip-test fixtures keep working.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}]})");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->registerCount(), 0u);
    EXPECT_EQ((*r)->callingConventionCount(), 0u);
}

TEST(TargetSchema, ShippedX86_64ParsesFullRegisterFile) {
    auto r = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(r.has_value());
    auto const& sch = **r;

    // GPRs + XMM* + rflags.
    EXPECT_GE(sch.registerCount(), 33u);

    // Spot-checks against SysV AMD64 references.
    auto rdi = sch.registerByName("rdi");
    ASSERT_TRUE(rdi.has_value());
    EXPECT_EQ(sch.registerInfo(*rdi)->regClass, TargetRegClass::GPR);
    EXPECT_EQ(sch.registerInfo(*rdi)->widthBytes, 8);

    auto xmm0 = sch.registerByName("xmm0");
    ASSERT_TRUE(xmm0.has_value());
    EXPECT_EQ(sch.registerInfo(*xmm0)->regClass, TargetRegClass::FPR);
    EXPECT_EQ(sch.registerInfo(*xmm0)->widthBytes, 16);

    auto rflags = sch.registerByName("rflags");
    ASSERT_TRUE(rflags.has_value());
    EXPECT_EQ(sch.registerInfo(*rflags)->regClass, TargetRegClass::Flags);

    // Unknown name returns nullopt.
    EXPECT_FALSE(sch.registerByName("definitely_not_a_reg").has_value());
}

TEST(TargetSchema, DuplicateRegisterNameRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[
              {"name":"rax","class":"gpr"},
              {"name":"rax","class":"gpr"}
            ]})");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, RegisterWidthOutOfRangeRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[{"name":"r","class":"gpr","widthBytes":-1}]})");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, RegisterSubOfMustResolve) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[{"name":"eax","class":"gpr","subOf":"rax"}]})");
    ASSERT_FALSE(r.has_value())
        << "subOf='rax' must fail-loud when 'rax' is not declared";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// ─── cycle 2b — calling conventions ──────────────────────────────────────

TEST(TargetSchema, ShippedX86_64HasBothSysVAndMsX64) {
    auto r = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(r.has_value());
    auto const& sch = **r;
    EXPECT_EQ(sch.callingConventionCount(), 2u);

    auto const* sysv = sch.callingConventionByName("sysv_amd64");
    ASSERT_NE(sysv, nullptr);
    // SysV first 6 int args.
    ASSERT_EQ(sysv->argGprs.size(), 6u);
    EXPECT_EQ(sysv->argGprs[0], "rdi");
    EXPECT_EQ(sysv->argGprs[5], "r9");
    // SysV uses 8 XMM regs for float args.
    EXPECT_EQ(sysv->argFprs.size(), 8u);
    // 16-byte stack align, 128-byte red zone, no shadow space.
    EXPECT_EQ(sysv->stackAlignment, 16);
    EXPECT_EQ(sysv->redZoneBytes, 128);
    EXPECT_EQ(sysv->shadowSpaceBytes, 0);

    auto const* msx64 = sch.callingConventionByName("ms_x64");
    ASSERT_NE(msx64, nullptr);
    // MS x64 uses 4 GPRs for args.
    ASSERT_EQ(msx64->argGprs.size(), 4u);
    EXPECT_EQ(msx64->argGprs[0], "rcx");
    EXPECT_EQ(msx64->argGprs[3], "r9");
    // 32-byte shadow space, no red zone.
    EXPECT_EQ(msx64->shadowSpaceBytes, 32);
    EXPECT_EQ(msx64->redZoneBytes, 0);
}

TEST(TargetSchema, CallingConventionUnknownRegisterRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[{"name":"rax","class":"gpr"}],
            "callingConventions":[
              {"name":"bogus","argGprs":["does_not_exist"]}
            ]})");
    ASSERT_FALSE(r.has_value())
        << "calling-convention reference to undeclared register must fail-loud";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, CallingConventionRegisterClassMismatchRejected) {
    // `argFprs` must name FPR-class registers. Pointing it at a GPR should
    // fail validate().
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[{"name":"rax","class":"gpr"}],
            "callingConventions":[
              {"name":"bad","argFprs":["rax"]}
            ]})");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, DuplicateCallingConventionNameRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "callingConventions":[
              {"name":"dupe"},
              {"name":"dupe"}
            ]})");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// ─── cycle 2b — cross-field validate() ────────────────────────────────────

TEST(TargetSchema, OpcodeMinGreaterThanMaxRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"mov","result":"value","minOperands":5,"maxOperands":2}
            ]})");
    ASSERT_FALSE(r.has_value())
        << "validate() must catch min>max even when each field parses cleanly";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, OpcodeMinSuccessorsGreaterThanMaxRejected) {
    // The successors-axis parallel of the min>max check. Pinned so a
    // future refactor that drops the second `if` in validate() trips
    // this test rather than silently passing.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"jmp","result":"none","isTerminator":true,
               "minSuccessors":3,"maxSuccessors":1}
            ]})");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, OpcodeTerminatorMinSuccessorsButZeroMaxRejected) {
    // `isTerminator: true` with `minSuccessors: 1, maxSuccessors: 0` is
    // self-contradictory. The Return shape (min=max=0) stays legal.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"jmp","result":"none","isTerminator":true,
               "minSuccessors":1,"maxSuccessors":0}
            ]})");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, OpcodeNonTerminatorWithSuccessorsRejected) {
    // Only terminators have CFG successors. A non-terminator with
    // maxSuccessors>0 is structurally impossible.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"mov","result":"value","isTerminator":false,
               "maxSuccessors":2}
            ]})");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, OpcodeReturnShapeIsLegal) {
    // Positive control: a `return`-kinded opcode with
    // (minSuccessors=0, maxSuccessors=0) must validate cleanly.
    // `terminatorKind` is MANDATORY on every terminator (validate()
    // enforces `isTerminator ↔ terminatorKind ≠ NotATerminator`).
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"ret","result":"none","isTerminator":true,
               "terminatorKind":"return",
               "minSuccessors":0,"maxSuccessors":0}
            ]})");
    ASSERT_TRUE(r.has_value())
        << "Return-shaped opcode (terminator + min=max=0) must validate cleanly";
}

TEST(TargetSchema, TerminatorKindIsTheSingleSourceOfTruth) {
    // With the `isTerminator` boolean field deleted (3-agent
    // convergence ML8 cycle 3 review: type-design + simplifier +
    // silent-failure), terminator-ness derives solely from
    // `terminatorKind != none`. The JSON `isTerminator` key is
    // IGNORED by the loader (kept here only to prove the loader
    // doesn't choke on a stray legacy key — but the value has no
    // effect). What matters is `terminatorKind`.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"add","result":"value","isTerminator":true,
               "minOperands":2,"maxOperands":2}
            ]})");
    ASSERT_TRUE(r.has_value())
        << "Legacy `isTerminator` key must be silently ignored; terminator-"
           "ness derives from `terminatorKind` (default `none` = non-"
           "terminator).";
    auto info = r.value()->opcodeInfo(1);
    ASSERT_NE(info, nullptr);
    EXPECT_FALSE(info->isTerminator())
        << "no `terminatorKind` declared → derived isTerminator() is false";
}

TEST(TargetSchema, TerminatorKindNonStringRejected) {
    // Silent-failure F1 (ML8 cycle 3 review): a non-string
    // `terminatorKind` value (e.g. integer, null) must be loud-rejected.
    // Earlier draft silently fell back to the default `none`, masking
    // the schema author's intent.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"ret","result":"none","terminatorKind":4,
               "minSuccessors":0,"maxSuccessors":0}
            ]})");
    ASSERT_FALSE(r.has_value())
        << "non-string `terminatorKind` must reject loudly";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, TerminatorKindBrRequiresOneSuccessor) {
    // `br`-kinded opcode declaring (min=max=2) is contradictory.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"badbr","result":"none",
               "terminatorKind":"br",
               "minSuccessors":2,"maxSuccessors":2}
            ]})");
    ASSERT_FALSE(r.has_value())
        << "br-kinded opcode requires exactly 1 successor";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, TerminatorKindCondBrRequiresTwoSuccessors) {
    // Companion to the Br test — locks the `kTargetTerminatorShapes`
    // table's CondBr row. Test-analyzer rating 7 fold-now from cycle 3.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"badcondbr","result":"none",
               "terminatorKind":"cond-br",
               "minSuccessors":1,"maxSuccessors":1}
            ]})");
    ASSERT_FALSE(r.has_value())
        << "cond-br-kinded opcode requires exactly 2 successors";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, TerminatorKindSwitchAllowsArbitrarySuccessorCount) {
    // Positive control for the `Switch.maxSuccessors == 255`
    // (unbounded sentinel) in `kTargetTerminatorShapes`. Test-analyzer
    // rating 8 — the Switch arm had zero coverage. A future refactor
    // that drops the 255-sentinel handling would silently constrain
    // Switch arity; this test traps it.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"sw","result":"none",
               "terminatorKind":"switch",
               "minSuccessors":2,"maxSuccessors":8}
            ]})");
    ASSERT_TRUE(r.has_value())
        << "switch-kinded opcode with min=2, max=8 must validate (the "
           "shape table's Switch row is unbounded above min=2).";
}

TEST(TargetSchema, TerminatorKindSwitchRejectsSubMinimumSuccCount) {
    // Negative: Switch with min=1 (< 2) is degenerate by design.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"sw","result":"none",
               "terminatorKind":"switch",
               "minSuccessors":1,"maxSuccessors":3}
            ]})");
    ASSERT_FALSE(r.has_value())
        << "switch with minSuccessors<2 violates the shape table";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// ─── cycle 2b — register-file validate() rules ────────────────────────────

TEST(TargetSchema, RegisterWidthBytesZeroWhenClassedRejected) {
    // Silent-zero guard: a register with class:gpr but no widthBytes
    // (defaults to 0) would silently pass through to ML6 regalloc and
    // produce zero-byte spills. validate() must reject.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[{"name":"rax","class":"gpr"}]})");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, RegisterSubOfCycleRejected) {
    // subOf chain `a -> b -> a` is a cycle. validate() must trap it
    // before ML6 walks the chain.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[
              {"name":"a","class":"gpr","widthBytes":4,"subOf":"b"},
              {"name":"b","class":"gpr","widthBytes":4,"subOf":"a"}
            ]})");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// ─── cycle 2b — calling-convention validate() rules ───────────────────────

TEST(TargetSchema, CallingConventionWithoutRegistersIsRejected) {
    // CRITICAL silent-failure trap (silent-failure-hunter finding):
    // a config with NO `registers` but a fully-populated
    // `callingConventions` would previously have resolved nothing
    // silently. validate() must flag every reference.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "callingConventions":[
              {"name":"bogus","argGprs":["rdi"]}
            ]})");
    ASSERT_FALSE(r.has_value())
        << "callingConventions referencing names with no registers section "
           "must fail-loud (the gate is registers.empty() && cc.empty())";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, CallingConventionStackAlignmentMustBePow2) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[{"name":"rax","class":"gpr","widthBytes":8}],
            "callingConventions":[
              {"name":"bad","argGprs":["rax"],"stackAlignment":12}
            ]})");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, CallingConventionShadowSpaceMustAlignToStack) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[{"name":"rax","class":"gpr","widthBytes":8}],
            "callingConventions":[
              {"name":"bad","argGprs":["rax"],"stackAlignment":16,"shadowSpaceBytes":12}
            ]})");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, CallingConventionRedZoneMustAlignToStack) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[{"name":"rax","class":"gpr","widthBytes":8}],
            "callingConventions":[
              {"name":"bad","argGprs":["rax"],"stackAlignment":16,"redZoneBytes":100}
            ]})");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// ─── cycle 2b — JSON loader edge cases ────────────────────────────────────

TEST(TargetSchema, RegistersSectionMustBeArray) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":{"oops":"not-an-array"}})");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, RegisterInvalidClassStringRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[{"name":"r","class":"banana","widthBytes":4}]})");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, RegisterWidthAboveU16MaxRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[{"name":"r","class":"gpr","widthBytes":65536}]})");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, CallingConventionsSectionMustBeArray) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "callingConventions":42})");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, CallingConventionArgGprsMustBeStringArray) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "callingConventions":[{"name":"x","argGprs":[42]}]})");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, CallingConventionStackAlignmentMustBeInteger) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "callingConventions":[{"name":"x","stackAlignment":"sixteen"}]})");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// ─── ShippedX86_64 — exact-count assertions ───────────────────────────────

// ─── cycle 3a — abiModel ─────────────────────────────────────────────────

TEST(TargetSchema, AbiModelDefaultsToRegisterMachine) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}]})");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->abiModel(), ::dss::TargetAbiModel::RegisterMachine);
}

TEST(TargetSchema, AbiModelOperandStackAccepted) {
    // WASM-shape schema: operand-stack ABI with empty registers/cc
    // sections. Must load cleanly — the cycle-2b validate() short-circuit
    // is the back-compat hook for non-register-machine targets.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"wasm","abiModel":"operand-stack"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}]})");
    ASSERT_TRUE(r.has_value())
        << "operand-stack target with empty register/cc sections must load";
    EXPECT_EQ((*r)->abiModel(), ::dss::TargetAbiModel::OperandStack);
}

TEST(TargetSchema, AbiModelResultIdAccepted) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"spirv","abiModel":"result-id"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}]})");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->abiModel(), ::dss::TargetAbiModel::ResultId);
}

TEST(TargetSchema, AbiModelInvalidStringRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X","abiModel":"register-typo"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}]})");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, NonRegisterMachineWithCcStillValidatesRefs) {
    // Silent-failure-hunter finding: a non-register-machine target that
    // ships calling-convention entries anyway (copy-paste / leftover) must
    // still have its references resolved — otherwise typos hide in
    // unloadable-anyway data. Pin the failure mode.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"wasm","abiModel":"operand-stack"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "callingConventions":[{"name":"x","argGprs":["doesnotexist"]}]})");
    ASSERT_FALSE(r.has_value())
        << "non-register-machine target with populated cc must still validate "
           "its register references (closes the silent-acceptance trap)";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// ─── cycle 3a — linkRegister (ARM64 AAPCS64-shape) ──────────────────────

TEST(TargetSchema, LinkRegisterResolvesToDeclaredGpr) {
    // Fabricated ARM64-shape config: declare x30 as a GPR and reference it
    // as the link register. Positive control. Cycle 3b also pins the
    // load-time ordinal cache so ML7 callconv lowering doesn't re-resolve.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"arm64"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[
              {"name":"x0","class":"gpr","widthBytes":8},
              {"name":"sp","class":"gpr","widthBytes":8},
              {"name":"x30","class":"gpr","widthBytes":8}
            ],
            "callingConventions":[
              {"name":"aapcs64","argGprs":["x0"],"linkRegister":"x30",
               "stackPointer":"sp","stackAlignment":16}
            ]})");
    ASSERT_TRUE(r.has_value())
        << "linkRegister resolving to a declared GPR must validate";
    auto const* cc = (*r)->callingConventionByName("aapcs64");
    ASSERT_NE(cc, nullptr);
    ASSERT_TRUE(cc->linkRegister.has_value());
    EXPECT_EQ(cc->linkRegister->name, "x30");

    // Cycle 3b fold: ordinal is resolved at load time (atomic with the
    // name in the same struct) and matches `registerByName("x30")`.
    auto const expectedOrdinal = (*r)->registerByName("x30");
    ASSERT_TRUE(expectedOrdinal.has_value());
    EXPECT_EQ(cc->linkRegister->ordinal, *expectedOrdinal);
}

TEST(TargetSchema, LinkRegisterUnknownNameRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"arm64"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[{"name":"x0","class":"gpr","widthBytes":8}],
            "callingConventions":[
              {"name":"aapcs64","argGprs":["x0"],"linkRegister":"x999",
               "stackAlignment":16}
            ]})");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, LinkRegisterMustBeString) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"arm64"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[{"name":"x0","class":"gpr","widthBytes":8}],
            "callingConventions":[
              {"name":"aapcs64","linkRegister":42,"stackAlignment":16}
            ]})");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, ShippedX86_64ExactRegisterCount) {
    // 16 GPRs + 16 FPRs + rflags = 33. EXPECT_EQ (not EXPECT_GE) so a
    // future accidental duplicate / addition trips the test rather than
    // silently passing.
    auto r = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->registerCount(), 33u);
}
