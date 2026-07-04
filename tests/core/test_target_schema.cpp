#include "core/types/target_schema.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <set>
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
        EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_InvalidTargetName))
            << "name='" << name << "'";
    }
}

TEST(TargetSchema, LoadShippedReportsNotFoundForUnknownName) {
    auto r = TargetSchema::loadShipped("definitely_not_a_real_target_xyz");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_InvalidTargetName));
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

// ─── cycle 10p — implicit-register constraint substrate ───────────────
//
// The new `implicitRegisters` per-opcode block declares fixed-register
// semantic constraints (e.g., x86 idiv ties RDX:RAX). This cycle lands
// substrate-only: struct + loader + validator + tests. No opcode in
// the shipped schemas declares it yet (consumer wiring is cycle 10q).
// Each test pins one positive or negative path through the substrate
// so a future regression in the loader or validator surfaces here.

TEST(TargetSchema, ImplicitRegistersValidDeclarationLoads) {
    // Mirror idiv's contract: dividend in RAX/RDX (implicit input),
    // quotient in RAX (output), remainder in RDX (output), and both
    // are clobbered. Cross-array overlap IS legal — a register may
    // legitimately appear in inputs + outputs + clobbered (idiv's
    // RDX/RAX do). Cycle 10r 7-agent review fold F1 invariant:
    // every output must also appear in clobbered.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"fakediv","result":"value",
               "implicitRegisters":{
                 "inputs":["rax","rdx"],
                 "outputs":["rax","rdx"],
                 "clobbered":["rax","rdx"]
               }}
            ],
            "registers":[
              {"name":"rax","class":"gpr","widthBytes":8},
              {"name":"rdx","class":"gpr","widthBytes":8}
            ]})",
        "<inline>");
    ASSERT_TRUE(r.has_value()) << "valid implicitRegisters must load";
    auto const& sch = **r;
    auto const op = sch.opcodeByMnemonic("fakediv");
    ASSERT_TRUE(op.has_value());
    auto const* info = sch.opcodeInfo(*op);
    ASSERT_NE(info, nullptr);
    ASSERT_TRUE(info->implicitRegisters.has_value());
    auto const& ir = *info->implicitRegisters;
    EXPECT_EQ(ir.inputNames.size(),    2u);
    EXPECT_EQ(ir.outputNames.size(),   2u);
    EXPECT_EQ(ir.clobberedNames.size(), 2u);
    EXPECT_EQ(ir.inputNames[0],    "rax");
    EXPECT_EQ(ir.inputNames[1],    "rdx");
    EXPECT_EQ(ir.clobberedNames[0], "rax");
    EXPECT_EQ(ir.clobberedNames[1], "rdx");
    // Loader-populated ordinals parallel the names (consumer-O(1)
    // pin — regalloc reads ordinals, not names).
    EXPECT_EQ(ir.inputOrdinals.size(),    ir.inputNames.size());
    EXPECT_EQ(ir.outputOrdinals.size(),   ir.outputNames.size());
    EXPECT_EQ(ir.clobberedOrdinals.size(), ir.clobberedNames.size());
    // CG2 (7-agent fold FOLD-NOW): pin ordinal VALUES via schema's
    // own register-lookup — target-agnostic (no hardcoded "rax=0").
    // A regression in the loader that pushed `j` (index) instead of
    // `it->second` (registerIndex value) would pass the size-equality
    // pin but fail this value pin.
    auto const raxOrd = sch.registerByName("rax");
    auto const rdxOrd = sch.registerByName("rdx");
    ASSERT_TRUE(raxOrd.has_value());
    ASSERT_TRUE(rdxOrd.has_value());
    EXPECT_EQ(ir.inputOrdinals[0],    *raxOrd);
    EXPECT_EQ(ir.inputOrdinals[1],    *rdxOrd);
    EXPECT_EQ(ir.outputOrdinals[0],   *raxOrd);
    EXPECT_EQ(ir.outputOrdinals[1],   *rdxOrd);
    EXPECT_EQ(ir.clobberedOrdinals[0], *raxOrd);
    EXPECT_EQ(ir.clobberedOrdinals[1], *rdxOrd);
}

// ─── FC1 (V2-4.X, 2026-06-10): role-tagged projection maps ─────────────────
// (D-CSUBSET-MOD-OP-CODEGEN-OUTPUT-INDEX-CONTRACT closure substrate.)

