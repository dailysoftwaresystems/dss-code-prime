#include "core/types/target_schema.hpp"

// `findShippedConfig` / `ShippedConfigLocator` — the SAME resolver
// `TargetSchema::loadShipped` uses, so the content-digest fixture below reads
// the exact file the compiler would have read.
#include "core/types/config_path_walk.hpp"
// The repo's SHA-256 — the independent oracle the retained `contentDigest()`
// is pinned against (the tests hex-render it themselves; see `hexOracle`).
#include "core/crypto/sha256.hpp"

#include "mutate_target_schema.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <ios>
#include <nlohmann/json.hpp>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

using ::dss::AsmConstraintBinding;
using ::dss::DiagnosticCode;
using ::dss::OperandKindFilter;
using ::dss::TargetRegClass;
using ::dss::TargetSchema;
using ::dss::EncodingSlotKind;
using ::dss::encodingSlotKindName;
using ::dss::encodingSlotKindFromName;
using ::dss::isSymbolBearingSlot;
using ::dss::slotShapeFor;

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
    // c104 (D-CSUBSET-INTRINSIC-ATOMIC-CAS): `lock_cmpxchg` (LOCK 0F B1 /r)
    // declares inputs/outputs/clobbered=[rax], inputRoles={comparand: rax},
    // outputRoles={old: rax} — the AtomicCas lowering pins the comparand into
    // RAX and captures the observed-original from it.
    // `rdtsc`: the tenth consumer, and it landed exactly as this pin was
    // written to make it land — the ratchet fired, the new opcode was named
    // here on purpose, and neither half of the assertion was loosened. It
    // declares outputs/clobbered=[rax,rdx] with outputRoles={low: rax,
    // high: rdx}, REUSING the high/low roles c103 added for the 128-bit MUL
    // rather than minting tsc-specific ones: both instructions split one wide
    // value across two registers, which is exactly what the roles mean. The
    // roles are what stop a reorder of the positional arrays from silently
    // swapping RDTSC's halves — a swap that multiplies every measured
    // interval by 2^32 while still producing a plausible-looking number.
    ASSERT_EQ(mnemonicsWithConstraint.size(), 10u)
        << "expected exactly 10 implicit-register-bearing opcodes "
           "(cqo + idiv_op + xor_rdx_zero + div_op from cycle 10r "
           "split; shl + shr_l + shr_a from FC3.5 shifts; umul_op "
           "from c103 __umulh; lock_cmpxchg from c104 AtomicCas; "
           "rdtsc from the inline-asm constraint cycle); "
           "update this count when a new consumer lands.";
    std::set<std::string> const observedSet(
        mnemonicsWithConstraint.begin(),
        mnemonicsWithConstraint.end());
    std::set<std::string> const expectedSet{
        "cqo", "idiv_op", "xor_rdx_zero", "div_op",
        "shl", "shr_l", "shr_a", "umul_op", "lock_cmpxchg", "rdtsc"};
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
              {"mnemonic":"jmp","result":"none",
               "terminatorKind":"cond-br",
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
              {"mnemonic":"jmp","result":"none",
               "terminatorKind":"cond-br",
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
              {"mnemonic":"mov","result":"value",
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
    // (The stray legacy `isTerminator` key this fixture used to carry was
    // incidental scaffolding, not the property under test, and it is now a
    // load-time reject — see RetiredIsTerminatorKeyIsRefusedNotIgnored. The
    // pin still asserts exactly what it protected: the RETURN SHAPE.)
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"ret","result":"none",
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
    // `terminatorKind != none`.
    //
    // ★ THIS PIN WAS STRENGTHENED, NOT WEAKENED, by the opcode-row key gate
    // (D-CONFIG-TARGET-LOADER-CONTAINER-KEYS-UNGATED). It used to assert that
    // a stray legacy `isTerminator` key was SILENTLY IGNORED — which is the
    // sharpest form of the very hazard that gate closes: `isTerminator` is a
    // spelling that USED TO WORK, so an author who writes it and omits
    // `terminatorKind` has every reason to believe they declared a terminator
    // and gets a NON-terminator with no diagnostic. What this pin PROTECTED
    // is the DERIVATION ("terminator-ness comes from `terminatorKind`"), and
    // refusing the retired key preserves that derivation while removing the
    // trap. Half 2 is the sibling test below.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"add","result":"value",
               "minOperands":2,"maxOperands":2}
            ]})");
    ASSERT_TRUE(r.has_value())
        << "terminator-ness derives from `terminatorKind` (default `none` = "
           "non-terminator).";
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
    // 16 GPRs + 16 FPRs + rflags + the 16 32-bit GPR views (eax..r15d)
    // + the 16 8-bit views (al..r15b) = 65.
    // EXPECT_EQ (not EXPECT_GE) so a future accidental duplicate / addition
    // trips the test rather than silently passing.
    //
    // ★ THE PIN GAINED A COMPOSITION CHECK WHEN THE 32-BIT VIEWS LANDED
    // (2026-08-13), rather than just a new total. What it protects is "no
    // register appears here by accident", and a bare total re-cut to fit each
    // new row would keep saying less every time it fired: 49 is also reachable
    // by, say, 17 GPRs + 32 sub-registers. Asserting each BUCKET keeps the
    // guard at least as strong after the change as before it — a swapped class,
    // a sub-register that forgot its `subOf`, or a full-width register that
    // grew one, all trip it now and none of them tripped the total.
    auto r = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->registerCount(), 65u);

    // ★★ THE COMPOSITION IS KEYED ON (class, sub-ness, WIDTH), NOT JUST
    // (class, sub-ness) — extended when the 8-bit views landed
    // (D-ASM-X86-NO-8BIT-REGISTER-FILE, 2026-08-13). A bucket that lumped every
    // `subOf` GPR together would have gone from "16" to "32" and said nothing
    // about WHICH 32; a pin has to be at least as strong after each change as
    // it was before, or it is the guard-weakened-by-its-own-subject shape this
    // tree has already been bitten by twice. With the width axis in, a byte
    // view that forgot its `subOf`, a 32-bit view that grew a byte width, and a
    // duplicated file at either width each trip a DIFFERENT expectation than
    // the bare total does.
    std::size_t fullGpr = 0, subGpr32 = 0, subGpr8 = 0, subGprOther = 0;
    std::size_t fpr = 0, flags = 0, other = 0;
    for (auto const& info : (*r)->registers()) {
        bool const sub = !info.subOf.empty();
        switch (info.regClass) {
            case TargetRegClass::GPR:
                if (!sub) { ++fullGpr; break; }
                if (info.widthBytes == 4)      ++subGpr32;
                else if (info.widthBytes == 1) ++subGpr8;
                else                           ++subGprOther;
                break;
            case TargetRegClass::FPR:   sub ? ++other : ++fpr;      break;
            case TargetRegClass::Flags: sub ? ++other : ++flags;    break;
            default:                    ++other;                    break;
        }
    }
    EXPECT_EQ(fullGpr,  16u) << "rax..r15";
    EXPECT_EQ(subGpr32, 16u) << "eax..r15d — the 32-bit views, `subOf` their "
                                "64-bit parent";
    EXPECT_EQ(subGpr8,  16u) << "al..r15b — the 8-bit views; without them "
                                "gas's `sete %al` is unspellable and the only "
                                "form DSS could spell (`sete %rax`) is one gas "
                                "rejects";
    EXPECT_EQ(subGprOther, 0u) << "no 16-bit (ax..r15w) file is declared — no "
                                  "consumer writes one yet";
    EXPECT_EQ(fpr,      16u) << "xmm0..xmm15";
    EXPECT_EQ(flags,     1u) << "rflags";
    EXPECT_EQ(other,     0u);
}

// D-CSUBSET-LONG-DOUBLE-IEEE128-ARITH (LD-2): the wideFloatSoftcalls table
// parses + resolves arg/result register NAMES to ordinals, and an unresolvable
// name fails loud.
TEST(TargetSchema, WideFloatSoftcallSchemaParsesArgAndResultRegisters) {
    // Positive: the shipped arm64 schema declares the __addtf3 softcall with
    // args v0/v1 and result v0, resolved to valid register ordinals.
    auto r = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(r.has_value());
    auto const* add = (*r)->wideFloatSoftcall(::dss::WideFloatOp::Add);
    ASSERT_NE(add, nullptr) << "arm64 must declare an F128 `add` softcall row";
    EXPECT_EQ(add->helperSymbol, "__addtf3");
    ASSERT_EQ(add->argRegisterOrdinals.size(), 2u);
    // The arg ordinals resolve to the v0/v1 registers.
    auto const v0 = (*r)->registerByName("v0");
    auto const v1 = (*r)->registerByName("v1");
    ASSERT_TRUE(v0.has_value() && v1.has_value());
    EXPECT_EQ(add->argRegisterOrdinals[0], *v0);
    EXPECT_EQ(add->argRegisterOrdinals[1], *v1);
    EXPECT_EQ(add->resultRegisterOrdinal, *v0);
    // Cross-check the ordinal actually names v0/v1 (VR-class, 128-bit).
    auto const* v0Info = (*r)->registerInfo(add->argRegisterOrdinals[0]);
    ASSERT_NE(v0Info, nullptr);
    EXPECT_EQ(v0Info->name, "v0");
    EXPECT_EQ(v0Info->regClass, TargetRegClass::VR);
    EXPECT_EQ(v0Info->widthBytes, 16u);
    // The from_f64 row marshals its double source through d0 (an FPR register),
    // proving the table is register-class-agnostic (not v-register-only).
    auto const* fromF64 = (*r)->wideFloatSoftcall(::dss::WideFloatOp::FromFloat64);
    ASSERT_NE(fromF64, nullptr);
    ASSERT_EQ(fromF64->argRegisterOrdinals.size(), 1u);
    auto const* d0Info = (*r)->registerInfo(fromF64->argRegisterOrdinals[0]);
    ASSERT_NE(d0Info, nullptr);
    EXPECT_EQ(d0Info->name, "d0");
    EXPECT_EQ(d0Info->regClass, TargetRegClass::FPR);

    // Negative: a softcall naming an UNRESOLVABLE register must fail loud.
    auto bad = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"},
                       {"mnemonic":"mov","result":"value"}],
            "registers":[{"name":"v0","class":"vr","widthBytes":16,"hwEncoding":0}],
            "wideFloatSoftcalls":[
              {"op":"add","helperSymbol":"__addtf3",
               "argRegisters":["v0","vX"],"resultRegister":"v0"}
            ]})",
        "<inline>");
    ASSERT_FALSE(bad.has_value())
        << "a softcall arg naming an undeclared register must fail loud";
    EXPECT_TRUE(anyHasCode(bad.error(), DiagnosticCode::C_MalformedJson));

    // Negative: an unknown op name must also fail loud.
    auto badOp = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"},
                       {"mnemonic":"mov","result":"value"}],
            "registers":[{"name":"v0","class":"vr","widthBytes":16,"hwEncoding":0}],
            "wideFloatSoftcalls":[
              {"op":"bogus","helperSymbol":"__x","argRegisters":["v0"],
               "resultRegister":"v0"}
            ]})",
        "<inline>");
    ASSERT_FALSE(badOp.has_value());
    EXPECT_TRUE(anyHasCode(badOp.error(), DiagnosticCode::C_MalformedJson));
}

// D-CSUBSET-LONG-DOUBLE-AGGREGATE-ABI (LD-4): the argVrs/returnVrs calling-
// convention lists (the AAPCS64 binary128 arg/return VR registers) parse +
// resolve like argFprs, are validated VR-class, and a mis-classed name fails
// loud. Mirrors the LD-2 wideFloatSoftcall schema pin.
TEST(TargetSchema, ArgVrsReturnVrsParseResolveAndValidateVrClass) {
    // Positive: the shipped arm64 aapcs64 cc (index 0) declares v0..v7 args +
    // v0..v3 returns, all resolving to VR-class 128-bit registers.
    auto r = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(r.has_value());
    ASSERT_FALSE((*r)->callingConventions().empty());
    auto const& cc = (*r)->callingConventions()[0];
    EXPECT_EQ(cc.name, "aapcs64");
    ASSERT_EQ(cc.argVrs.size(), 8u);
    ASSERT_EQ(cc.returnVrs.size(), 4u);
    for (std::size_t k = 0; k < cc.argVrs.size(); ++k) {
        EXPECT_EQ(cc.argVrs[k], std::string("v") + std::to_string(k));
        auto const ord = (*r)->registerByName(cc.argVrs[k]);
        ASSERT_TRUE(ord.has_value()) << cc.argVrs[k];
        auto const* info = (*r)->registerInfo(*ord);
        ASSERT_NE(info, nullptr);
        EXPECT_EQ(info->regClass, TargetRegClass::VR);
        EXPECT_EQ(info->widthBytes, 16u);
    }
    EXPECT_EQ(cc.returnVrs[0], "v0");

    // Negative: an argVrs naming a NON-VR register (a GPR) must fail the
    // VR-class validation (C_MalformedJson), exactly as argFprs rejects a
    // non-FPR name — a mis-classed boundary register is a silent wrong-file move.
    auto bad = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"},
                       {"mnemonic":"mov","result":"value"}],
            "registers":[{"name":"x0","class":"gpr","widthBytes":8,"hwEncoding":0},
                         {"name":"v0","class":"vr","widthBytes":16,"hwEncoding":0}],
            "callingConventions":[
              {"name":"cc","argVrs":["x0"],"returnVrs":["v0"],"stackAlignment":16}
            ]})",
        "<inline>");
    ASSERT_FALSE(bad.has_value())
        << "an argVrs naming a GPR (non-VR) register must fail loud";
    EXPECT_TRUE(anyHasCode(bad.error(), DiagnosticCode::C_MalformedJson));
}

// ─────────────────────────────────────────────────────────────────────────────
// TF-C74 — per-ARCHITECTURE identity predefined macros (`predefinedMacros`)
// + the target family's closed root-key vocabulary.
//
// The macros that tell the preprocessor which CPU it is compiling for live on
// the TARGET, next to the other per-target language-affecting semantics
// (`charIsUnsigned`, `aggregateLayout`, `tls`, `callingConventions`) — putting
// them on the language would force `c-subset.lang.json` to enumerate CPU
// architectures. Entry grammar is the SHARED parser the language loader uses.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// One shipped predefine row, flattened for exact-set comparison. Comparing the
// WHOLE row (name + kind + value + format gate) in ORDER is the point: a count
// would pass if `__arm64__` silently lost its ["macho"] gate and started
// leaking an Apple-only spelling onto the ELF leg.
struct PredefineRow {
    std::string_view              name;
    ::dss::PredefinedMacroKind    kind;
    std::string_view              value;
    std::vector<std::string_view> formats;   // empty ⇒ ungated (every format)

    bool operator==(PredefineRow const&) const = default;
};

