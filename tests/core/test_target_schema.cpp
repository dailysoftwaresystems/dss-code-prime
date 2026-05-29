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