TEST(TargetSchema, ImplicitRegisterRolesValidDeclarationLoadsAndResolves) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"fakediv","result":"value",
               "implicitRegisters":{
                 "inputs":["rax","rdx"],
                 "outputs":["rax","rdx"],
                 "clobbered":["rax","rdx"],
                 "inputRoles":  {"dividend":"rax"},
                 "outputRoles": {"quotient":"rax","remainder":"rdx"}
               }}
            ],
            "registers":[
              {"name":"rax","class":"gpr","widthBytes":8},
              {"name":"rdx","class":"gpr","widthBytes":8}
            ]})",
        "<inline>");
    ASSERT_TRUE(r.has_value()) << "valid role maps must load";
    auto const& sch = **r;
    auto const op = sch.opcodeByMnemonic("fakediv");
    ASSERT_TRUE(op.has_value());
    auto const* info = sch.opcodeInfo(*op);
    ASSERT_NE(info, nullptr);
    ASSERT_TRUE(info->implicitRegisters.has_value());
    auto const& ir = *info->implicitRegisters;
    auto const raxOrd = sch.registerByName("rax");
    auto const rdxOrd = sch.registerByName("rdx");
    ASSERT_TRUE(raxOrd.has_value() && rdxOrd.has_value());
    // The consumer-facing lookup helpers resolve role → ordinal.
    auto const dividend  = ir.inputOrdinalForRole("dividend");
    auto const quotient  = ir.outputOrdinalForRole("quotient");
    auto const remainder = ir.outputOrdinalForRole("remainder");
    ASSERT_TRUE(dividend.has_value());
    ASSERT_TRUE(quotient.has_value());
    ASSERT_TRUE(remainder.has_value());
    EXPECT_EQ(*dividend,  *raxOrd);
    EXPECT_EQ(*quotient,  *raxOrd);
    EXPECT_EQ(*remainder, *rdxOrd);
    // An undeclared role resolves to nothing (the lowering fail-louds
    // at its query site).
    EXPECT_FALSE(ir.outputOrdinalForRole("dividend").has_value());
    EXPECT_FALSE(ir.inputOrdinalForRole("quotient").has_value());
}

// FC3.5 sweep-c1: "count" joined the registered role vocabulary — the
// shift-count input of the implicit-count shift realization (x86 SHL/
// SHR/SAR read CL; the MIR→LIR shift lowering pins the count vreg by
// this role). A declaration must load + resolve; the SHIPPED x86_64
// schema must actually declare it on all three shift opcodes.
TEST(TargetSchema, ImplicitRegisterCountRoleLoadsAndShipsOnX64Shifts) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"fakeshift","result":"value",
               "implicitRegisters":{
                 "inputs":["rcx"],
                 "clobbered":["rcx"],
                 "inputRoles": {"count":"rcx"}
               }}
            ],
            "registers":[
              {"name":"rcx","class":"gpr","widthBytes":8}
            ]})",
        "<inline>");
    ASSERT_TRUE(r.has_value()) << "the count role must be registered";
    auto const op = (*r)->opcodeByMnemonic("fakeshift");
    ASSERT_TRUE(op.has_value());
    auto const* info = (*r)->opcodeInfo(*op);
    ASSERT_NE(info, nullptr);
    ASSERT_TRUE(info->implicitRegisters.has_value());
    EXPECT_TRUE(info->implicitRegisters
                    ->inputOrdinalForRole("count").has_value());

    auto shipped = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(shipped.has_value());
    auto const rcxOrd = (*shipped)->registerByName("rcx");
    ASSERT_TRUE(rcxOrd.has_value());
    for (auto const* mn : {"shl", "shr_l", "shr_a"}) {
        auto const sop = (*shipped)->opcodeByMnemonic(mn);
        ASSERT_TRUE(sop.has_value()) << mn;
        auto const* sinfo = (*shipped)->opcodeInfo(*sop);
        ASSERT_NE(sinfo, nullptr) << mn;
        ASSERT_TRUE(sinfo->implicitRegisters.has_value()) << mn;
        auto const ord =
            sinfo->implicitRegisters->inputOrdinalForRole("count");
        ASSERT_TRUE(ord.has_value()) << mn;
        EXPECT_EQ(*ord, *rcxOrd) << mn;
    }
}

// An UNREGISTERED role name ("remaindr" typo) must fail at LOAD —
// the lowering queries a registered vocabulary, so the typo would
// otherwise surface only as a confusing missing-role diagnostic at
// the first divide lowering.
TEST(TargetSchema, ImplicitRegisterRoleUnknownRoleNameRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"fakediv","result":"value",
               "implicitRegisters":{
                 "outputs":["rdx"],
                 "clobbered":["rdx"],
                 "outputRoles": {"remaindr":"rdx"}
               }}
            ],
            "registers":[
              {"name":"rdx","class":"gpr","widthBytes":8}
            ]})",
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "an unknown role name must be rejected at load (typo "
           "discriminator — registered roles are dividend/quotient/"
           "remainder).";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// A role naming a register that is NOT in the corresponding