[[nodiscard]] std::vector<PredefineRow> rowsOf(::dss::TargetSchema const& t) {
    std::vector<PredefineRow> out;
    for (auto const& pm : t.predefinedMacros()) {
        PredefineRow r{pm.name, pm.kind, pm.value, {}};
        for (auto const& f : pm.availableObjectFormats) r.formats.push_back(f);
        out.push_back(std::move(r));
    }
    return out;
}

}  // namespace

// EXACT SET, in declaration order — names, kinds, values AND format gates.
//
// MEASURED 2026-07-28 with `clang -dM -E -x c /dev/null -target <triple>`:
//   arm64-apple-darwin  defines __aarch64__ __ARM_ARCH_ISA_A64 __arm64__ __arm64
//   aarch64-linux-gnu   defines __aarch64__ __ARM_ARCH_ISA_A64   (NO __arm64*)
// So `__arm64__`/`__arm64` are APPLE-ONLY and MUST carry ["macho"]. Shipping
// them ungated leaks an Apple spelling onto ELF; shipping only `__aarch64__`
// clears nothing on macOS, whose SDK arch ladders gate on `__arm64__`.
// RED-ON-DISABLE: drop the gate from arm64.target.json and the format-set
// comparison fails.
TEST(TargetSchema, TFC74Arm64PredefinedMacrosExactSet) {
    auto r = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(r.has_value());
    using K = ::dss::PredefinedMacroKind;
    EXPECT_EQ(rowsOf(**r),
              (std::vector<PredefineRow>{
                  {"__aarch64__",        K::Constant, "1", {}},
                  {"__ARM_ARCH_ISA_A64", K::Constant, "1", {}},
                  {"__arm64__",          K::Constant, "1", {"macho"}},
                  {"__arm64",            K::Constant, "1", {"macho"}},
                  // TF-C75: NOT an identity spelling — the PREPROCESSOR face of
                  // this file's `charIsUnsigned` key, gated to exactly the leg
                  // where that key's `default` (true) is the effective answer.
                  // The macho/pe `byObjectFormat` overrides make bare `char`
                  // SIGNED there, so the macro must NOT appear on those legs.
                  // MEASURED 2026-07-28, `/usr/bin/clang -dM -E`: defined for
                  // aarch64-linux-gnu only.
                  {"__CHAR_UNSIGNED__",  K::Constant, "1", {"elf"}},
                  // TF-C115 (D-PP-ENDIANNESS-PREDEFINES): the per-CPU byte-order
                  // ANSWER. UNGATED — MEASURED 2026-08-04 (`clang-19 -dM -E -x c
                  // /dev/null -target <triple>`), __LITTLE_ENDIAN__ is 1 on
                  // arm64-apple-darwin AND aarch64-linux-gnu (and every other
                  // triple DSS targets), so unlike __arm64__ it is NOT an
                  // Apple-only spelling. __BYTE_ORDER__'s value is a MACRO
                  // REFERENCE, not a literal: it names __ORDER_LITTLE_ENDIAN__,
                  // which is declared on the LANGUAGE because it is invariant
                  // across every triple including the big-endian control.
                  // ★ __BIG_ENDIAN__ MUST NOT APPEAR IN THIS LIST — MEASURED, it
                  // is defined only on a big-endian triple (aarch64_be-linux-gnu),
                  // and Apple's libkern/OSByteOrder.h:165 tests it BEFORE the
                  // little-endian arm, so a stray row silently selects
                  // byte-swapping macros. This exact-set comparison is what keeps
                  // it out.
                  {"__LITTLE_ENDIAN__",  K::Constant, "1", {}},
                  {"__BYTE_ORDER__",     K::Constant, "__ORDER_LITTLE_ENDIAN__", {}},
              }))
        << "arm64 must predefine the two UNIVERSAL AArch64 spellings ungated, "
           "the two APPLE-ONLY spellings gated to macho, __CHAR_UNSIGNED__ "
           "gated to elf — the one row whose gate is an ABI property rather "
           "than a vendor spelling — and the two UNGATED endianness rows, with "
           "NO __BIG_ENDIAN__ anywhere";
}

// The x86_64 twin: MEASURED identical on x86_64-linux-gnu, x86_64-apple-darwin
// AND x86_64-pc-windows-msvc, so all four spellings are UNGATED. This test
// pinning EMPTY format sets is what keeps someone from "symmetrically" adding
// a gate here by analogy with arm64 — the asymmetry is a real toolchain fact.
TEST(TargetSchema, TFC74X86_64PredefinedMacrosExactSet) {
    auto r = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(r.has_value());
    using K = ::dss::PredefinedMacroKind;
    EXPECT_EQ(rowsOf(**r),
              (std::vector<PredefineRow>{
                  {"__x86_64__", K::Constant, "1", {}},
                  {"__x86_64",   K::Constant, "1", {}},
                  {"__amd64__",  K::Constant, "1", {}},
                  {"__amd64",    K::Constant, "1", {}},
                  // TF-C115 (D-PP-ENDIANNESS-PREDEFINES): the arm64 twin's rows,
                  // identical here because endianness is a per-CPU fact and
                  // x86_64 is little-endian under elf64, macho64 AND pe64 —
                  // which is exactly the test __LP64__ FAILED (LP64 on
                  // elf/macho, LLP64 on pe), sending it to the object format.
                  // MEASURED 2026-08-04: __LITTLE_ENDIAN__ 1 on
                  // x86_64-unknown-linux-gnu, x86_64-apple-darwin AND
                  // x86_64-pc-windows-msvc, so UNGATED like the four above.
                  // ★ __BIG_ENDIAN__ MUST NOT APPEAR — see the arm64 twin.
                  {"__LITTLE_ENDIAN__", K::Constant, "1", {}},
                  {"__BYTE_ORDER__",    K::Constant, "__ORDER_LITTLE_ENDIAN__", {}},
              }))
        << "x86_64 predefines all four spellings UNGATED (measured present on "
           "linux, darwin and windows-msvc alike), plus the two UNGATED "
           "endianness rows and NO __BIG_ENDIAN__";
}

// A target declaring NO `predefinedMacros` is legal and yields an EMPTY span —
// the no-regression path (the preprocessor's effective list is then exactly the
// language's).
TEST(TargetSchema, TFC74PredefinedMacrosOptional) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}]})",
        "<inline>");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE((*r)->predefinedMacros().empty());
}

// ── the entry grammar, inherited from the SHARED parser ──────────────────
// Each of these would have to be re-implemented (and could drift) had the
// target loader copied the language loader's parser instead of calling it.

TEST(TargetSchema, TFC74PredefinedMacroUnknownKindRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "predefinedMacros":[{"name":"__X__","kind":"consant","value":"1","impliedSurface":{"kind":"claims-nothing","reason":"arch-property"}}]})",
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "the `kind` verb set is CLOSED — a typo must never load as some "
           "default kind";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, TFC74PredefinedMacroConstantRequiresValue) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "predefinedMacros":[{"name":"__X__","kind":"constant","impliedSurface":{"kind":"claims-nothing","reason":"arch-property"}}]})",
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "a constant predefine with no `value` would expand to nothing — "
           "that must be a load error, not an empty macro";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MissingField));
}

TEST(TargetSchema, TFC74PredefinedMacroBadObjectFormatRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "predefinedMacros":[{"name":"__X__","kind":"constant","value":"1",
                                 "availableObjectFormats":["machoo"],"impliedSurface":{"kind":"claims-nothing","reason":"arch-property"}}]})",
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "an unknown object-format name is a typo that would make the macro "
           "dead on EVERY target — it must fail loud";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}
// (The SENTINEL variant of this typo test lives with the other TF-C76 sentinel
// pins at the end of this file — it needs the `anyMentions` helper declared
// there.)

TEST(TargetSchema, TFC74PredefinedMacroDuplicateNameRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "predefinedMacros":[{"name":"__X__","kind":"constant","value":"1","impliedSurface":{"kind":"claims-nothing","reason":"arch-property"}},
                                {"name":"__X__","kind":"constant","value":"2","impliedSurface":{"kind":"claims-nothing","reason":"arch-property"}}]})",
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "two entries for one name would make the effective value depend on "
           "which preprocessor seed site iterated last — fail loud instead";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// ── D-CONFIG-PREDEFINED-MACRO-ROW-KEYS-UNGATED ───────────────────────────
//
// The ENTRY object had no closed key vocabulary. Every optional field below is
// read with a bare `contains()` probe, so a misspelled key was silently DROPPED
// and the entry loaded clean carrying the default it was written to override —
// `"availabelObjectFormats"` predefining an architecture spelling on EVERY
// format is the sharpest form of it. Same archetype as the encoding-variant
// row whose `"tempalte"` yielded an all-default template.
//
// The gate is in the SHARED entry parser, so the TARGET family inherits it
// rather than re-implementing it — the same argument that extracted the parser
// at TF-C74. This pin exists because "inherited" is a claim about THIS caller,
// and a claim about a caller is only true where a test drives that caller.
//
// RED-ON-DISABLE: delete the rejection loop in `parsePredefinedMacroArray` and
// this load succeeds.
TEST(TargetSchema, PredefinedMacroEntryUnknownKeyRejectedAndNamed) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "predefinedMacros":[{"name":"__X__","kind":"constant","value":"1",
                                 "availabelObjectFormats":["elf"],"impliedSurface":{"kind":"claims-nothing","reason":"arch-property"}}]})",
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "a misspelled entry key must be REFUSED — silently ignoring this "
           "one predefines an architecture spelling on EVERY object format";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
    bool named = false;
    for (auto const& d : r.error()) {
        named = d.message.find("availabelObjectFormats") != std::string::npos
             && d.message.find("predefinedMacros") != std::string::npos;
        if (named) break;
    }
    EXPECT_TRUE(named)
        << "the diagnostic must name the offending key AND the container";
}

// The `$`-prefix carve-out inside an ENTRY (the root-key carve-out is pinned
// separately). Both shipped targets and the shipped c-subset language put prose
// inside these rows, so without this the gate would reject them at load — the
// inverse failure, and the one that actually fires.
TEST(TargetSchema, PredefinedMacroEntryDollarPrefixedKeyStillAccepted) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "predefinedMacros":[{"name":"__X__","kind":"constant","value":"1",
                                 "$valueComment":"why this spelling","impliedSurface":{"kind":"claims-nothing","reason":"arch-property"}}]})",
        "<inline>");
    ASSERT_TRUE(r.has_value())
        << "`$`-prefixed keys are prose, not knobs — and the carve-out must be "
           "the PREFIX predicate, not a literal `$comment` compare";
}

// ── defaultAssemblyLanguage (D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET) ─────
//
// A NAME, and only a name. The value is a `loadShipped` stem — exactly what
// `--language` takes — and the loader deliberately does NOT check that the
// language exists or that it claims `.s`: resolving a language name is the
// grammar loader's job, and a second copy of language discovery here would be
// a drifting one.

TEST(TargetSchema, DefaultAssemblyLanguageIsReadAsAName) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "defaultAssemblyLanguage":"asm-x86_64-att",
            "opcodes":[{"mnemonic":"invalid","result":"none"}]})",
        "<inline>");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->defaultAssemblyLanguage(), "asm-x86_64-att");
}

// ABSENT is a legitimate state, not a malformed document: a target with no
// assembly dialect simply fails loud BY NAME at the driver when a build needs
// one. Rejecting it here would force every target file to carry the key.
TEST(TargetSchema, DefaultAssemblyLanguageAbsentIsEmptyNotAnError) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}]})",
        "<inline>");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE((*r)->defaultAssemblyLanguage().empty());
}

TEST(TargetSchema, DefaultAssemblyLanguageNonStringRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "defaultAssemblyLanguage":["asm-x86_64-att"],
            "opcodes":[{"mnemonic":"invalid","result":"none"}]})",
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "a list would imply a SELECTOR between dialects, which does not "
           "exist — the key is one name and a wrong shape must say so";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// `""` is rejected rather than folded into "absent". The two states are
// operator-distinguishable — absent says "this target has no dialect", `""`
// says "I meant to name one" — and silently equating them is how a config key
// stops meaning anything.
TEST(TargetSchema, DefaultAssemblyLanguageEmptyStringRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "defaultAssemblyLanguage":"",
            "opcodes":[{"mnemonic":"invalid","result":"none"}]})",
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "an empty name is a half-filled key, not a declaration of absence";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// ★ THE SHIPPED TARGETS MUST ACTUALLY CARRY IT, and they must carry DIFFERENT
// values. Equal values would mean one dialect served both CPUs, which is the
// state the whole per-target resolution exists because we are NOT in: `#` is a
// line comment in the x86_64 dialect and an immediate marker in the arm64 one.
TEST(TargetSchema, ShippedTargetsDeclareDistinctAssemblyLanguages) {
    auto x86 = TargetSchema::loadShipped("x86_64");
    auto arm = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(x86.has_value());
    ASSERT_TRUE(arm.has_value());
    EXPECT_EQ((*x86)->defaultAssemblyLanguage(), "asm-x86_64-att");
    EXPECT_EQ((*arm)->defaultAssemblyLanguage(), "asm-arm64-gas");
    EXPECT_NE((*x86)->defaultAssemblyLanguage(),
              (*arm)->defaultAssemblyLanguage())
        << "one invocation compiling a .s for both CPUs must resolve TWO "
           "grammars; equal names here would silently make it one";
}

// ★★ THE LINE BETWEEN VOCABULARY AND GRAMMAR, PINNED IN A TEST.
// An `asmSyntax` block carrying `registerPrefix`/`immediatePrefix`/comment
// characters/operand order was written into both shipped target files once and
// REVERTED ([[D-CONFIG-ASM-DIALECT-DECLARED-AS-TARGET-VOCABULARY]]): those are
// (target, DIALECT) facts — ✔MEASURED with gcc on ONE target, AT&T
// `movq %rsi, (%rdi)` vs Intel `mov QWORD PTR [rdi], rsi`. The closed root-key
// vocabulary is what makes re-proposing it fail rather than load, so assert
// that directly instead of trusting the reverted diff to stay reverted.
TEST(TargetSchema, AsmGrammarKeyOnATargetIsRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "asmSyntax":{"registerPrefix":"%","immediatePrefix":"$"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}]})",
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "assembly GRAMMAR on a target must not load — a register prefix is "
           "a function of (target, dialect), not of target";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// ── the closed root-key vocabulary (TF-C74) ──────────────────────────────
//
// ★ This is the pin that makes the whole feature HONEST. Before TF-C74 the
// target loader read every root key through a bare `doc.contains(…)` and
// ignored unknowns, so a misspelled `"predefindMacros"` would have loaded
// perfectly clean and the entire per-architecture-identity feature would have
// silently no-op'd — a knob that lies. RED-ON-DISABLE: delete the
// `kTargetDocumentKeys` loop in target_schema_json.cpp and this test fails
// while every other test in this file still passes.
TEST(TargetSchema, TFC74UnknownRootKeyRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "predefindMacros":[{"name":"__X__","kind":"constant","value":"1"}]})",
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "a misspelled root key must be REJECTED — silently ignoring it "
           "makes every optional key a knob that can lie";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// The `$`-prefix documentation carve-out is MANDATORY, not decorative: both
