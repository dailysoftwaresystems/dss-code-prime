#include "core/types/parse_diagnostic.hpp"

#include <gtest/gtest.h>

using dss::diagnosticCodeName;
using dss::diagnosticCodePrefix;
using dss::DiagnosticCode;
using dss::DiagnosticSeverity;
using dss::ParseDiagnostic;
using dss::ScopeKind;
using dss::scopeName;
using dss::severityName;

TEST(DiagnosticCode, SymbolicNameRoundtrip) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::P_UnexpectedToken),     "P_UnexpectedToken");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::P_PrematureEndOfInput), "P_PrematureEndOfInput");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::C_MissingField),        "C_MissingField");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::P_TooManyDiagnostics),  "P_TooManyDiagnostics");
    // D_* driver / compilation-unit codes (CU2).
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_FileNotFound),        "D_FileNotFound");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_EmptyInput),          "D_EmptyInput");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_DuplicateFile),       "D_DuplicateFile");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::C_UnknownTypeExtension), "C_UnknownTypeExtension");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::C_TypeExtensionParamMismatch),
              "C_TypeExtensionParamMismatch");
    // H_* HIR verifier / lowering codes (plan 09, HR2 + HR3).
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::H_TypeUnresolved),      "H_TypeUnresolved");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::H_InvalidBreak),        "H_InvalidBreak");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::H_VerifierFailure),     "H_VerifierFailure");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::H_ExternHasInitializer), "H_ExternHasInitializer");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::H_ExternDeclMalformed),  "H_ExternDeclMalformed");
}

TEST(DiagnosticCode, PrefixIsPhaseLetterPlusHexNumber) {
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::P_UnexpectedToken),  "P0001");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::P_AmbiguousToken),   "P0008");
    // 9xxx range stays as 9xxx (not collapsed via the high-nibble strip).
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::P_BuilderInvariant), "P9000");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::P_RecoveryStalled),  "P9003");
    // C_* prefix and the high nibble is stripped for the numeric portion.
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::C_MissingField),     "C0001");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::C_AmbiguousAlternatives), "C0010");
    // D_* prefix; high nibble stripped like C_*.
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::D_FileNotFound),     "D0001");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::D_DuplicateFile),    "D0003");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::C_UnknownTypeExtension),       "C002A");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::C_TypeExtensionParamMismatch), "C002B");
    // H_* prefix; the 0xF high nibble renders as 'H' and is stripped from the
    // numeric portion (like C/D/S).
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::H_TypeUnresolved),   "H0001");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::H_InvalidBreak),     "H0002");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::H_VerifierFailure),  "H0003");
}

TEST(DiagnosticSeverity, NameMapping) {
    EXPECT_EQ(severityName(DiagnosticSeverity::Hint),    "hint");
    EXPECT_EQ(severityName(DiagnosticSeverity::Info),    "info");
    EXPECT_EQ(severityName(DiagnosticSeverity::Warning), "warning");
    EXPECT_EQ(severityName(DiagnosticSeverity::Error),   "error");
}

TEST(ParseDiagnostic, DefaultsAreSensible) {
    ParseDiagnostic d;
    EXPECT_EQ(d.code, DiagnosticCode::None);
    EXPECT_EQ(d.severity, DiagnosticSeverity::Error);
    EXPECT_FALSE(d.ruleContext.has_value());
    EXPECT_TRUE(d.expected.empty());
    EXPECT_TRUE(d.scopeStack.empty());
    EXPECT_TRUE(d.related.empty());
    EXPECT_TRUE(d.actual.empty());
}

TEST(ScopeKind, NameMapping) {
    EXPECT_EQ(scopeName(ScopeKind::None),    "None");
    EXPECT_EQ(scopeName(ScopeKind::Root),    "Root");
    EXPECT_EQ(scopeName(ScopeKind::Block),   "Block");
    EXPECT_EQ(scopeName(ScopeKind::Generic), "Generic");
    EXPECT_EQ(scopeName(static_cast<ScopeKind>(2048)), "Custom");
}