// positional array is internally inconsistent → load reject.
TEST(TargetSchema, ImplicitRegisterRoleRegisterNotInPositionalArrayRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"fakediv","result":"value",
               "implicitRegisters":{
                 "outputs":["rax"],
                 "clobbered":["rax"],
                 "outputRoles": {"remainder":"rdx"}
               }}
            ],
            "registers":[
              {"name":"rax","class":"gpr","widthBytes":8},
              {"name":"rdx","class":"gpr","widthBytes":8}
            ]})",
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "a projection role must tag a register the op actually "
           "declares in the positional array.";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// The unknown-sub-key typo discriminator covers the NEW keys too:
// `outputRolez` must be rejected, not silently ignored (which would
// leave the lowering fail-louding on a 'missing' role the author
// believes is declared).
TEST(TargetSchema, ImplicitRegistersUnknownRoleMapKeyRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"fakediv","result":"value",
               "implicitRegisters":{
                 "outputs":["rdx"],
                 "clobbered":["rdx"],
                 "outputRolez": {"remainder":"rdx"}
               }}
            ],
            "registers":[
              {"name":"rdx","class":"gpr","widthBytes":8}
            ]})",
        "<inline>");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// D-TARGET-IMPLICIT-REGISTER-CONSTRAINT outputs ⊆ clobbered invariant
// (cycle 10r 7-agent review fold F1 CRITICAL 9/10, 2026-06-04). A
// schema declaring `outputs:[rax]` without `clobbered:[rax]` must
// FAIL LOUD at load time — every register the instruction writes is
// by definition clobbered for any prior live vreg in it; the regalloc
// consumes only `clobbered` (not `outputs`) to build its forbidden
// set. Missing this declaration would silently admit divisor vregs
// into a register the op is about to overwrite.
TEST(TargetSchema, ImplicitRegistersOutputMissingFromClobberedRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"badop","result":"value",
               "implicitRegisters":{
                 "outputs":["rax"],
                 "clobbered":[]
               }}
            ],
            "registers":[
              {"name":"rax","class":"gpr","widthBytes":8}
            ]})",
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "outputs without matching clobbered MUST fail loud — the "
           "schema-loader invariant catches the JSON-typo silent-"
           "miscompile class (e.g., dropping clobbered:[rdx] from "
           "xor_rdx_zero while keeping outputs:[rdx]).";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// II1 (7-agent fold FOLD-NOW): inputs-only positive shape — the
// canonical x86 shift-by-CL / cdq / cqo precedent. cdq for example
// has implicit input RAX → output RDX (sign-extended); the
// clobbered array stays empty (no separate destruction beyond the
// declared output). A regression that gated non-empty-block check
// on `inputs.empty() && outputs.empty()` (forgetting `clobbered`)
// or that REQUIRED all three arrays non-empty would fail this pin.
TEST(TargetSchema, ImplicitRegistersInputsOnlyShapeLoads) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"shl_cl","result":"value",
               "implicitRegisters":{"inputs":["rcx"]}}
            ],
            "registers":[
              {"name":"rcx","class":"gpr","widthBytes":8}
            ]})",
        "<inline>");
    ASSERT_TRUE(r.has_value()) << "inputs-only shape must load";
    auto const& sch = **r;
    auto const op = sch.opcodeByMnemonic("shl_cl");
    ASSERT_TRUE(op.has_value());
    auto const* info = sch.opcodeInfo(*op);
    ASSERT_NE(info, nullptr);
    ASSERT_TRUE(info->implicitRegisters.has_value());
    auto const& ir = *info->implicitRegisters;
    EXPECT_EQ(ir.inputNames.size(),     1u);
    EXPECT_TRUE(ir.outputNames.empty());
    EXPECT_TRUE(ir.clobberedNames.empty());
    EXPECT_TRUE(ir.outputOrdinals.empty());
    EXPECT_TRUE(ir.clobberedOrdinals.empty());
}

// CG1 (7-agent fold FOLD-NOW): loader-tier shape rejects.
// Three distinct emit sites in the loader — each needs its own
// negative-pin so a regression in any one of them surfaces
// independently.