// shipped target files use `$comment` / `$…Comment` heavily, so without it the
// closed vocabulary above would reject every shipped target on first load.
TEST(TargetSchema, TFC74DollarPrefixedRootKeysStillAccepted) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "$comment":"prose, not config",
            "$predefinedMacrosComment":"why these spellings are gated",
            "opcodes":[{"mnemonic":"invalid","result":"none"}]})",
        "<inline>");
    ASSERT_TRUE(r.has_value())
        << "`$`-prefixed keys are the codebase-wide documentation convention "
           "and must survive the typo discriminator";
}

// Both SHIPPED targets must load clean under the closed vocabulary — the
// regression this catches is adding a root key to a .target.json without
// adding it to `kTargetDocumentKeys` (or vice versa).
TEST(TargetSchema, TFC74ShippedTargetsSatisfyClosedRootKeyVocabulary) {
    for (char const* name : {"arm64", "x86_64"}) {
        auto r = TargetSchema::loadShipped(name);
        EXPECT_TRUE(r.has_value())
            << name << ".target.json must load clean under the closed "
                       "root-key vocabulary";
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// TF-C75 (D-TARGET-CHAR-SIGNEDNESS-PER-PLATFORM) — bare-`char` signedness, the
// SINGLE SOURCE OF TRUTH.
//
// The whole (processor × platform) fact lives in ONE key on the TARGET:
//
//     "charIsUnsigned": { "default": true,
//                         "byObjectFormat": { "macho": false, "pe": false } }
//
// It was briefly split across the target (a bare bool) and every `.format.json`
// (a `charSignedness` tri-state) — two places, reconciled by a free function.
// The collapse to one place is what these tests pin: the shape, its fail-loud
// rejections, the resolution, and the shipped declarations themselves.
//
// WHY IT CANNOT LIVE ON THE FORMAT INSTEAD: `elf` serves BOTH aarch64
// (unsigned) and x86_64 (signed), so a flat value there is a lie on one of
// them. WHY IT CANNOT BE A BARE BOOL HERE: `true` alone asserts one answer for
// every platform this processor serves, and that assertion was the miscompile
// (correct for aarch64-linux, silently wrong for arm64-darwin).
// ═════════════════════════════════════════════════════════════════════════════

namespace {

using ::dss::ObjectFormatKind;

// A minimal target whose `charIsUnsigned` key is exactly `body` (spliced in
// verbatim, so a test can probe a MALFORMED shape and not just a wrong value).
[[nodiscard]] std::string targetWithCharIsUnsigned(std::string_view body) {
    return std::string{
               R"({"dssTargetVersion":1,"target":{"name":"X"},)"
               R"("opcodes":[{"mnemonic":"invalid","result":"none"}],)"
               R"("charIsUnsigned":)"}
           + std::string{body} + "}";
}

// True iff SOME diagnostic mentions `needle` (message or path). A rejection
// that does not NAME the offending key sends the reader back to eyeballing the
// file by hand.
[[nodiscard]] bool anyMentions(auto const& diags, std::string_view needle) {
    return std::ranges::any_of(diags, [needle](auto const& d) {
        return d.message.find(needle) != std::string::npos
            || d.path.find(needle)    != std::string::npos;
    });
}

// Every ObjectFormatKind an emitted image can actually have, so a "resolves for
// EVERY format" assertion cannot quietly skip one.
constexpr ObjectFormatKind kRealFormatKinds[] = {
    ObjectFormatKind::Elf,  ObjectFormatKind::Pe,   ObjectFormatKind::MachO,
    ObjectFormatKind::Wasm, ObjectFormatKind::Spirv};

}  // namespace

// ── the SHIPPED matrix, EXACT SET (not a count, not a spot check) ───────────
//
// ★ THE anti-drift pin. Every (shipped target × every format kind) pair, whole
// map, one comparison. A count would stay green if `macho` and `elf` silently
// traded values; a spot check on arm64×macho would stay green if the `pe`
// forward guard were dropped. The resolved truth table is the deliverable, so
// the resolved truth table is what is compared.
//
// RED-ON-DISABLE: delete either `byObjectFormat` row from arm64.target.json, or
// make `charIsUnsigned(kind)` ignore its argument, and this fails naming the
// exact pair.
TEST(TargetSchema, TFC75ShippedCharSignednessMatrixIsExact) {
    struct Row {
        std::string_view target;
        std::string_view format;
        bool             charIsUnsigned;
        bool operator==(Row const&) const = default;
    };
    // MEASURED 2026-07-28 with /usr/bin/clang (Apple clang 21.0.0) via
    // `clang -dM -E` (__CHAR_UNSIGNED__), `_Static_assert((char)-1 < 0)` and
    // ldrsb-vs-ldrb codegen: aarch64-linux is the ONLY unsigned leg.
    std::vector<Row> const expected{
        // arm64: the SAME processor, OPPOSITE answers, decided by the platform.
        {"arm64",  "elf",   true },  // AAPCS64 base standard — unsigned
        {"arm64",  "pe",    false},  // forward guard: no pe64-arm64 format file
                                     // exists yet; if one lands while PE is
                                     // silent, Windows-ARM64 silently inherits
                                     // the bug this anchor was opened for
        {"arm64",  "macho", false},  // Apple's platform ABI — signed
        {"arm64",  "wasm",  true },  // no override ⇒ the processor default
        {"arm64",  "spirv", true },
        // x86_64: signed on every platform it serves, so it declares NO key at
        // all and every row falls to the absent-key default.
        {"x86_64", "elf",   false},
        {"x86_64", "pe",    false},
        {"x86_64", "macho", false},
        {"x86_64", "wasm",  false},
        {"x86_64", "spirv", false},
    };

    std::vector<Row> actual;
    for (auto const& row : expected) {
        auto t = TargetSchema::loadShipped(std::string{row.target});
        ASSERT_TRUE(t.has_value()) << row.target;
        auto const kind = ::dss::objectFormatKindFromName(row.format);
        ASSERT_TRUE(kind.has_value()) << row.format;
        actual.push_back(Row{row.target, row.format,
                             (*t)->charIsUnsigned(*kind)});
    }
    EXPECT_EQ(actual, expected)
        << "the shipped bare-`char` signedness matrix drifted — arm64 must be "
           "UNSIGNED on elf and SIGNED on macho/pe, x86_64 SIGNED everywhere";
}

// ── the accessor's own quadrants, on hand-built schemas ─────────────────────
//
// The shipped matrix above cannot reach two of these: no shipped target
// overrides a `false` default UP to unsigned, and none declares the object form
// without a `byObjectFormat`. An inverted comparison inside the accessor would
// survive a shipped-only test, because both branches produce a valid-looking
// answer.
TEST(TargetSchema, TFC75OverrideBeatsDefaultInBothDirections) {
    // default UNSIGNED, macho overrides DOWN to signed (the shipped shape).
    auto down = TargetSchema::loadFromText(
        targetWithCharIsUnsigned(
            R"({"default":true,"byObjectFormat":{"macho":false}})"),
        "<inline>");
    ASSERT_TRUE(down.has_value());
    EXPECT_FALSE((*down)->charIsUnsigned(ObjectFormatKind::MachO))
        << "a declared override must beat the default";
    EXPECT_TRUE((*down)->charIsUnsigned(ObjectFormatKind::Elf))
        << "a format with no override must take the default";

    // default SIGNED, macho overrides UP to unsigned — the OTHER direction,
    // which an inverted `if` would keep passing without.
    auto up = TargetSchema::loadFromText(
        targetWithCharIsUnsigned(
            R"({"default":false,"byObjectFormat":{"macho":true}})"),
        "<inline>");
    ASSERT_TRUE(up.has_value());
    EXPECT_TRUE((*up)->charIsUnsigned(ObjectFormatKind::MachO))
        << "the override must win when it flips signed→unsigned too, not only "
           "unsigned→signed";
    EXPECT_FALSE((*up)->charIsUnsigned(ObjectFormatKind::Elf));
}

// ★ An override of `false` must be DISTINGUISHABLE from no override at all.
// This is the whole reason the storage carries a presence bit rather than a
// bare bool: the shipped macho/pe rows declare exactly `false`, so if "declared
// false" collapsed into "undeclared" those rows would silently vanish and arm64
// would zero-extend on Darwin again.
TEST(TargetSchema, TFC75FalseOverrideIsDistinguishableFromNoOverride) {
    auto r = TargetSchema::loadFromText(
        targetWithCharIsUnsigned(
            R"({"default":true,"byObjectFormat":{"macho":false}})"),
        "<inline>");
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE((*r)->charIsUnsigned(ObjectFormatKind::MachO));
    for (auto const kind : kRealFormatKinds) {
        if (kind == ObjectFormatKind::MachO) continue;
        EXPECT_TRUE((*r)->charIsUnsigned(kind))
            << "only the DECLARED format may be overridden — "
            << ::dss::objectFormatKindName(kind) << " must keep the default";
    }
}

// `byObjectFormat` is optional: an object with only `default` states a uniform
// answer, and must resolve to it for EVERY format kind.
TEST(TargetSchema, TFC75DefaultOnlyObjectAppliesToEveryFormat) {
    auto r = TargetSchema::loadFromText(
        targetWithCharIsUnsigned(R"({"default":true})"), "<inline>");
    ASSERT_TRUE(r.has_value());
    for (auto const kind : kRealFormatKinds) {
        EXPECT_TRUE((*r)->charIsUnsigned(kind))
            << ::dss::objectFormatKindName(kind);
    }
}

// The key is OPTIONAL as a whole — absent ⇒ signed on every format. This is the
// x86_64 path and it must stay byte-identical to pre-TF-C75 behaviour.
TEST(TargetSchema, TFC75AbsentKeyMeansSignedOnEveryFormat) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}]})",
        "<inline>");
    ASSERT_TRUE(r.has_value());
    for (auto const kind : kRealFormatKinds) {
        EXPECT_FALSE((*r)->charIsUnsigned(kind))
            << "a target that declares nothing must be SIGNED everywhere — the "
               "C-common default x86_64.target.json relies on: "
            << ::dss::objectFormatKindName(kind);
    }
}

// ── loader fail-loud ────────────────────────────────────────────────────────

// ★ THE HOLE THE PRECEDENT LEFT OPEN, closed here. `wideFloatSoftcallLibrary-
// ByFormat` accepts ARBITRARY string keys, so on that key `"machO"` silently
// means NO ENTRY. Inherited here it would mean: no override → silent fallback
// to the arm64 default → bare `char` zero-extends on Darwin, with no
// diagnostic. Exactly the miscompile this cycle exists to fix.
TEST(TargetSchema, TFC75UnknownObjectFormatKeyRejectedAndNamed) {
    auto r = TargetSchema::loadFromText(
        targetWithCharIsUnsigned(
            R"({"default":true,"byObjectFormat":{"machO":false}})"),
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "a mis-cased/typo'd format name must fail loud — silently ignoring "
           "it re-creates the exact silent miscompile this key was reshaped to "
           "eliminate";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
    EXPECT_TRUE(anyMentions(r.error(), "machO"))
        << "the diagnostic must NAME the offending key";
}

// The `unknown` sentinel names no real format, so an override under it could
// never fire — the `bitFieldStrategy` "none" discipline.
TEST(TargetSchema, TFC75UnknownSentinelFormatKeyRejected) {
    auto r = TargetSchema::loadFromText(
        targetWithCharIsUnsigned(
            R"({"default":true,"byObjectFormat":{"unknown":false}})"),
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "'unknown' is the invalid sentinel, not a selectable format";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// ★ A BARE BOOLEAN IS REJECTED, ON PURPOSE. It is the pre-TF-C75 shape and it
// READS as "char is unsigned on this target, full stop" — the false sentence
// this reshape deleted. Accepting it as a shorthand would let that sentence
// back into a config file, and the resulting silent-fallback-to-default is
// indistinguishable at the call site from a correct uniform answer.
TEST(TargetSchema, TFC75BareBooleanShapeRejected) {
    for (char const* legacy : {"true", "false"}) {
        auto r = TargetSchema::loadFromText(
            targetWithCharIsUnsigned(legacy), "<inline>");
        ASSERT_FALSE(r.has_value())
            << "the legacy bare-bool shape must fail loud, never be silently "
               "re-interpreted as a uniform default: " << legacy;
        EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
    }
}

// Any other non-object scalar likewise — a string `"unsigned"` is the shape of
// the REMOVED format-side key and must not be quietly accepted here.
TEST(TargetSchema, TFC75NonObjectShapesRejected) {
    for (char const* body : {R"("unsigned")", "1", "null", "[]"}) {
        auto r = TargetSchema::loadFromText(
            targetWithCharIsUnsigned(body), "<inline>");
        EXPECT_FALSE(r.has_value())
            << "non-object charIsUnsigned must fail loud: " << body;
    }
}

// `default` is REQUIRED inside the object. Without it, every format with no
// override would resolve to an implicit `false` that no file states — a silent
// fallback on the one axis whose two answers are opposite high-bit extensions.
TEST(TargetSchema, TFC75MissingDefaultRejected) {
    auto r = TargetSchema::loadFromText(
        targetWithCharIsUnsigned(R"({"byObjectFormat":{"macho":false}})"),
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "an object with no 'default' leaves un-overridden formats resolving "
           "to a signedness nothing declares";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MissingField));
}

// The inner keys are a CLOSED vocabulary: a misspelled `"defualt"` would
// otherwise read as "no default declared" AND drop the value silently.
TEST(TargetSchema, TFC75UnknownInnerKeyRejectedAndNamed) {
    auto r = TargetSchema::loadFromText(
        targetWithCharIsUnsigned(R"({"default":true,"byObjectFormt":{}})"),
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "a misspelled inner key must be REJECTED — ignoring it makes the "
           "override map a knob that can lie";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
    EXPECT_TRUE(anyMentions(r.error(), "byObjectFormt"))
        << "the diagnostic must NAME the offending key";
}

// Malformed value types on both halves.
TEST(TargetSchema, TFC75NonBooleanValuesRejected) {
    auto badDefault = TargetSchema::loadFromText(
        targetWithCharIsUnsigned(R"({"default":"true"})"), "<inline>");
    EXPECT_FALSE(badDefault.has_value())
        << "'default' must be a boolean — a string 'true' is a typo, not a yes";

    auto badOverride = TargetSchema::loadFromText(
        targetWithCharIsUnsigned(
            R"({"default":true,"byObjectFormat":{"macho":"signed"}})"),
        "<inline>");
    EXPECT_FALSE(badOverride.has_value())
        << "an override must be a boolean — 'signed' is the spelling of the "
           "REMOVED format-side key and must not be accepted here";

    auto badMap = TargetSchema::loadFromText(
        targetWithCharIsUnsigned(R"({"default":true,"byObjectFormat":true})"),
        "<inline>");
    EXPECT_FALSE(badMap.has_value())
        << "'byObjectFormat' must be an object";
}

// The `$`-documentation carve-out must reach INSIDE the block too — the shipped
// arm64 file explains this key at length, and prose must never be mistaken for
// a format name.
TEST(TargetSchema, TFC75DollarPrefixedInnerKeysAccepted) {
    auto r = TargetSchema::loadFromText(
        targetWithCharIsUnsigned(
            R"({"$comment":"why Darwin differs","default":true,
                "byObjectFormat":{"$machoComment":"Apple chose signed",
                                  "macho":false}})"),
        "<inline>");
    ASSERT_TRUE(r.has_value())
        << "`$`-prefixed keys are the codebase-wide documentation convention";
    EXPECT_FALSE((*r)->charIsUnsigned(ObjectFormatKind::MachO));
    EXPECT_TRUE((*r)->charIsUnsigned(ObjectFormatKind::Elf));
}

// ═════════════════════════════════════════════════════════════════════════════
// `wideFloatSoftcallLibraryByFormat` — THE HOLE THE TF-C75 TESTS NAMED
// ═════════════════════════════════════════════════════════════════════════════
//
// TF-C75's `TFC75UnknownObjectFormatKeyRejectedAndNamed` above opens with "★ THE
// HOLE THE PRECEDENT LEFT OPEN" and points AT THIS KEY: it accepted ARBITRARY
// string keys, so `"elff"` / `"ELF"` stored cleanly, the accessor's raw-string
// lookup missed, and the F128 softcall path reported that the format declares no
// softcall library. Long-double arithmetic degraded on a PURE TYPO with no
// diagnostic naming the config.
//
// The fix is the SAME reshape TF-C75 used, not merely the same validation: the
// map is now an `ObjectFormatKind`-INDEXED ARRAY, so an unresolvable key has no
// slot to be stored in. Invalid state unrepresentable, not just rejected — which
// is why the accessor now takes the KIND and the raw-string lookup is gone.
//
// A minimal target whose `wideFloatSoftcallLibraryByFormat` is exactly `body`,
// spliced verbatim so a test can probe a MALFORMED shape, not just a bad value.
namespace {
[[nodiscard]] std::string targetWithSoftcallLibrary(std::string_view body) {
    return std::string{
               R"({"dssTargetVersion":1,"target":{"name":"X"},)"
               R"("opcodes":[{"mnemonic":"invalid","result":"none"}],)"
               R"("wideFloatSoftcallLibraryByFormat":)"}
           + std::string{body} + "}";
}
}  // namespace

// ★ THE TYPO CASE. Mirrors TFC75UnknownObjectFormatKeyRejectedAndNamed exactly:
// rejected AND named. Both spellings a human actually produces are covered — a
// slip (`elff`) and a case error (`ELF`) — because the old string map treated
// both as "no entry for elf" and said nothing.
//
// RED-ON-DISABLE: drop the `objectFormatKindFromName` check in the loader and
// the load succeeds, leaving `wideFloatSoftcallLibrary(Elf)` empty.
TEST(TargetSchema, TFC76SoftcallLibraryUnknownFormatKeyRejectedAndNamed) {
    for (auto const* bad : {"elff", "ELF", "Mach-O"}) {
        auto const body =
            std::string{R"({")"} + bad + R"(":"libgcc_s.so.1"})";
        auto r = TargetSchema::loadFromText(targetWithSoftcallLibrary(body),
                                            "<inline>");
        ASSERT_FALSE(r.has_value())
            << "a typo'd/mis-cased format key must fail loud — silently "
               "ignoring it leaves the F128 softcall path with no runtime "
               "library and no diagnostic pointing at the config: " << bad;
        EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson))
            << bad;
        EXPECT_TRUE(anyMentions(r.error(), bad))
            << "the diagnostic must NAME the offending key: " << bad;
    }
}

// ★ A DIFFERENT SPECIES FROM A TYPO — the sentinel SPELLS CORRECTLY, so
// `objectFormatKindFromName("unknown")` SUCCEEDS and the name check above waves
// it through. Only an explicit selectability check stops it. The
// `bitFieldStrategy` "none" discipline, and TF-C75's sibling assertion.
TEST(TargetSchema, TFC76SoftcallLibrarySentinelFormatKeyRejected) {
    auto r = TargetSchema::loadFromText(
        targetWithSoftcallLibrary(R"({"unknown":"libgcc_s.so.1"})"),
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "'unknown' is the invalid sentinel, not a selectable format — a "
           "library declared under it could never resolve for any real image";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
    EXPECT_TRUE(anyMentions(r.error(), "sentinel"))
        << "the diagnostic must say WHY 'unknown' is refused — it spells "
           "correctly, so 'unrecognized name' would be a confusing lie";
}

// An EMPTY library string is the same silent fallback one layer down: the
// accessor cannot distinguish it from an absent key, so a file that plainly
// declares a library would resolve to "this format declares none".
TEST(TargetSchema, TFC76SoftcallLibraryEmptyValueRejected) {
    auto r = TargetSchema::loadFromText(
        targetWithSoftcallLibrary(R"({"elf":""})"), "<inline>");
    ASSERT_FALSE(r.has_value())
        << "an empty library value is indistinguishable from declaring none";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
    EXPECT_TRUE(anyMentions(r.error(), "non-empty"));
}

// Shape errors on both halves stay loud.
TEST(TargetSchema, TFC76SoftcallLibraryMalformedShapesRejected) {
    for (char const* body : {R"({"elf":5})", R"({"elf":null})",
                             R"({"elf":["libgcc_s.so.1"]})",
                             R"("libgcc_s.so.1")", "[]", "true"}) {
        auto r = TargetSchema::loadFromText(targetWithSoftcallLibrary(body),
                                            "<inline>");
        EXPECT_FALSE(r.has_value())
            << "malformed wideFloatSoftcallLibraryByFormat must fail loud: "
            << body;
    }
}

// The `$`-documentation carve-out reaches inside this map too (the shipped
// arm64 file documents this key at length right next to it).
TEST(TargetSchema, TFC76SoftcallLibraryDollarPrefixedKeysAccepted) {
    auto r = TargetSchema::loadFromText(
        targetWithSoftcallLibrary(
            R"({"$elfComment":"libgcc holds the __addtf3 family",
                "elf":"libgcc_s.so.1"})"),
        "<inline>");
    ASSERT_TRUE(r.has_value())
        << "`$`-prefixed keys are the codebase-wide documentation convention";
    EXPECT_EQ((*r)->wideFloatSoftcallLibrary(ObjectFormatKind::Elf),
              "libgcc_s.so.1");
}

// ★ SHIPPED BEHAVIOUR IS UNCHANGED, AS AN EXACT TABLE — not a spot check.
// `arm64.target.json` declares `{"elf": "libgcc_s.so.1"}` and nothing else; the
// whole resolved row is compared so a reshape that quietly moved the value to a
// different kind, or leaked it to every kind, cannot pass. x86_64 declares the
// key not at all and must resolve empty everywhere.
//
// RED-ON-DISABLE: index the array by anything other than the resolved kind and
// the elf cell moves; return the first non-empty slot instead of the indexed one
// and every arm64 cell fills.
TEST(TargetSchema, TFC76ShippedSoftcallLibraryMatrixIsExact) {
    // Rows are compared as printable "target/format => library" strings so a
    // failure NAMES the drifted cell instead of dumping struct bytes. Note the
    // accessor returns a `string_view` into the schema, so each cell is copied
    // to a `std::string` before the schema goes out of scope.
    struct Cell { std::string_view target, format; };
    constexpr Cell kCells[] = {
        {"arm64",  "elf"}, {"arm64",  "pe"}, {"arm64",  "macho"},
        {"arm64",  "wasm"}, {"arm64", "spirv"},
        {"x86_64", "elf"}, {"x86_64", "pe"}, {"x86_64", "macho"},
        {"x86_64", "wasm"}, {"x86_64", "spirv"},
    };
    std::vector<std::string> const expected{
        // arm64: ONLY elf. The f64-axis formats (pe/macho-arm64) collapse long
        // double to double and never reach the softcall path.
        "arm64/elf => libgcc_s.so.1",
        "arm64/pe => ",
        "arm64/macho => ",
        "arm64/wasm => ",
        "arm64/spirv => ",
        // x86_64 declares no key at all (it uses the inline x87 sequence).
        "x86_64/elf => ",
        "x86_64/pe => ",
        "x86_64/macho => ",
        "x86_64/wasm => ",
        "x86_64/spirv => ",
    };

    std::vector<std::string> actual;
    for (auto const& cell : kCells) {
        auto t = TargetSchema::loadShipped(std::string{cell.target});
        ASSERT_TRUE(t.has_value()) << cell.target;
        auto const kind = ::dss::objectFormatKindFromName(cell.format);
        ASSERT_TRUE(kind.has_value()) << cell.format;
        actual.push_back(
            std::string{cell.target} + "/" + std::string{cell.format} + " => "
            + std::string{(*t)->wideFloatSoftcallLibrary(*kind)});
    }
    EXPECT_EQ(actual, expected)
        << "the shipped softcall-library matrix changed — this reshape must be "
           "byte-for-byte behaviour-preserving for shipped config";
}

// ★ THE SENTINEL VARIANT of `TFC74PredefinedMacroBadObjectFormatRejected`
// (which probes the typo `"machoo"`). "machoo" fails the name lookup;
// "unknown" PASSES it — it is a row in the table — and then narrows
// availability to a format no image can have, so the macro is silently
// predefined NOWHERE. That is exactly the dead-on-every-target outcome the
// typo check exists to prevent, reached by a correctly-spelled word.
//
// RED-ON-DISABLE: remove the `isSelectableObjectFormatKind` branch in
// `predefined_macro_json.cpp` and this load succeeds.
TEST(TargetSchema, TFC76PredefinedMacroSentinelObjectFormatRejected) {
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}],
            "predefinedMacros":[{"name":"__X__","kind":"constant","value":"1",
                                 "availableObjectFormats":["unknown"],"impliedSurface":{"kind":"claims-nothing","reason":"arch-property"}}]})",
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "'unknown' spells correctly, so only an explicit selectability check "
           "stops it from making the macro dead on every target";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
    EXPECT_TRUE(anyMentions(r.error(), "sentinel"))
        << "the diagnostic must say WHY 'unknown' is refused";
}

// The sentinel slot must stay empty even though the array physically HAS one
// (it is indexed by ordinal, and Unknown == 0). Nothing can write it, so
// nothing can read a library out of it.
TEST(TargetSchema, TFC76SoftcallLibrarySentinelSlotAlwaysEmpty) {
    for (auto const* name : {"arm64", "x86_64"}) {
        auto t = TargetSchema::loadShipped(name);
        ASSERT_TRUE(t.has_value()) << name;
        EXPECT_TRUE(
            (*t)->wideFloatSoftcallLibrary(ObjectFormatKind::Unknown).empty())
            << "the Unknown slot is unwritable by the loader, so it must never "
               "resolve to a library: " << name;
    }
}

// ── D-ASM-ARM64-NEGATIVE-IMMEDIATE-UNENCODABLE — guard-axis loader pins ──
//
// The sign-routing axis was `guard.negMemoffset` and was MEMOFFSET-ONLY by
// its validate() rule alone — the matcher underneath always read "the first
// ImmInt-or-MemOffset operand". Sign-routing an IMMEDIATE (arm64 MOVN vs
// MOVZ) is the same question about a different operand kind, so the axis was
// RENAMED to `guard.negValue` and its coherence rule widened to the SAME
// predicate immMin/immMax already used. These pins hold that shape.

namespace {
// The minimum well-formed target document, with one opcode's encoding
// variant guard supplied by the caller. Keeps each pin to the ONE key it is
// actually about.
[[nodiscard]] std::string targetDocWithGuard(std::string_view guardBody,
                                             std::string_view slotKind) {
    return std::string{
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"mov","result":"value",
               "minOperands":1,"maxOperands":1,
               "encoding":{"format":"fixed32","variants":[
                 {"guard":{)"} + std::string{guardBody} + R"(},
                  "template":{"fixedWord":2457862144},
                  "resultSlot":"rd",
                  "wires":[{"index":0,"slotKind":")"
        + std::string{slotKind} + R"("}]}
               ]}}
            ]})";
}
} // namespace

TEST(TargetSchema, NegValueOnAnImmediateOperandLoads) {
    // The POINT of the generalization: `negValue` on an `imm32` guard — the
    // arm64 MOVN shape — must be ACCEPTED. Under the old memoffset-only rule
    // this exact document was rejected, which is why the anchor needed a
    // schema change and not just a config row.
    auto r = TargetSchema::loadFromText(
        targetDocWithGuard(R"("operandKinds":["imm32"],"negValue":true,)"
                           R"("immMin":1,"immMax":65536)",
                           "imm16.inverted"),
        "<inline>");
    if (!r.has_value()) {
        for (auto const& d : r.error()) {
            ADD_FAILURE() << "load: " << d.path << ": " << d.message;
        }
    }
    EXPECT_TRUE(r.has_value())
        << "negValue must be legal on an immediate-bearing guard";
}

TEST(TargetSchema, NegValueOnAMemoffsetOperandStillLoads) {
    // The axis's ORIGINAL consumer must keep working after the widening —
    // a generalization that quietly drops its first caller is a regression.
    auto r = TargetSchema::loadFromText(
        targetDocWithGuard(
            R"("operandKinds":["memoffset"],"negValue":true,"immMax":4095)",
            "imm12"),
        "<inline>");
    EXPECT_TRUE(r.has_value())
        << "negValue must remain legal on a memoffset-bearing guard";
}

TEST(TargetSchema, NegValueWithNoValueBearingOperandRejected) {
    // The coherence rule, still enforced — just against the WIDER predicate.
    // A `reg`-only guard carries nothing to sign-route on, so the flag would
    // key on a value that does not exist and the variant would silently
    // match nothing.
    auto r = TargetSchema::loadFromText(
        targetDocWithGuard(R"("operandKinds":["reg"],"negValue":true)", "rm"),
        "<inline>");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, UnknownGuardKeyRejectedIncludingTheOldNegMemoffsetSpelling) {
    // ★ THE RENAME'S SAFETY NET, and the reason the rename is not a silent
    // hazard. Every `guard` sub-key is read through a bare `contains(...)`,
    // so before this gate a stale or typo'd key was SILENTLY IGNORED — the
    // variant would quietly take default routing. For the renamed axis that
    // is not cosmetic: a leftover `negMemoffset:true` read as `false` turns
    // arm64's three negative-displacement `lea` variants into duplicate
    // POSITIVE ones. Both spellings below must be rejected at load.
    for (auto key : {std::string_view{"negMemoffset"},
                     std::string_view{"negVlaue"}}) {
        auto r = TargetSchema::loadFromText(
            targetDocWithGuard(
                std::string{R"("operandKinds":["memoffset"],")"}
                    + std::string{key} + R"(":true)",
                "imm12"),
            "<inline>");
        ASSERT_FALSE(r.has_value()) << "guard key '" << key
            << "' must be rejected, never silently ignored";
        EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
    }
}