TEST(TargetSchema, ImplicitRegistersNonObjectBlockRejected) {
    // `"implicitRegisters": "rax"` (string instead of object).
    // Classic copy-paste-from-`callerSaved`-style typo.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"fakediv","result":"value",
               "implicitRegisters":"rax"}
            ],
            "registers":[
              {"name":"rax","class":"gpr","widthBytes":8}
            ]})",
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "non-object implicitRegisters must fail-loud";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, ImplicitRegistersNonArrayInputsRejected) {
    // `"inputs": "rax"` (string instead of array).
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"fakediv","result":"value",
               "implicitRegisters":{"inputs":"rax"}}
            ],
            "registers":[
              {"name":"rax","class":"gpr","widthBytes":8}
            ]})",
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "non-array implicitRegisters.inputs must fail-loud";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, ImplicitRegistersNonStringEntryRejected) {
    // `"inputs": [42]` (number instead of register-name string).
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"fakediv","result":"value",
               "implicitRegisters":{"inputs":[42]}}
            ],
            "registers":[
              {"name":"rax","class":"gpr","widthBytes":8}
            ]})",
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "non-string entry in implicitRegisters.inputs must fail-loud";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// Unknown sub-key reject (per D-CONFIG-LOADER-UNKNOWN-KEYS-FAIL-LOUD
// discipline; 7-agent fold FOLD-NOW). A typo like `"inpts": [...]`
// pre-fold silently dropped the field, then the empty-block check
// fired with a misleading "typo discriminator" diagnostic. Post-
// fold the unknown-key check fires loud at the right path.
TEST(TargetSchema, ImplicitRegistersUnknownSubKeyRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"fakediv","result":"value",
               "implicitRegisters":{"inpts":["rax"]}}
            ],
            "registers":[
              {"name":"rax","class":"gpr","widthBytes":8}
            ]})",
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "typo'd sub-key 'inpts' must fail-loud (D-CONFIG-LOADER-"
           "UNKNOWN-KEYS-FAIL-LOUD discipline)";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, ImplicitRegistersUnknownNameRejected) {
    // `rxyz` is not in the register table → loud reject. Critical
    // for the substrate's silent-failure surface: a typo'd register
    // name would otherwise leave the regalloc consumer reading an
    // empty ordinal list and treating the opcode as constraint-free
    // — silent miscompile in the eventual regalloc wiring.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"fakediv","result":"value",
               "implicitRegisters":{"inputs":["rxyz"]}}
            ],
            "registers":[
              {"name":"rax","class":"gpr","widthBytes":8}
            ]})",
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "implicitRegisters.inputs=['rxyz'] must fail-loud — rxyz is "
           "not in the register table";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, ImplicitRegistersEmptyBlockRejected) {
    // A block with all three arrays empty (or with no arrays at all)
    // is structurally meaningless and almost certainly a typo
    // discriminator (the author intended to constrain something but
    // miswrote the keys, e.g. `implictInputs` instead of `inputs`).
    // Loud reject covers that class.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"fakediv","result":"value",
               "implicitRegisters":{}}
            ],
            "registers":[
              {"name":"rax","class":"gpr","widthBytes":8}
            ]})",
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "empty implicitRegisters block must fail-loud (typo "
           "discriminator pin)";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, ImplicitRegistersDuplicateWithinArrayRejected) {
    // Within-array duplicates are never intentional. Cross-array
    // overlap IS allowed (covered by ImplicitRegistersValidDeclaration
    // Loads). This negative pin guards the discrimination.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"fakediv","result":"value",
               "implicitRegisters":{"inputs":["rax","rax"]}}
            ],
            "registers":[
              {"name":"rax","class":"gpr","widthBytes":8}
            ]})",
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "duplicate register name within implicitRegisters.inputs "
           "must fail-loud";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, ImplicitRegistersOmissionLeavesNullopt) {
    // Most opcodes (mov / add / sub / ret / etc.) have no implicit-
    // register constraint. The pre-cycle invariant: every shipped
    // x86_64 opcode row omits the block; loading shipped schema is a
    // no-op pass through the new arm. Silent regression class: a
    // future bug that mis-defaults the optional to "empty constraint"
    // instead of nullopt would change behavior under future regalloc
    // consumers. Pin the default explicitly.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"mov","result":"value"}
            ]})",
        "<inline>");
    ASSERT_TRUE(r.has_value());
    auto const op = (*r)->opcodeByMnemonic("mov");
    ASSERT_TRUE(op.has_value());
    auto const* info = (*r)->opcodeInfo(*op);
    ASSERT_NE(info, nullptr);
    EXPECT_FALSE(info->implicitRegisters.has_value())
        << "opcodes omitting implicitRegisters must leave the optional "
           "nullopt — not auto-default to an empty constraint";
}

TEST(TargetSchema, ShippedX86_64ImplicitRegistersConsumerCount) {
    // Cycle 10p landed the substrate UNCONSUMED. Cycle 10q opened
    // consumption with sdiv_compound + udiv_compound; cycle 10r
    // SPLIT those into 4 separate opcodes (cqo + idiv_op + xor_rdx_zero
    // + div_op) to fix the REX-prefix overlap silent-miscompile (see
    // mir_to_lir.cpp lowerDiv $comment for the full diagnosis). All
    // four declare implicitRegisters. This test pins the consumer
    // count + names so:
    //   * future cycles adding new implicit-register-bearing opcodes
    //     (mul-1-op for 128-bit results / shift-by-CL / ARM64
    //     fixed-operand ops) update this list intentionally, not
    //     silently;
    //   * a regression that accidentally drops the implicit-register
    //     declaration during a JSON edit fails loud with attribution-
    //     clear "expected 4, got 3" diagnostic.
    auto r = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(r.has_value());
    auto const& sch = **r;
    std::vector<std::string> mnemonicsWithConstraint;
    for (std::uint16_t op = 0; op < sch.opcodeCount(); ++op) {
        auto const* info = sch.opcodeInfo(op);
        if (info && info->implicitRegisters.has_value()) {
            mnemonicsWithConstraint.push_back(info->mnemonic);
        }
    }
    // 7-agent review fold F4 (silent-failure 7/10, 2026-06-04):
    // unordered identity compare. Positional `[i] == "name"` style
    // gives muddy diagnostics on positional-swap regressions (e.g.,
    // dropping `xor_rdx_zero` at position 2 and adding `shl_cl` at
    // the end fails with a misleading "got div_op" at index 2). Use
    // a sorted set compare so "which one disappeared" reads cleanly.
    // FC3.5 sweep-c1: the anticipated "shift-by-CL" consumers landed —
    // shl/shr_l/shr_a each declare the implicit-count contract
    // (inputs=[rcx], inputRoles={count: rcx}).
    // c103 (D-CSUBSET-INTRINSIC-UMULH): the anticipated "mul-1-op for 128-bit
    // results" consumer landed — `umul_op` (MUL r/m64, 0xF7 /4) declares
    // inputs=[rax], outputs/clobbered=[rax,rdx], inputRoles={multiplicand: rax},
    // outputRoles={high: rdx, low: rax}; the __umulh lowering captures 'high'.
    ASSERT_EQ(mnemonicsWithConstraint.size(), 8u)
        << "expected exactly 8 implicit-register-bearing opcodes "
           "(cqo + idiv_op + xor_rdx_zero + div_op from cycle 10r "
           "split; shl + shr_l + shr_a from FC3.5 shifts; umul_op "
           "from c103 __umulh); update this count when a new "
           "consumer lands.";
    std::set<std::string> const observedSet(
        mnemonicsWithConstraint.begin(),
        mnemonicsWithConstraint.end());
    std::set<std::string> const expectedSet{
        "cqo", "idiv_op", "xor_rdx_zero", "div_op",
        "shl", "shr_l", "shr_a", "umul_op"};
    EXPECT_EQ(observedSet, expectedSet);

    // FLAG 1 discrimination: the four ops carry DIFFERENT implicit-
    // register profiles reflecting their distinct semantics.
    auto const cqoOp     = sch.opcodeByMnemonic("cqo");
    auto const idivOp    = sch.opcodeByMnemonic("idiv_op");
    auto const xorRdxOp  = sch.opcodeByMnemonic("xor_rdx_zero");
    auto const divOp     = sch.opcodeByMnemonic("div_op");
    ASSERT_TRUE(cqoOp.has_value());
    ASSERT_TRUE(idivOp.has_value());
    ASSERT_TRUE(xorRdxOp.has_value());
    ASSERT_TRUE(divOp.has_value());

    // CQO: reads RAX, writes RDX, clobbers RDX. RAX is unchanged.
    auto const* cqoInfo = sch.opcodeInfo(*cqoOp);
    ASSERT_TRUE(cqoInfo->implicitRegisters.has_value());
    EXPECT_EQ(cqoInfo->implicitRegisters->inputNames,
              (std::vector<std::string>{"rax"}));
    EXPECT_EQ(cqoInfo->implicitRegisters->outputNames,
              (std::vector<std::string>{"rdx"}));
    EXPECT_EQ(cqoInfo->implicitRegisters->clobberedNames,
              (std::vector<std::string>{"rdx"}));

    // IDIV /7: reads RDX:RAX, writes RDX:RAX, clobbers RDX:RAX.
    // The `clobbered=[rax,rdx]` is required by the `outputs ⊆
    // clobbered` schema-loader invariant added in cycle 10r 7-agent
    // review fold F1 — every implicit-output register is by
    // definition clobbered for any prior live vreg in it.
    auto const* idivInfo = sch.opcodeInfo(*idivOp);
    ASSERT_TRUE(idivInfo->implicitRegisters.has_value());
    EXPECT_EQ(idivInfo->implicitRegisters->inputNames,
              (std::vector<std::string>{"rax", "rdx"}));
    EXPECT_EQ(idivInfo->implicitRegisters->outputNames,
              (std::vector<std::string>{"rax", "rdx"}));
    EXPECT_EQ(idivInfo->implicitRegisters->clobberedNames,
              (std::vector<std::string>{"rax", "rdx"}));

    // XOR RDX,RDX: writes RDX (zero), clobbers RDX. No inputs.
    auto const* xorInfo = sch.opcodeInfo(*xorRdxOp);
    ASSERT_TRUE(xorInfo->implicitRegisters.has_value());
    EXPECT_TRUE(xorInfo->implicitRegisters->inputNames.empty())
        << "XOR RDX,RDX writes a constant zero; it does NOT depend on "
           "the prior RDX value.";
    EXPECT_EQ(xorInfo->implicitRegisters->outputNames,
              (std::vector<std::string>{"rdx"}));
    EXPECT_EQ(xorInfo->implicitRegisters->clobberedNames,
              (std::vector<std::string>{"rdx"}));

    // DIV /6: reads RDX:RAX, writes RDX:RAX, clobbers RDX. Same as
    // IDIV /7 at the register-profile level; distinction is byte-level.
    auto const* divInfo = sch.opcodeInfo(*divOp);
    ASSERT_TRUE(divInfo->implicitRegisters.has_value());
    EXPECT_EQ(divInfo->implicitRegisters->inputNames,
              idivInfo->implicitRegisters->inputNames)
        << "DIV /6 must declare the SAME implicit-input profile as "
           "IDIV /7 — both consume RDX:RAX. Distinction is the modrm "
           "/6 vs /7 byte, not the register profile.";
    EXPECT_EQ(divInfo->implicitRegisters->outputNames,
              idivInfo->implicitRegisters->outputNames);
}