TEST(TargetSchema, GuardCommentKeyIsAccepted) {
    // `$comment` is the one universally-allowed unknown key across DSS
    // configs; the new gate must not break the convention.
    auto r = TargetSchema::loadFromText(
        targetDocWithGuard(
            R"("$comment":"why this variant exists","operandKinds":["imm32"])",
            "imm16"),
        "<inline>");
    EXPECT_TRUE(r.has_value())
        << "'$comment' must stay legal inside a variant guard";
}

TEST(TargetSchema, InvertedImm16SlotNameRoundTrips) {
    // The slot vocabulary is SHARED and its JSON spelling is the config
    // contract. Pin the name in BOTH directions so a table edit that renames
    // it breaks here rather than in every arm64 target file at once. The
    // name is deliberately generic ("an inverted 16-bit immediate"), never
    // arm64- or MOVN-named.
    EXPECT_EQ(encodingSlotKindName(EncodingSlotKind::Imm16Inverted),
              "imm16.inverted");
    auto const back = encodingSlotKindFromName("imm16.inverted");
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, EncodingSlotKind::Imm16Inverted);
    // It shares Imm16's ENCODING SHAPE (both are fixed32 bit-windows) but is
    // a DISTINCT slot — the two must never collapse into one enumerator.
    EXPECT_EQ(slotShapeFor(EncodingSlotKind::Imm16Inverted),
              slotShapeFor(EncodingSlotKind::Imm16));
    EXPECT_NE(EncodingSlotKind::Imm16Inverted, EncodingSlotKind::Imm16);
    // Not symbol-bearing: the encoder writes the complement directly, so a
    // wire to it must NOT declare a relocationKind.
    EXPECT_FALSE(isSymbolBearingSlot(EncodingSlotKind::Imm16Inverted));
}

TEST(TargetSchema, RetiredIsTerminatorKeyIsRefusedNotIgnored) {
    // Half 2 of the single-source-of-truth property. A RETIRED key is more
    // dangerous than a typo: a typo was never meaningful, but `isTerminator`
    // WAS, so silently dropping it hands the author a non-terminator they
    // believe they declared. The opcode-row key gate refuses it, and the
    // message names the vocabulary that replaced it, so the fix is readable
    // straight off the diagnostic.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[
              {"mnemonic":"invalid","result":"none"},
              {"mnemonic":"add","result":"value","isTerminator":true,
               "minOperands":2,"maxOperands":2}
            ]})",
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "the retired `isTerminator` key must be REFUSED, never ignored";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
    bool namesReplacement = false;
    for (auto const& d : r.error()) {
        if (d.message.find("terminatorKind") != std::string::npos) {
            namesReplacement = true;
        }
    }
    EXPECT_TRUE(namesReplacement)
        << "the diagnostic must name `terminatorKind` — the vocabulary that "
           "replaced the retired key";
}

// ═════════════════════════════════════════════════════════════════════════════
// D-CONFIG-TARGET-LOADER-CONTAINER-KEYS-UNGATED — the CLASS, not the instance.
//
// The archetype: a CONTAINER object with no key set sitting next to NESTED
// objects that have one. The neighbours' rejection loops are exactly what make
// the container's absence invisible — you see `variants[j]/guard` refusing
// typos and conclude the variant ROW does too, when a misspelled `"tempalte"`
// was silently yielding an all-default encoding template.
//
// These pins are TABLE-DRIVEN over the REAL SHIPPED arm64 document, so they
// cannot rot against a hand-written sample, and each container is asserted in
// BOTH directions:
//
//   * a typo'd key is REFUSED               (the gate exists and bites)
//   * a `$`-prefixed prose key is ACCEPTED  (the carve-out survives)
//
// ★ THE SECOND DIRECTION IS NOT DECORATION. Three of the file's five original
// gates hardcoded the literal `"$comment"` or nothing at all instead of the
// `$`-PREFIX predicate, so a `$templateComment` / `$framePointerComment` —
// spellings the shipped targets actually use — would have been reported as
// typos. A closed key set that omits a real key breaks EVERY target at load;
// that inverse failure is the one this half catches.

namespace {

// Insert `key` into the container named by `label` inside a parsed shipped
// arm64 document, then load. Each mutator navigates to ONE container.
using ContainerMutator = void (*)(nlohmann::json&, std::string const&);

struct GatedContainer {
    char const*      label;
    ContainerMutator inject;
};

[[nodiscard]] nlohmann::json& firstOpcodeWithEncoding(nlohmann::json& doc) {
    for (auto& o : doc["opcodes"]) {
        if (o.contains("encoding") && o["encoding"].contains("variants")
            && !o["encoding"]["variants"].empty()) {
            return o;
        }
    }
    return doc["opcodes"][0];
}

[[nodiscard]] nlohmann::json& firstVariantWithWires(nlohmann::json& doc) {
    for (auto& o : doc["opcodes"]) {
        if (!o.contains("encoding") || !o["encoding"].contains("variants")) continue;
        for (auto& v : o["encoding"]["variants"]) {
            if (v.contains("wires") && !v["wires"].empty()
                && v.contains("template")) {
                return v;
            }
        }
    }
    return doc["opcodes"][0];
}

[[nodiscard]] nlohmann::json* firstOpcodeWithImplicitRegisters(nlohmann::json& doc) {
    for (auto& o : doc["opcodes"]) {
        if (o.contains("implicitRegisters")) return &o["implicitRegisters"];
    }
    return nullptr;
}

constexpr std::array<GatedContainer, 13> kGatedContainers{{
    {"/target",
     [](nlohmann::json& d, std::string const& k) { d["target"][k] = true; }},
    {"/aggregateLayout",
     [](nlohmann::json& d, std::string const& k) { d["aggregateLayout"][k] = true; }},
    {"/opcodes[i]",
     [](nlohmann::json& d, std::string const& k) { d["opcodes"][0][k] = true; }},
    {"/opcodes[i]/encoding",
     [](nlohmann::json& d, std::string const& k) {
         firstOpcodeWithEncoding(d)["encoding"][k] = true; }},
    {"/opcodes[i]/encoding/variants[j]",
     [](nlohmann::json& d, std::string const& k) {
         firstVariantWithWires(d)[k] = true; }},
    {"/opcodes[i]/encoding/variants[j]/guard",
     [](nlohmann::json& d, std::string const& k) {
         firstVariantWithWires(d)["guard"][k] = true; }},
    {"/opcodes[i]/encoding/variants[j]/template",
     [](nlohmann::json& d, std::string const& k) {
         firstVariantWithWires(d)["template"][k] = true; }},
    {"/opcodes[i]/encoding/variants[j]/wires[k]",
     [](nlohmann::json& d, std::string const& k) {
         firstVariantWithWires(d)["wires"][0][k] = true; }},
    {"/registers[i]",
     [](nlohmann::json& d, std::string const& k) { d["registers"][0][k] = true; }},
    {"/registerClassOps[i]",
     [](nlohmann::json& d, std::string const& k) { d["registerClassOps"][0][k] = true; }},
    {"/relocations[i]",
     [](nlohmann::json& d, std::string const& k) { d["relocations"][0][k] = true; }},
    {"/callingConventions[i]",
     [](nlohmann::json& d, std::string const& k) { d["callingConventions"][0][k] = true; }},
    {"/callingConventions[i]/vaListLayout",
     [](nlohmann::json& d, std::string const& k) {
         d["callingConventions"][0]["vaListLayout"][k] = true; }},
}};

} // namespace

TEST(TargetSchema, EveryGatedContainerRefusesATypoKey) {
    for (auto const& c : kGatedContainers) {
        // `before`/`after` prove the mutation actually landed — a navigator
        // that missed its container would make this pin assert nothing (the
        // failure mode the implicitRegisters pins below walked into).
        nlohmann::json before, after;
        auto mutated = dss::test_support::mutateShippedTargetSchemaDoc(
            "arm64", [&](nlohmann::json& doc) {
                before = doc;
                c.inject(doc, "zzNotARealKey");
                after = doc;
            });
        EXPECT_NE(before, after)
            << c.label << ": the typo mutation was a NO-OP — the navigator "
                          "did not reach that container";
        EXPECT_FALSE(mutated.has_value())
            << c.label << " has NO closed key vocabulary — a misspelled key "
                          "there loads clean and the feature it names stays "
                          "silently switched off";
    }
}

TEST(TargetSchema, EveryGatedContainerStillAcceptsDollarPrefixedProse) {
    // The carve-out must be the PREFIX predicate, not a literal "$comment".
    // `$zzProseComment` is deliberately NOT `$comment`: a gate that
    // allowlisted the exact string would pass a `$comment` probe and still
    // reject the `$templateComment` / `$framePointerComment` spellings the
    // shipped targets use.
    for (auto const& c : kGatedContainers) {
        nlohmann::json before, after;
        auto mutated = dss::test_support::mutateShippedTargetSchemaDoc(
            "arm64", [&](nlohmann::json& doc) {
                before = doc;
                c.inject(doc, "$zzProseComment");
                after = doc;
            });
        EXPECT_NE(before, after)
            << c.label << ": the prose mutation was a NO-OP — the navigator "
                          "did not reach that container";
        EXPECT_TRUE(mutated.has_value())
            << c.label << " rejects a `$`-prefixed documentation key — the "
                          "carve-out must be the PREFIX predicate, never a "
                          "literal \"$comment\" entry";
    }
}

// ⚠ THESE TWO USE x86_64, NOT arm64, AND THE REASON IS A NULL EXPERIMENT I
// WALKED INTO: arm64 declares ZERO `implicitRegisters` blocks (they are the
// x86 div/mul/shift/cmpxchg projection), so the arm64 mutation was a NO-OP and
// the "refuses a typo" pin failed for the right reason while the "accepts
// prose" pin PASSED for the wrong one — it asserted nothing at all. Every
// mutation below therefore asserts the container WAS FOUND before asserting
// anything about the load.
TEST(TargetSchema, ImplicitRegistersAcceptsDollarPrefixedProse) {
    bool injected = false;
    auto mutated = dss::test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [&injected](nlohmann::json& doc) {
            if (auto* ir = firstOpcodeWithImplicitRegisters(doc)) {
                (*ir)["$zzProseComment"] = "prose, not a register list";
                injected = true;
            }
        });
    ASSERT_TRUE(injected)
        << "no shipped x86_64 opcode carries an `implicitRegisters` block — "
           "the mutation was a no-op and this pin would assert nothing";
    EXPECT_TRUE(mutated.has_value())
        << "an implicitRegisters block must accept `$`-prefixed prose (its "
           "hand-rolled loop rejected every `$` key before it was routed "
           "through the shared rejector)";
}

TEST(TargetSchema, CondCodeEncodingAcceptsDollarPrefixedProse) {
    bool injected = false;
    auto mutated = dss::test_support::mutateShippedTargetSchemaDoc(
        "arm64", [&injected](nlohmann::json& doc) {
            if (doc.contains("condCodeEncoding")) {
                doc["condCodeEncoding"]["$zzProseComment"] =
                    "prose, not a condition";
                injected = true;
            }
        });
    ASSERT_TRUE(injected)
        << "arm64 declares no `condCodeEncoding` — the mutation was a no-op";
    EXPECT_TRUE(mutated.has_value())
        << "condCodeEncoding must accept `$`-prefixed prose — its loop "
           "matches names against a closed table and needs the same "
           "carve-out every other closed vocabulary gets";
}

TEST(TargetSchema, CondCodeEncodingRefusesATypoKey) {
    bool injected = false;
    auto mutated = dss::test_support::mutateShippedTargetSchemaDoc(
        "arm64", [&injected](nlohmann::json& doc) {
            if (doc.contains("condCodeEncoding")) {
                doc["condCodeEncoding"]["eqq"] = 0;
                injected = true;
            }
        });
    ASSERT_TRUE(injected);
    EXPECT_FALSE(mutated.has_value())
        << "a misspelled condition name must be refused — silently ignoring "
           "it leaves that condition unencoded and every branch using it "
           "resolves to the zero nibble";
}

TEST(TargetSchema, ImplicitRegistersRefusesATypoKey) {
    bool injected = false;
    auto mutated = dss::test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [&injected](nlohmann::json& doc) {
            if (auto* ir = firstOpcodeWithImplicitRegisters(doc)) {
                (*ir)["inpts"] = nlohmann::json::array();
                injected = true;
            }
        });
    ASSERT_TRUE(injected)
        << "the mutation was a no-op — this pin would assert nothing";
    EXPECT_FALSE(mutated.has_value())
        << "a misspelled `inputs` must be refused, not silently leave the "
           "implicit-register block empty";
}

TEST(TargetSchema, ShippedTargetsLoadCleanUnderEveryContainerGate) {
    // ★ THE INVERSE-FAILURE PIN, and it has already earned its place: the
    // first cut of these gates omitted `/target/description` — a key both
    // shipped targets declare and the loader reads for nothing — and every
    // shipped target stopped loading (✔MEASURED: 21 red tests). A closed key
    // set is a claim about the CONFIGS, so it is pinned against the configs.
    for (char const* name : {"arm64", "x86_64"}) {
        auto r = TargetSchema::loadShipped(name);
        if (!r.has_value()) {
            for (auto const& d : r.error()) {
                ADD_FAILURE() << name << ": " << d.path << ": " << d.message;
            }
        }
        EXPECT_TRUE(r.has_value())
            << name << ".target.json must load clean under EVERY closed "
                       "container-key vocabulary, not just the root one";
    }
}


// ═══════════════════════════════════════════════════════════════════════
// GNU inline-asm constraint letters — the `asmConstraints` facet
// ═══════════════════════════════════════════════════════════════════════
//
// The facet declares WHICH LETTER MEANS WHICH REGISTER on this processor.
// ✔MEASURED with gcc 13.3.0 (`-O2 -S`, native x86_64 + aarch64-linux-gnu
// cross), with THREE competing live `"r"` operands so a lucky allocation
// cannot be mistaken for a pin: x86_64 r→%rax a→%rax b→%rbx c→%rcx d→%rdx
// S→%rsi D→%rdi x→%xmm0, `"m"(*p)`→`(%rdi)`, `"i"(7)`→`$7`; aarch64 r→x0
// w→v0, `"m"(*p)`→`[x0]`, `"i"(7)`→`7`, and a/b/c/d/D/q are every one of
// them `error: impossible constraint in 'asm'`.
//
// The three axes bind to vocabulary that already existed (`TargetRegClass`,
// `registers[].name`, `OperandKindFilter`) — see `TargetAsmConstraint` for
// why this is target vocabulary and the reverted `asmSyntax` block was not.

namespace {

// A minimal target carrying a register file, so a constraint row has
// something to resolve against. `rax` is ordinal 0 and `xmm0` ordinal 1 —
// ordinal 0 being VALID is exactly why the payload arms are `optional`.
constexpr char const* kConstraintFixturePrefix =
    R"({"dssTargetVersion":1,"target":{"name":"X"},
        "opcodes":[{"mnemonic":"invalid","result":"none"}],
        "registers":[
          {"name":"rax","class":"gpr","widthBytes":8,"hwEncoding":0},
          {"name":"xmm0","class":"fpr","widthBytes":16,"hwEncoding":0}
        ],
        "asmConstraints":)";