// D-CSUBSET-DIVISION-OP-CODEGEN anti-regression pin (cycle 10r
// 7-agent review fold pr-test #2 8/10, 2026-06-04). The cycle-10q
// compound mnemonics (`sdiv_compound`, `udiv_compound`) packed
// `[REX.W 0x99, REX.W 0xF7 /7]` (and the unsigned analog) into a
// single opcode whose embedded second 0x48 OVERRODE the encoder's
// auto-REX prefix — losing REX.B for high-reg divisors and silently
// miscompiling. Cycle 10r split each compound into pre + core
// opcodes so each instruction's REX is auto-computed correctly.
//
// **Why this anti-regression test matters**: nothing else in the
// suite directly asserts the OLD names are absent. A merge that
// reintroduces the compound rows (e.g., from a stale branch, a
// partial revert, or a wrong cherry-pick) would silently re-enable
// both encodings — the new split tests would still pass, the byte
// pins would still pass, and the silent-miscompile would return.
// This test makes any such reintroduction fail loud immediately.
TEST(TargetSchema, ShippedX86_64HasNoLegacyCompoundDivideOpcodes) {
    auto r = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(r.has_value());
    auto const& sch = **r;
    EXPECT_FALSE(sch.opcodeByMnemonic("sdiv_compound").has_value())
        << "the cycle-10q compound `sdiv_compound` mnemonic must NOT "
           "be present in the shipped schema — cycle 10r split it into "
           "`cqo` + `idiv_op` to fix the REX-overlap silent-miscompile.";
    EXPECT_FALSE(sch.opcodeByMnemonic("udiv_compound").has_value())
        << "the cycle-10q compound `udiv_compound` mnemonic must NOT "
           "be present in the shipped schema — cycle 10r split it into "
           "`xor_rdx_zero` + `div_op`.";
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

    // D-LK10-ENTRY-TRAMP-PROLOGUE: shipped entry-stack-pointer-bias
    // values must match the OS-loader convention for each cc.
    // Regression to either would silently mis-emit the trampoline
    // prologue (caught end-to-end by Slice C's runnable smoke on
    // Windows, but byte-pin here catches cross-host CI before the
    // smoke runs).
    EXPECT_EQ(sysv->entryStackPointerBias, 0)
        << "SysV kernel JUMPs to _start with RSP 16-aligned and no "
           "return address — bias must be 0";
    EXPECT_EQ(msx64->entryStackPointerBias, 8)
        << "Win64 RtlUserThreadStart CALLs the entry point — bias "
           "must be 8 (RSP ≡ 8 mod 16 at the first trampoline op)";
}