// Build a one-target document whose `asmConstraints` array is `rows`.
std::string constraintDoc(std::string_view rows) {
    return std::string{kConstraintFixturePrefix} + std::string{rows} + "}";
}

// One constraint row wrapped in that document — the negative tests differ
// only in the row, so the fixture noise stays out of each assertion.
std::string oneConstraintRow(std::string_view row) {
    return constraintDoc(std::string{"["} + std::string{row} + "]");
}

// Every register name in `cls`, as a set — the "what does this letter
// actually resolve to" projection the operator-required test compares.
std::set<std::string> registersInClass(TargetSchema const& s,
                                       TargetRegClass cls) {
    std::set<std::string> out;
    for (auto const& r : s.registers()) {
        if (r.regClass == cls) out.insert(r.name);
    }
    return out;
}

}  // namespace

TEST(TargetSchema, AsmConstraintsAbsentIsEmptyNotAnError) {
    // A processor whose inline-asm binding has not been described is a real
    // state, not a malformed document — it simply refuses every letter by
    // name. Rejecting absence would force the key onto every target file.
    auto r = TargetSchema::loadFromText(
        R"({"dssTargetVersion":1,"target":{"name":"X"},
            "opcodes":[{"mnemonic":"invalid","result":"none"}]})",
        "<inline>");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->asmConstraintCount(), 0u);
    EXPECT_EQ((*r)->asmConstraint("r"), nullptr);
    EXPECT_TRUE((*r)->declaredAsmConstraintLetters().empty());
}

TEST(TargetSchema, AsmConstraintsNonArrayRejected) {
    auto r = TargetSchema::loadFromText(
        constraintDoc(R"({"r":"gpr"})"), "<inline>");
    ASSERT_FALSE(r.has_value())
        << "an object keyed by letter would silently drop duplicates "
           "(nlohmann keeps the last) — the array shape is what makes a "
           "duplicate letter detectable at all";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, AsmConstraintRoundTripsThroughEveryAxis) {
    auto r = TargetSchema::loadFromText(
        constraintDoc(R"([
            {"letter":"r","binds":"registerClass","registerClass":"gpr"},
            {"letter":"a","binds":"register","register":"rax"},
            {"letter":"i","binds":"operandKind","operandKind":"imm32"}
        ])"),
        "<inline>");
    ASSERT_TRUE(r.has_value());
    auto const& s = **r;
    ASSERT_EQ(s.asmConstraintCount(), 3u);

    auto const* cr = s.asmConstraint("r");
    ASSERT_NE(cr, nullptr);
    EXPECT_EQ(cr->binds, AsmConstraintBinding::RegisterClass);
    ASSERT_TRUE(cr->registerClass.has_value());
    EXPECT_EQ(*cr->registerClass, TargetRegClass::GPR);
    // ★ THE ARMS `binds` DID NOT NAME MUST BE `nullopt`, never a plausible
    // zero. Ordinal 0 IS `rax` in this fixture and `OperandKindFilter::Reg`
    // is 0 too, so a zero-initialized arm would answer a consumer that
    // forgot to switch with a real-looking register and a real-looking
    // operand kind — the silent-wrong-answer shape.
    EXPECT_FALSE(cr->registerOrdinal.has_value());
    EXPECT_FALSE(cr->operandKind.has_value());

    auto const* ca = s.asmConstraint("a");
    ASSERT_NE(ca, nullptr);
    EXPECT_EQ(ca->binds, AsmConstraintBinding::Register);
    ASSERT_TRUE(ca->registerOrdinal.has_value());
    ASSERT_NE(s.registerInfo(*ca->registerOrdinal), nullptr);
    EXPECT_EQ(s.registerInfo(*ca->registerOrdinal)->name, "rax");
    EXPECT_FALSE(ca->registerClass.has_value());
    EXPECT_FALSE(ca->operandKind.has_value());

    auto const* ci = s.asmConstraint("i");
    ASSERT_NE(ci, nullptr);
    EXPECT_EQ(ci->binds, AsmConstraintBinding::OperandKind);
    ASSERT_TRUE(ci->operandKind.has_value());
    EXPECT_EQ(*ci->operandKind, OperandKindFilter::ImmInt);
    EXPECT_FALSE(ci->registerOrdinal.has_value());
    EXPECT_FALSE(ci->registerClass.has_value());

    EXPECT_EQ(s.declaredAsmConstraintLetters(), "'r', 'a', 'i'");
}

// ★★ THE LINE THIS FACET IS DRAWN ON, PINNED. The modifiers are GNU-asm
// GRAMMAR — they mean the same thing on every processor — so storing them
// here would put a grammar fact in the target file, which is exactly how
// the reverted `asmSyntax` block got in. Such a letter would ALSO silently
// never match: a front end strips modifiers before looking up, so it
// searches for the bare letter. A silent never-match is worse than a reject.
TEST(TargetSchema, AsmConstraintLetterCarryingAModifierIsRejected) {
    for (std::string_view letter : {"=a", "+a", "&a", "%a"}) {
        auto r = TargetSchema::loadFromText(
            oneConstraintRow(std::string{R"({"letter":")"} +
                             std::string{letter} +
                             R"(","binds":"register","register":"rax"})"),
            "<inline>");
        ASSERT_FALSE(r.has_value())
            << letter
            << " carries a GNU-asm modifier — grammar owned by the source "
               "language, never target vocabulary";
        EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
    }
}

TEST(TargetSchema, AsmConstraintLetterCarryingASigilOrSpaceIsRejected) {
    for (std::string_view letter : {"$a", "#a", "a ", " a"}) {
        auto r = TargetSchema::loadFromText(
            oneConstraintRow(std::string{R"({"letter":")"} +
                             std::string{letter} +
                             R"(","binds":"register","register":"rax"})"),
            "<inline>");
        ASSERT_FALSE(r.has_value())
            << letter << " is dialect grammar or a typo, not a letter";
        EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
    }
}

TEST(TargetSchema, AsmConstraintEmptyLetterIsRejected) {
    auto r = TargetSchema::loadFromText(
        oneConstraintRow(
            R"({"letter":"","binds":"register","register":"rax"})"),
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "an empty letter is a half-filled row — omit the ROW to declare "
           "that this target binds nothing";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// Two rows for one letter is an ambiguity the consumer cannot see, and
// silently keeping one of them is how a config key stops meaning anything.
TEST(TargetSchema, AsmConstraintDuplicateLetterIsRejected) {
    auto r = TargetSchema::loadFromText(
        constraintDoc(R"([
            {"letter":"a","binds":"register","register":"rax"},
            {"letter":"a","binds":"registerClass","registerClass":"gpr"}
        ])"),
        "<inline>");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// ★ CASE IS LOAD-BEARING AND MUST NOT BE FOLDED: ✔MEASURED, x86_64 `d` is
// %rdx and `D` is %rdi. A case-insensitive letter table would bind one to
// the other's register — a wrong register, not a diagnostic.
TEST(TargetSchema, AsmConstraintLetterIsCaseSensitive) {
    auto r = TargetSchema::loadFromText(
        constraintDoc(R"([
            {"letter":"a","binds":"register","register":"rax"},
            {"letter":"A","binds":"registerClass","registerClass":"fpr"}
        ])"),
        "<inline>");
    ASSERT_TRUE(r.has_value())
        << "'a' and 'A' are DIFFERENT letters — folding case here would "
           "report a false duplicate and lose one of them";
    ASSERT_NE((*r)->asmConstraint("a"), nullptr);
    ASSERT_NE((*r)->asmConstraint("A"), nullptr);
    EXPECT_EQ((*r)->asmConstraint("a")->binds, AsmConstraintBinding::Register);
    EXPECT_EQ((*r)->asmConstraint("A")->binds,
              AsmConstraintBinding::RegisterClass);
}

TEST(TargetSchema, AsmConstraintUnknownBindingAxisIsRejected) {
    auto r = TargetSchema::loadFromText(
        oneConstraintRow(R"({"letter":"r","binds":"registerFile",
                             "registerClass":"gpr"})"),
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "a free-form axis string would let a typo declare a letter that "
           "binds nothing; a new axis needs core vocabulary, not a string";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, AsmConstraintMissingThePayloadItsAxisNamesIsRejected) {
    auto r = TargetSchema::loadFromText(
        oneConstraintRow(R"({"letter":"a","binds":"register"})"), "<inline>");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MissingField));
}

// ⚠ A SECOND PAYLOAD IS A SECOND ANSWER. Accepting it would silently
// discard one, and the operator who wrote the discarded key would never
// learn it was dead.
TEST(TargetSchema, AsmConstraintCarryingASecondPayloadIsRejected) {
    auto r = TargetSchema::loadFromText(
        oneConstraintRow(R"({"letter":"a","binds":"register","register":"rax",
                             "registerClass":"gpr"})"),
        "<inline>");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, AsmConstraintUnknownRegisterNameIsRejected) {
    auto r = TargetSchema::loadFromText(
        oneConstraintRow(
            R"({"letter":"b","binds":"register","register":"rbx"})"),
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "a dangling name must fail at LOAD, where the target is in hand — "
           "not at some later lookup site with no target to name";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// The sigil-in-a-register-name mistake is the same grammar-into-vocabulary
// error the letter guard catches, one field over.
TEST(TargetSchema, AsmConstraintSigilPrefixedRegisterNameIsRejected) {
    auto r = TargetSchema::loadFromText(
        oneConstraintRow(
            R"({"letter":"a","binds":"register","register":"%rax"})"),
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "`%rax` is the AT&T PRINTING of the register named `rax`; a "
           "target file names registers, never their dialect spellings";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, AsmConstraintUnknownRegisterClassIsRejected) {
    auto r = TargetSchema::loadFromText(
        oneConstraintRow(R"({"letter":"r","binds":"registerClass",
                             "registerClass":"general"})"),
        "<inline>");
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, AsmConstraintUnknownOperandKindIsRejected) {
    auto r = TargetSchema::loadFromText(
        oneConstraintRow(R"({"letter":"i","binds":"operandKind",
                             "operandKind":"immediate"})"),
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "`imm32` is the declared JSON spelling of OperandKindFilter::"
           "ImmInt — the SAME closed vocabulary the encoding variant guards "
           "use, deliberately reused instead of re-minted";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// ★ A LETTER BOUND TO A CLASS THIS TARGET POPULATES WITH NOTHING IS A KNOB
// THAT LIES: it loads clean, reads as support, and resolves to the empty
// set. ✔MEASURED that this is reachable in practice and not a hypothetical
// — x86_64 declares ZERO `vr` registers and arm64 declares ZERO `flags`.
TEST(TargetSchema, AsmConstraintBoundToAnEmptyClassIsRejected) {
    auto r = TargetSchema::loadFromText(
        oneConstraintRow(R"({"letter":"v","binds":"registerClass",
                             "registerClass":"vr"})"),
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "the fixture declares gpr + fpr registers and no vr — a letter "
           "bound to vr would allocate nothing while reading as supported";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

TEST(TargetSchema, AsmConstraintBoundToClassNoneIsRejected) {
    auto r = TargetSchema::loadFromText(
        oneConstraintRow(R"({"letter":"z","binds":"registerClass",
                             "registerClass":"none"})"),
        "<inline>");
    ASSERT_FALSE(r.has_value())
        << "`none` is a spelling in the class table but not a class a letter "
           "can match — a declared-but-dead letter is worse than no letter";
    EXPECT_TRUE(anyHasCode(r.error(), DiagnosticCode::C_MalformedJson));
}

// The INVERSE-FAILURE pin every closed key vocabulary in this file has had
// to earn: `$`-prefixed keys are prose and must survive the rejector. Not
// decorative — every shipped constraint row carries its gcc measurement.
TEST(TargetSchema, AsmConstraintsAcceptDollarPrefixedProse) {
    auto r = TargetSchema::loadFromText(
        oneConstraintRow(R"({"$comment":"measured with gcc -O2 -S",
                             "letter":"a","binds":"register",
                             "register":"rax"})"),
        "<inline>");
    ASSERT_TRUE(r.has_value())
        << "the carve-out must be the `$`-PREFIX predicate, never a literal "
           "\"$comment\" compare";
    EXPECT_EQ((*r)->asmConstraintCount(), 1u);
}

// ═══════════════════════════════════════════════════════════════════════
// The facet against the SHIPPED targets — and the red-on-disable that
// proves the shipped bytes were actually READ.
// ═══════════════════════════════════════════════════════════════════════
//
// ⚠⚠ THESE RUN THROUGH `loadShipped` / `mutateShippedTargetSchemaDoc`,
// WHICH REACH `findShippedConfig` — it reads `DSS_CONFIG_ROOT` and
// otherwise WALKS THE CWD. `dss_add_test` sets that variable, so these
// assertions are only about the intended config tree when the binary runs
// under ctest. A bare `.exe` invocation silently reads whichever tree the
// shell happens to stand in.

TEST(TargetSchema, ShippedTargetsDeclareAsmConstraints) {
    // ⚠⚠ THE SINGLE HIGHEST-RISK LINE IN THE FACET IS THE ROOT-KEY ALLOWLIST
    // ENTRY, and this is the test that makes forgetting it impossible to
    // miss: without `"asmConstraints"` in `kTargetDocumentKeys` the shipped
    // documents would not load at all, and with the entry but no loader the
    // counts below would be zero. Both halves are asserted.
    auto x86 = TargetSchema::loadShipped("x86_64");
    auto arm = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(x86.has_value());
    ASSERT_TRUE(arm.has_value());

    // ✔MEASURED against gcc 13.3.0 — see this file's section header for the
    // probe. The counts are a ratchet: a letter added or dropped updates
    // this deliberately, never silently.
    EXPECT_EQ((*x86)->asmConstraintCount(), 11u);
    EXPECT_EQ((*arm)->asmConstraintCount(), 4u);
    EXPECT_EQ((*x86)->declaredAsmConstraintLetters(),
              "'r', 'a', 'b', 'c', 'd', 'S', 'D', 'x', 'q', 'm', 'i'");
    EXPECT_EQ((*arm)->declaredAsmConstraintLetters(), "'r', 'w', 'm', 'i'");

    // ★★ `q` IS PRESENT AND `Q` IS ABSENT, AND THE ASYMMETRY IS A
    // MEASUREMENT, NOT A READING OF THE LETTERS' NAMES — an earlier draft of
    // this facet refused BOTH, on the strength of `q` being "the
    // byte-addressable subclass" on i386. ✔THE PROBE counts how many
    // SIMULTANEOUS live operands each letter admits, i.e. the size of its
    // register set, read off where gcc says "impossible constraints":
    // r=15, q=15, Q=4, a=1. On x86_64 REX makes every GPR byte-addressable,
    // so `q` and `r` are the same set and `q`→`gpr` is EXACT; `Q`=4 is a
    // genuine subset that `TargetRegClass` cannot express, so it is refused
    // by name rather than approximated as `gpr` (which would hand the
    // allocator 11 registers gcc forbids). `a`=1 is the control proving the
    // probe can see a single-register pin.
    auto const* q = (*x86)->asmConstraint("q");
    ASSERT_NE(q, nullptr);
    ASSERT_TRUE(q->registerClass.has_value());
    EXPECT_EQ(*q->registerClass, TargetRegClass::GPR);
    EXPECT_EQ((*x86)->asmConstraint("Q"), nullptr)
        << "`Q` names 4 registers; declaring it `gpr` would be a knob that "
           "lies, and a letter this file cannot describe EXACTLY is one it "
           "refuses by name";
    EXPECT_EQ((*arm)->asmConstraint("x"), nullptr)
        << "aarch64 `x` is V0-V15 — ✔MEASURED, it fails at 18 simultaneous "
           "operands where `w` compiles the same 18; a subset `vr` cannot "
           "express";
    EXPECT_EQ((*arm)->asmConstraint("y"), nullptr)
        << "aarch64 `y` is V0-V7 — ✔MEASURED, it fails at 10";

    // The row sqlite's own `hwtime.h` needs: `__asm__("rdtsc" : "=a"(lo),
    // "=d"(hi))`. ✔MEASURED to PIN — with three competing live `"r"`
    // operands the `"=a"` output still landed in %rax.
    auto const* a = (*x86)->asmConstraint("a");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->binds, AsmConstraintBinding::Register);
    ASSERT_TRUE(a->registerOrdinal.has_value());
    EXPECT_EQ((*x86)->registerInfo(*a->registerOrdinal)->name, "rax");

    auto const* d = (*x86)->asmConstraint("d");
    ASSERT_NE(d, nullptr);
    ASSERT_TRUE(d->registerOrdinal.has_value());
    EXPECT_EQ((*x86)->registerInfo(*d->registerOrdinal)->name, "rdx");

    // ★ CASE, ON THE REAL CONFIG: `d` is %rdx and `D` is %rdi.
    auto const* dUpper = (*x86)->asmConstraint("D");
    ASSERT_NE(dUpper, nullptr);
    ASSERT_TRUE(dUpper->registerOrdinal.has_value());
    EXPECT_EQ((*x86)->registerInfo(*dUpper->registerOrdinal)->name, "rdi");
    EXPECT_NE(*d->registerOrdinal, *dUpper->registerOrdinal)
        << "one letter of case apart, two different registers — a "
           "case-folding lookup would silently bind rdx traffic to rdi";

    // arm64's `w` is the V file, NOT the D file. ✔MEASURED and it is the
    // non-obvious half: gcc prints `v0` for a `w`-constrained DOUBLE, so
    // `fpr` would have been the plausible wrong answer. The `d0`/`s0`
    // spellings come from the `%d`/`%s` operand modifiers, which are
    // dialect grammar and do not live in this file.
    auto const* w = (*arm)->asmConstraint("w");
    ASSERT_NE(w, nullptr);
    ASSERT_TRUE(w->registerClass.has_value());
    EXPECT_EQ(*w->registerClass, TargetRegClass::VR);
}

// ★★★ THE OPERATOR-REQUIRED TEST: THE SAME LETTER RESOLVES TO DIFFERENT
// REGISTERS UNDER TWO TARGETS, WHICH IS ONLY POSSIBLE IF THE MAPPING IS
// CONFIG AND NOT CODE. A C++ table would have to branch on the
// architecture to produce these two answers.
TEST(TargetSchema, SameAsmConstraintLetterResolvesDifferentlyPerTarget) {
    auto x86 = TargetSchema::loadShipped("x86_64");
    auto arm = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(x86.has_value());
    ASSERT_TRUE(arm.has_value());

    // ── half one: `r` is declared by BOTH and resolves to DISJOINT sets ──
    auto const* rx = (*x86)->asmConstraint("r");
    auto const* ra = (*arm)->asmConstraint("r");
    ASSERT_NE(rx, nullptr);
    ASSERT_NE(ra, nullptr);
    ASSERT_TRUE(rx->registerClass.has_value());
    ASSERT_TRUE(ra->registerClass.has_value());

    auto const xRegs = registersInClass(**x86, *rx->registerClass);
    auto const aRegs = registersInClass(**arm, *ra->registerClass);
    EXPECT_TRUE(xRegs.contains("rax"));
    EXPECT_FALSE(xRegs.contains("x0"));
    EXPECT_TRUE(aRegs.contains("x0"));
    EXPECT_FALSE(aRegs.contains("rax"));

    std::vector<std::string> shared;
    std::ranges::set_intersection(xRegs, aRegs, std::back_inserter(shared));
    EXPECT_TRUE(shared.empty())
        << "letter 'r' must resolve to two disjoint register sets; "
           << shared.size() << " name(s) appear in both, which would mean "
              "one of the targets is describing the other's register file";

    // ── half two: `a` is declared by ONE of them, and the other REFUSES ──
    // ✔MEASURED: aarch64-linux-gnu-gcc rejects `"=a"` with `error:
    // impossible constraint in 'asm'`. That asymmetry is what a shared
    // C++ table could not express without an `if (arch == ...)`.
    ASSERT_NE((*x86)->asmConstraint("a"), nullptr);
    EXPECT_EQ((*arm)->asmConstraint("a"), nullptr)
        << "arm64 has no `a` constraint — declaring one would be inventing "
           "a binding gcc says does not exist";
    EXPECT_NE((*x86)->declaredAsmConstraintLetters().find("'a'"),
              std::string::npos);
    EXPECT_EQ((*arm)->declaredAsmConstraintLetters().find("'a'"),
              std::string::npos)
        << "the consumer's refusal diagnostic renders its valid list from "
           "whichever target it was handed — there is no correct constant "
           "to hand-type, which is the point";

    // ── half three: an axis they AGREE on, so the divergence above is a
    // measurement and not an artefact of the two files being unrelated ──
    auto const* ix = (*x86)->asmConstraint("i");
    auto const* ia = (*arm)->asmConstraint("i");
    ASSERT_NE(ix, nullptr);
    ASSERT_NE(ia, nullptr);
    EXPECT_EQ(ix->operandKind, ia->operandKind)
        << "`i` is an operand FORM on both processors — ✔MEASURED `$7` vs "
           "`7`, and the sigil is dialect grammar, not a different binding";
}

// ★★★ RED-ON-DISABLE AT CONFIG LEVEL, WITH THE MUTANT PROVEN TO HAVE BEEN
// READ. All five fail-closed clauses are discharged here:
//   1. UNIQUE witness — the loader rejects duplicate letters, so exactly
//      one row declares `a` and `asmConstraint("a")` cannot be ambiguous.
//   2. The mutant DIFFERS BYTE-WISE — `mutateShippedTargetSchemaDoc`
//      compares `doc.dump()` before and after and THROWS on a no-op. Not a
//      line or row count: a same-length substitution is exactly this
//      mutation's shape.
//   3. The witness is ABSENT FROM THE MUTANT BY THE SAME MATCHER THE PIN
//      USES — the identical `asmConstraint("a")` call, not a re-derived one.
//   4. The mutant STILL PARSES — asserted, so the red cannot be a parse
//      error dressed up as a facet failure.
//   5. THE MUTATED BYTES WERE READ — and this is the clause a "the pin went
//      red" test would leave undischarged. The observed value TRACKS the
//      mutation: after re-pointing `a` at `rbx` the loaded schema answers
//      `rbx`. A stale, ignored or hardcoded table cannot produce the
//      mutant's own new value; only reading those bytes can.
TEST(TargetSchema, ShippedAsmConstraintRegisterIsReadFromTheDocument) {
    auto shipped = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(shipped.has_value());
    auto const* baseline = (*shipped)->asmConstraint("a");
    ASSERT_NE(baseline, nullptr);
    ASSERT_TRUE(baseline->registerOrdinal.has_value());
    ASSERT_EQ((*shipped)->registerInfo(*baseline->registerOrdinal)->name,
              "rax");

    bool repointed = false;
    auto mutated = dss::test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [&repointed](nlohmann::json& doc) {
            if (!doc.contains("asmConstraints")) return;
            for (auto& row : doc.at("asmConstraints")) {
                if (row.value("letter", std::string{}) == "a") {
                    row["register"] = "rbx";
                    repointed = true;
                }
            }
        });
    ASSERT_TRUE(repointed)
        << "no shipped x86_64 constraint row declares letter 'a' — the "
           "mutation was a no-op and this pin would assert nothing";
    ASSERT_TRUE(mutated.has_value())
        << "the mutant must still PARSE, or the red below would be a parse "
           "error wearing the facet's clothes";

    auto const* seen = (*mutated)->asmConstraint("a");
    ASSERT_NE(seen, nullptr);
    ASSERT_TRUE(seen->registerOrdinal.has_value());
    EXPECT_EQ((*mutated)->registerInfo(*seen->registerOrdinal)->name, "rbx")
        << "the loader must report the MUTANT's register — reporting `rax` "
           "would mean the letter table is hardcoded in C++ and the JSON is "
           "decoration";
    EXPECT_NE((*mutated)->registerInfo(*seen->registerOrdinal)->name, "rax");

    // And the shipped schema is untouched by the mutation — the two answers
    // coexist, which is the whole claim: the mapping is per-document.
    EXPECT_EQ((*shipped)->registerInfo(*baseline->registerOrdinal)->name,
              "rax");
}

TEST(TargetSchema, RemovingAsmConstraintsFromTheShippedDocumentEmptiesIt) {
    bool removed = false;
    auto mutated = dss::test_support::mutateShippedTargetSchemaDoc(
        "arm64", [&removed](nlohmann::json& doc) {
            if (!doc.contains("asmConstraints")) return;
            doc.erase("asmConstraints");
            removed = true;
        });
    ASSERT_TRUE(removed)
        << "arm64 declares no `asmConstraints` — the mutation was a no-op";
    ASSERT_TRUE(mutated.has_value())
        << "the key is OPTIONAL: removing it must load clean, not error";
    EXPECT_EQ((*mutated)->asmConstraintCount(), 0u);
    EXPECT_EQ((*mutated)->asmConstraint("r"), nullptr)
        << "with the key gone every letter must be refused — a non-empty "
           "table here would mean the facet is populated from somewhere "
           "other than the document";
}