TEST(TargetSchema, EntryStackPointerBiasGreaterThanOrEqualAlignmentRejected) {
    // D-LK10-ENTRY-TRAMP-PROLOGUE validator: the bias is an offset
    // INTO the stackAlignment quantum and MUST be strictly less
    // than it. A bias equal to (or greater than) alignment would
    // denote a full alignment cycle (== 0) or be nonsense — fail
    // loud at schema-load rather than silently producing the wrong
    // adjust at the trampoline-emit site.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[{"name":"rsp","class":"gpr"}],
            "callingConventions":[
              {"name":"bad","argGprs":["rsp"],
               "stackAlignment":16,"entryStackPointerBias":16}
            ]})");
    EXPECT_FALSE(r.has_value());
    if (!r.has_value()) {
        bool sawBiasMsg = false;
        for (auto const& d : r.error()) {
            if (d.message.find("entryStackPointerBias") != std::string::npos) {
                sawBiasMsg = true;
                break;
            }
        }
        EXPECT_TRUE(sawBiasMsg)
            << "validator must surface entryStackPointerBias in the "
               "diagnostic so the schema author can triage";
    }
}

TEST(TargetSchema, CallPushBytesGreaterThanOrEqualAlignmentRejected) {
    // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY validator: callPushBytes is
    // the ISA-level CALL-instruction RSP push width, used by ML7
    // `computeFrameLayout` as the post-CALL alignment bias for non-
    // leaf functions. Must be strictly less than stackAlignment —
    // same invariant as entryStackPointerBias (the bias is an offset
    // INTO the alignment quantum; equal-to-alignment would denote a
    // full cycle, semantically zero but expressed wrong).
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[{"name":"rsp","class":"gpr"}],
            "callingConventions":[
              {"name":"bad","argGprs":["rsp"],
               "stackAlignment":16,"callPushBytes":16}
            ]})");
    EXPECT_FALSE(r.has_value());
    if (!r.has_value()) {
        bool sawCallPushMsg = false;
        for (auto const& d : r.error()) {
            if (d.message.find("callPushBytes") != std::string::npos) {
                sawCallPushMsg = true;
                break;
            }
        }
        EXPECT_TRUE(sawCallPushMsg)
            << "validator must surface callPushBytes in the "
               "diagnostic so the schema author can triage";
    }
}

TEST(TargetSchema, CallPushBytesStrictlyGreaterThanAlignmentRejected) {
    // The strict-less-than invariant must also reject callPushBytes
    // values STRICTLY greater than stackAlignment (not just equal-
    // to-alignment). A value like 24 with alignment 16 is doubly
    // wrong: it implies the CALL pushed more than one alignment
    // quantum, which no register-machine ISA does.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[{"name":"rsp","class":"gpr"}],
            "callingConventions":[
              {"name":"bad","argGprs":["rsp"],
               "stackAlignment":16,"callPushBytes":24}
            ]})");
    EXPECT_FALSE(r.has_value());
    if (!r.has_value()) {
        bool sawCallPushMsg = false;
        for (auto const& d : r.error()) {
            if (d.message.find("callPushBytes") != std::string::npos) {
                sawCallPushMsg = true;
                break;
            }
        }
        EXPECT_TRUE(sawCallPushMsg);
    }
}

TEST(TargetSchema, CallPushBytesShippedX8664SysVDeclaresEight) {
    // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY happy-path pin: shipped
    // x86_64.target.json declares callPushBytes=8 on sysv_amd64
    // (x86_64 CALL pushes 8-byte return address — ISA fact, same
    // for SysV and Win64). Without this declaration, ML7's frame
    // formula degenerates back to the pre-fold alignUp shape and
    // the hello_puts SEGV class returns.
    auto r = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(r.has_value());
    auto const* sysvCc = (*r)->callingConvention(0);
    ASSERT_NE(sysvCc, nullptr);
    EXPECT_STREQ(sysvCc->name.c_str(), "sysv_amd64");
    EXPECT_EQ(sysvCc->callPushBytes, 8)
        << "x86_64 CALL pushes 8-byte return address — ISA fact, "
           "must be declared on sysv_amd64";
    auto const* msx64Cc = (*r)->callingConvention(1);
    ASSERT_NE(msx64Cc, nullptr);
    EXPECT_STREQ(msx64Cc->name.c_str(), "ms_x64");
    EXPECT_EQ(msx64Cc->callPushBytes, 8)
        << "x86_64 CALL pushes 8-byte return address regardless of "
           "OS — ms_x64 coincides with sysv_amd64 on this ISA fact "
           "(diverges on entryStackPointerBias)";
}