// ⚠⚠ THE FAILURE MODE THE ROOT-KEY ALLOWLIST EXISTS TO ABOLISH, PINNED
// FROM THE OTHER SIDE. Before TF-C74 an unknown root key was silently
// ignored, so a misspelled facet name loaded perfectly clean and no-op'd.
// `ShippedTargetsLoadCleanUnderEveryContainerGate` proves `asmConstraints`
// IS in the allowlist; this proves a near-miss is NOT.
TEST(TargetSchema, MisspelledAsmConstraintsRootKeyIsRefused) {
    bool renamed = false;
    auto mutated = dss::test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [&renamed](nlohmann::json& doc) {
            if (!doc.contains("asmConstraints")) return;
            doc["asmConstraint"] = doc.at("asmConstraints");  // singular typo
            doc.erase("asmConstraints");
            renamed = true;
        });
    ASSERT_TRUE(renamed)
        << "x86_64 declares no `asmConstraints` — the mutation was a no-op";
    EXPECT_FALSE(mutated.has_value())
        << "`asmConstraint` (singular) must be REFUSED — silently ignoring "
           "it would leave a target whose whole inline-asm binding is a "
           "knob that lies";
    if (!mutated.has_value()) {
        EXPECT_TRUE(anyHasCode(mutated.error(),
                               DiagnosticCode::C_MalformedJson));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// nop / rdtsc / cntvct — the three opcodes the inline-asm clients need
// ═══════════════════════════════════════════════════════════════════════

TEST(TargetSchema, ShippedTargetsDeclareNop) {
    // ✔MEASURED by assembling a bare `nop` with gcc 13.3.0 and reading it
    // back with objdump: x86_64 `90` (one byte), aarch64 `d503201f`.
    auto x86 = TargetSchema::loadShipped("x86_64");
    auto arm = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(x86.has_value());
    ASSERT_TRUE(arm.has_value());

    auto const xOp = (*x86)->opcodeByMnemonic("nop");
    ASSERT_TRUE(xOp.has_value());
    auto const* xInfo = (*x86)->opcodeInfo(*xOp);
    ASSERT_NE(xInfo, nullptr);
    EXPECT_EQ(xInfo->maxOperands, 0);
    ASSERT_EQ(xInfo->encoding.variants.size(), 1u);
    EXPECT_EQ(xInfo->encoding.variants[0].tmpl.opcodeBytes,
              (std::vector<std::uint8_t>{0x90}));
    // ★ NOT DECORATION: a `nop` has no result, so a PURE declaration would
    // make it dead by definition and a dead-code pass would be RIGHT to
    // delete the one instruction whose entire purpose is to occupy bytes.
    EXPECT_TRUE(xInfo->hasSideEffects);

    auto const aOp = (*arm)->opcodeByMnemonic("nop");
    ASSERT_TRUE(aOp.has_value());
    auto const* aInfo = (*arm)->opcodeInfo(*aOp);
    ASSERT_NE(aInfo, nullptr);
    EXPECT_EQ(aInfo->maxOperands, 0);
    ASSERT_EQ(aInfo->encoding.variants.size(), 1u);
    EXPECT_EQ(aInfo->encoding.variants[0].tmpl.fixedWord, 0xD503201Fu);
    EXPECT_TRUE(aInfo->hasSideEffects);
}

TEST(TargetSchema, ShippedX86_64DeclaresRdtscWithItsRegisterPair) {
    auto x86 = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(x86.has_value());
    auto const op = (*x86)->opcodeByMnemonic("rdtsc");
    ASSERT_TRUE(op.has_value());
    auto const* info = (*x86)->opcodeInfo(*op);
    ASSERT_NE(info, nullptr);

    // ✔MEASURED with gcc + objdump: `0f 31`, two bytes, no ModR/M, no
    // operands, and NO REX.W (a 64-bit RDTSC form does not exist).
    EXPECT_EQ(info->maxOperands, 0);
    ASSERT_EQ(info->encoding.variants.size(), 1u);
    EXPECT_EQ(info->encoding.variants[0].tmpl.opcodeBytes,
              (std::vector<std::uint8_t>{0x0F, 0x31}));

    // ★ RDTSC is not a function of its (empty) operand list: two reads must
    // return two values. Declared pure, CSE folds the pair into one read
    // and every elapsed-time measurement in the program becomes ZERO.
    EXPECT_TRUE(info->hasSideEffects);

    // The value arrives in EDX:EAX, which no single result slot can name.
    EXPECT_EQ(info->result, dss::TargetResultRule::None);
    ASSERT_TRUE(info->implicitRegisters.has_value());
    EXPECT_EQ(info->implicitRegisters->outputNames,
              (std::vector<std::string>{"rax", "rdx"}));
    EXPECT_EQ(info->implicitRegisters->clobberedNames,
              (std::vector<std::string>{"rax", "rdx"}));

    // ★ ROLES REUSED, NOT MINTED: `high`/`low` already existed for the
    // 128-bit MUL projection and mean exactly what they mean here. They are
    // what stops a reorder of the positional arrays from silently swapping
    // the halves — a swap that multiplies every measured interval by 2^32
    // while still producing a plausible-looking number.
    auto const& roles = info->implicitRegisters->outputRoleOrdinals;
    auto ordinalOfRole = [&roles](std::string_view role)
        -> std::optional<std::uint16_t> {
        for (auto const& [name, ord] : roles) {
            if (name == role) return ord;
        }
        return std::nullopt;
    };
    auto const low  = ordinalOfRole("low");
    auto const high = ordinalOfRole("high");
    ASSERT_TRUE(low.has_value());
    ASSERT_TRUE(high.has_value());
    EXPECT_EQ((*x86)->registerInfo(*low)->name, "rax");
    EXPECT_EQ((*x86)->registerInfo(*high)->name, "rdx");
}

TEST(TargetSchema, ShippedArm64DeclaresCntvctDistinctFromTlsbase) {
    auto arm = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(arm.has_value());

    auto const op = (*arm)->opcodeByMnemonic("cntvct");
    ASSERT_TRUE(op.has_value());
    auto const* info = (*arm)->opcodeInfo(*op);
    ASSERT_NE(info, nullptr);

    // ✔MEASURED, aarch64-linux-gnu-gcc 13.3.0 + objdump, four destination
    // registers assembled together so the fixed word separates cleanly from
    // the Rd field: mrs x0 → d53be040, x1 → d53be041, x7 → d53be047,
    // x30 → d53be05e. ⇒ base 0xD53BE040 with Rd in bits 0..4.
    EXPECT_EQ(info->maxOperands, 0);
    ASSERT_EQ(info->encoding.variants.size(), 1u);
    EXPECT_EQ(info->encoding.variants[0].tmpl.fixedWord, 0xD53BE040u);
    ASSERT_TRUE(info->encoding.variants[0].resultSlot.has_value());
    EXPECT_EQ(*info->encoding.variants[0].resultSlot, dss::EncodingSlotKind::Rd)
        << "the STANDARD rd slot — zero new slot vocabulary, exactly as the "
           "tlsbase row does it";

    // ★★ THE TWO MRS ROWS DIFFER IN EXACTLY ONE PLACE, AND IT MATTERS.
    // Same instruction, same slot, adjacent CRm (13 vs 14) — and OPPOSITE
    // side-effect declarations. A thread's TLS base is invariant, so
    // folding two reads of it is a legitimate optimization; a counter is
    // not, and folding two reads of THAT makes every measured interval
    // exactly zero. The flag is the only thing separating a value you may
    // cache from one you may not.
    auto const tls = (*arm)->opcodeByMnemonic("tlsbase");
    ASSERT_TRUE(tls.has_value());
    auto const* tlsInfo = (*arm)->opcodeInfo(*tls);
    ASSERT_NE(tlsInfo, nullptr);
    EXPECT_EQ(tlsInfo->encoding.variants[0].tmpl.fixedWord, 0xD53BD040u);
    EXPECT_NE(info->encoding.variants[0].tmpl.fixedWord,
              tlsInfo->encoding.variants[0].tmpl.fixedWord);
    EXPECT_TRUE(info->hasSideEffects);
    EXPECT_FALSE(tlsInfo->hasSideEffects);

    // ★ NOT NAMED `mrs`. `tlsbase` is ALSO an MRS, so a row claiming the
    // generic name while encoding one hardcoded system register — with no
    // slot in which to name another — would make `opcodeByMnemonic("mrs")`
    // resolve to something that can only ever read CNTVCT_EL0.
    EXPECT_FALSE((*arm)->opcodeByMnemonic("mrs").has_value())
        << "`mrs` must stay unclaimed for a genuine general MRS with a "
           "system-register slot, if a consumer ever justifies one";
    // ⚠ And it is NOT a cycle counter — CNTVCT_EL0 is a fixed-frequency
    // system counter. `cyclecounter` would have been the plausible wrong
    // name; code dividing it by the CPU clock is wrong on this target.
    EXPECT_FALSE((*arm)->opcodeByMnemonic("cyclecounter").has_value());
}

// ★★★ THE REGISTER TABLE IS CASE-SENSITIVE, AND THAT IS THE LOAD-BEARING HALF
// OF D-ASM-DIALECT-MNEMONIC-MATCH-IS-CASE-SENSITIVE'S DESIGN.
//
// gas accepts `MOV X0, X1`, so DSS must too — but the fold belongs to the
// DIALECT, not to the CPU. One processor can be written in two dialects with
// different case rules (AT&T and Intel are already two dialects for this same
// target), so a case-insensitive register table would push a dialect fact into
// the target description and every dialect reading that target would inherit
// it. The engine therefore folds the written spelling under
// `assembly.spellingCase` and then asks this table, which compares exactly.
//
// ⚠ THIS PIN IS WHAT STOPS THE OBVIOUS "FIX". Making `registerByName`
// case-insensitive would turn the uppercase tests in `tests/asm/` green while
// silently relocating the policy — and nothing else in the tree would notice.
TEST(TargetSchema, RegisterByNameStaysCaseSensitiveOnBothShippedTargets) {
    struct Probe {
        std::string_view target;
        std::string_view declared;   // as the shipped table spells it
        std::string_view shouted;
    };
    // ✔MEASURED by reading both shipped documents: every `registers[].name` and
    // every `opcodes[].mnemonic` in x86_64 (65 / 88) and arm64 (129 / 82) is
    // entirely lowercase, with no fold-collisions in either table — which is
    // what makes a lowercase fold direction correct for the dialect side.
    constexpr std::array<Probe, 4> kProbes{{
        {"x86_64", "rax", "RAX"},
        {"x86_64", "eax", "EAX"},
        {"arm64",  "x0",  "X0"},
        {"arm64",  "w0",  "W0"},
    }};
    for (auto const& p : kProbes) {
        auto sch = TargetSchema::loadShipped(p.target);
        ASSERT_TRUE(sch.has_value()) << p.target;
        EXPECT_TRUE((*sch)->registerByName(p.declared).has_value())
            << p.target << ": the shipped table must declare '" << p.declared
            << "', or this pin is asserting nothing";
        EXPECT_FALSE((*sch)->registerByName(p.shouted).has_value())
            << p.target << ": '" << p.shouted
            << "' resolved — the CASE POLICY has moved into the CPU "
               "description, where a second dialect for this target could no "
               "longer choose a different one";
    }
}

// ★ THE SAME BOUNDARY FOR THE OPCODE TABLE. A dialect row names target opcodes
// by string (`opcodes: ["mov", "load", "store"]`), and that is CONFIG-TO-CONFIG
// vocabulary — never source text — so it is not a folded surface either.
TEST(TargetSchema, OpcodeByMnemonicStaysCaseSensitive) {
    auto sch = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(sch.has_value());
    EXPECT_TRUE((*sch)->opcodeByMnemonic("mov").has_value());
    EXPECT_FALSE((*sch)->opcodeByMnemonic("MOV").has_value())
        << "the opcode table folded — a dialect row naming 'MOV' would then "
           "resolve, which is a config typo silently accepted";
}

// ─────────────────────────────────────────────────────────────────────────
// `contentDigest()` — the retained content digest
// ─────────────────────────────────────────────────────────────────────────
//
// `TargetSchema::loadFromText` retains the lowercase 64-hex SHA-256 of the
// EXACT document bytes it was handed, computed at the one chokepoint where
// those bytes are already in memory. It exists so the runtime-object cache can
// key on the config a build actually LOADED without re-walking
// `src/dss-config/` from disk — ~165 ms per invocation, MEASURED 2026-08-17
// (86 files, 2,078,133 bytes; I/O-dominated: walk+read 152-160 ms, hash only
// 9-13 ms), which would be paid on every build.
//
// ★★ THE ONE-BYTE ARM IS BUILT AT EQUAL LENGTH, AND THAT IS THE POINT OF IT.
// A "digest" that had quietly become a size or length stamp would sail through
// a mutation test whose two inputs differ in SIZE — and the mutation this cache
// has to tell apart is exactly the equal-length kind (MEASURED: a real
// descriptor mutation was 9149 bytes before AND after). So the fixture ASSERTS
// equal length and EXACTLY ONE differing byte rather than merely being
// constructed that way, and it perturbs a byte of STRUCTURAL JSON WHITESPACE so
// the two documents PARSE IDENTICALLY — the digest cannot then be coming from
// anything the parser produced.
//
// ★ The fixture is the SHIPPED descriptor's real bytes, resolved through the
// same `findShippedConfig` the compiler's own `loadShipped` uses. A digest is a
// claim about BYTES, so a hand-authored stand-in would pin the digest of a
// document nothing ever loads.

namespace {

// Lowercase-hex render, written here rather than reached for from
// `dss::crypto::toHexLower`: an oracle that shares code with the subject
// cannot witness the subject.
[[nodiscard]] std::string hexOracle(std::array<std::uint8_t, 32> const& digest) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (std::uint8_t const byte : digest) {
        out.push_back(kHexDigits[byte >> 4]);
        out.push_back(kHexDigits[byte & 0x0fu]);
    }
    return out;
}

// SHA-256 over `text`'s exact bytes — the INDEPENDENT expectation the retained
// digest is pinned against, computed here and never read back off the schema.
[[nodiscard]] std::string digestOracle(std::string_view text) {
    return hexOracle(::dss::crypto::sha256(std::span<std::uint8_t const>{
        reinterpret_cast<std::uint8_t const*>(text.data()), text.size()}));
}

// Number of positions at which two strings differ; `npos` if their lengths do
// (so a length change can never be mistaken for a one-byte change).
[[nodiscard]] std::size_t differingBytes(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return std::string_view::npos;
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) ++n;
    }
    return n;
}

// `text` with its first LF turned into a space: same length, one byte
// different, and provably the same document to the parser — JSON forbids a raw
// newline inside a string, so every LF in a valid document is structural
// whitespace and a space is its equal in every position it can occupy.
[[nodiscard]] std::string withOneWhitespaceByteChanged(std::string_view text) {
    std::string out{text};
    auto const pos = out.find('\n');
    EXPECT_NE(pos, std::string::npos)
        << "the fixture carries no LF to perturb — reach for another "
           "same-length mutation rather than dropping this arm";
    if (pos != std::string::npos) out[pos] = ' ';
    return out;
}

// The shipped descriptor's bytes, read the way `loadFromFile` reads them
// (binary, no newline translation) from the path `loadShipped` would resolve.
[[nodiscard]] std::string shippedTargetText(std::string_view targetName) {
    auto pathR = ::dss::findShippedConfig(::dss::ShippedConfigLocator{
        targetName, "targets", ".target.json", "target",
        ::dss::DiagnosticCode::C_InvalidTargetName});
    EXPECT_TRUE(pathR.has_value())
        << "cannot resolve shipped target '" << targetName << "'";
    if (!pathR.has_value()) return {};
    std::ifstream in{*pathR, std::ios::binary};
    EXPECT_TRUE(in.is_open()) << pathR->string();
    std::ostringstream buf;
    buf << in.rdbuf();
    return std::move(buf).str();
}

}  // namespace

TEST(TargetSchemaContentDigest, IsSixtyFourLowercaseHexDigits) {
    std::string const text = shippedTargetText("x86_64");
    ASSERT_FALSE(text.empty());
    auto r = TargetSchema::loadFromText(text, "<digest-fixture>");
    ASSERT_TRUE(r.has_value());
    auto const digest = (*r)->contentDigest();
    EXPECT_EQ(digest.size(), 64u);
    EXPECT_EQ(digest.find_first_not_of("0123456789abcdef"),
              std::string_view::npos)
        << "not lowercase hex: " << digest;
}

TEST(TargetSchemaContentDigest, SameTextTwiceYieldsTheSameDigest) {
    std::string const text = shippedTargetText("x86_64");
    ASSERT_FALSE(text.empty());
    auto a = TargetSchema::loadFromText(text, "<digest-fixture>");
    auto b = TargetSchema::loadFromText(text, "<digest-fixture>");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_NE(a->get(), b->get())
        << "the two loads returned the SAME object, so an equal digest would "
           "be a tautology rather than a determinism claim";
    EXPECT_EQ((*a)->contentDigest(), (*b)->contentDigest());
}

TEST(TargetSchemaContentDigest, OneByteAtEqualLengthChangesTheDigest) {
    std::string const original = shippedTargetText("x86_64");
    ASSERT_FALSE(original.empty());
    std::string const perturbed = withOneWhitespaceByteChanged(original);

    ASSERT_EQ(original.size(), perturbed.size())
        << "the two inputs must be the SAME LENGTH, or a size stamp would "
           "pass this test";
    ASSERT_EQ(differingBytes(original, perturbed), 1u);

    auto a = TargetSchema::loadFromText(original, "<digest-fixture>");
    auto b = TargetSchema::loadFromText(perturbed, "<digest-fixture>");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());

    // Parse-identical — the perturbed byte was JSON whitespace …
    EXPECT_EQ((*a)->name(), (*b)->name());
    EXPECT_EQ((*a)->version(), (*b)->version());
    EXPECT_EQ((*a)->opcodes().size(), (*b)->opcodes().size());
    // … and still byte-distinguishable, which is the whole contract.
    EXPECT_NE((*a)->contentDigest(), (*b)->contentDigest());
}

TEST(TargetSchemaContentDigest, EqualsAnIndependentSha256OfTheLoadedBytes) {
    std::string const text = shippedTargetText("x86_64");
    ASSERT_FALSE(text.empty());
    auto r = TargetSchema::loadFromText(text, "<digest-fixture>");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->contentDigest(), digestOracle(text));
}

// The file route lands the SAME digest as handing the same bytes to
// `loadFromText` directly — `loadShipped` → `loadFromFile` → `loadFromText` is
// the path every real load takes, and it must not acquire a different key.
TEST(TargetSchemaContentDigest, LoadShippedDigestsTheFileItRead) {
    std::string const text = shippedTargetText("x86_64");
    ASSERT_FALSE(text.empty());
    auto shipped = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(shipped.has_value());
    EXPECT_EQ((*shipped)->contentDigest(), digestOracle(text));
}

// ⚠ EMPTY MEANS UNKNOWN, NEVER WRONG. The public `TargetSchemaData` ctor is the
// documented bypass, and it has no document bytes to digest. An empty digest is
// a DETECTABLE unknown a cache can refuse to key on; a fabricated or inherited
// one is a silent wrong key.
TEST(TargetSchemaContentDigest, ConstructionBypassingLoadFromTextLeavesItEmpty) {
    TargetSchema const schema{::dss::detail::TargetSchemaData{}};
    EXPECT_TRUE(schema.contentDigest().empty())
        << "a schema with no document bytes reported a digest: "
        << schema.contentDigest();
}