TEST(TargetSchema, SlotAlignedShippedMsX64IsTrueOthersFalse) {
    // D-ML7-2.6 (closed co-with-D-ML7-2.2, 2026-06-02): the shipped
    // schemas must declare `slotAligned: true` on ms_x64 (the only
    // SLOT-ALIGNED cc DSS supports today) and leave it false elsewhere.
    // A schema regression that silently flipped this would: (a) flip
    // SysV's arg-passing semantics from independent counters to
    // slot-aligned (wrong code for mixed int/float calls); (b) flip
    // Win64's mixed int/float overflow to count by independent
    // counters (wrong code for the same case in reverse).
    auto x86 = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(x86.has_value());
    auto const* sysv = (*x86)->callingConvention(0);
    ASSERT_NE(sysv, nullptr);
    EXPECT_STREQ(sysv->name.c_str(), "sysv_amd64");
    EXPECT_FALSE(sysv->slotAligned)
        << "sysv_amd64 uses independent per-class arg counters";
    auto const* msx64 = (*x86)->callingConvention(1);
    ASSERT_NE(msx64, nullptr);
    EXPECT_STREQ(msx64->name.c_str(), "ms_x64");
    EXPECT_TRUE(msx64->slotAligned)
        << "ms_x64 uses Win64 SLOT-ALIGNED arg passing — slot N "
           "consumes both argGprs[N] AND argFprs[N]";

    auto arm = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(arm.has_value());
    auto const* aapcs = (*arm)->callingConvention(0);
    ASSERT_NE(aapcs, nullptr);
    EXPECT_STREQ(aapcs->name.c_str(), "aapcs64");
    EXPECT_FALSE(aapcs->slotAligned)
        << "AAPCS64 uses independent per-class arg counters (x0..x7 "
           "for integers, v0..v7 for floats; separate pools)";
}

TEST(TargetSchema, SlotAlignedRejectsNonBoolean) {
    // D-ML7-2.6 validator: the slotAligned field must be a JSON
    // boolean. A non-boolean value (string "true", number 1, null,
    // etc.) is rejected loud with C_MalformedJson — matches the
    // sibling fields' (`isCall`, `pcRelative`, `rexW`) discipline.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[{"name":"rsp","class":"gpr"}],
            "callingConventions":[
              {"name":"bad","argGprs":["rsp"],
               "stackAlignment":16,"callPushBytes":8,
               "slotAligned":"true"}
            ]})");
    EXPECT_FALSE(r.has_value());
    if (!r.has_value()) {
        bool sawSlotAlignedMsg = false;
        for (auto const& d : r.error()) {
            if (d.message.find("slotAligned") != std::string::npos) {
                sawSlotAlignedMsg = true;
                break;
            }
        }
        EXPECT_TRUE(sawSlotAlignedMsg)
            << "loader must surface slotAligned in the diagnostic "
               "for triage";
    }
}

TEST(TargetSchema, CallPushBytesShippedAArch64DeclaresZero) {
    // ARM64 BL writes LR/x30 — no stack push. callPushBytes=0.
    auto r = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(r.has_value());
    auto const* cc = (*r)->callingConvention(0);
    ASSERT_NE(cc, nullptr);
    EXPECT_STREQ(cc->name.c_str(), "aapcs64");
    EXPECT_EQ(cc->callPushBytes, 0)
        << "ARM64 BL writes LR — no stack push, callPushBytes must "
           "be 0 (the non-leaf x30-save lands in savedRegAreaSize "
           "via callee-saved tracking, independent of this bias).";
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

// FC7 C2 (review LOW-2): a CC declaring a REAL aggregate-classification strategy
// but no (zero / omitted) `aggregateMaxRegBytes` would silently classify EVERY
// by-value struct by-reference (`size <= 0` is always false) — validate() must
// reject it so a future target.json that wires the strategy but forgets the
// register budget is caught instead of quietly mis-passing every aggregate.
TEST(TargetSchema, CallingConventionAggregateStrategyNeedsNonZeroMaxRegBytes) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "registers":[{"name":"rax","class":"gpr","widthBytes":8}],
            "callingConventions":[
              {"name":"bad","argGprs":["rax"],"stackAlignment":16,
               "aggregateClassification":"sysv_eightbyte"}
            ]})");
    ASSERT_FALSE(r.has_value())
        << "a real aggregateClassification with aggregateMaxRegBytes=0 must fail-loud";
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
