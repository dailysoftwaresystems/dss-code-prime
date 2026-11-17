// P69 L2b2 (lane `lm`), ruling 575: THE PIPELINE'S GATES FROM THE ANALYSIS TO HIR→MIR.
//
// `lowerModelToHir` (src/program/model_to_hir.hpp) is the one place the build decides that a
// model carrying an error is never lowered, that a HIR which did not lower cleanly never goes
// on to MIR, and that a HIR which does is first settled for its object format. The build
// (`buildCuHirImpl`) and every test fixture that lowers source to MIR lower through it, so
// no fixture runs a tier the build would never run. ✔MEASURED 2026-11-13: a fixture that
// lowered past the model gate on its own ABORTED its test binary in the MIR tier
// (`TypeInterner::get: TypeId out of range`) on a model holding an unresolved `va_list`,
// which the build refuses with S_UnknownType.
//
// RED-ON-DISABLE, one case per gate: the model gate dropped (a HIR comes back for a model
// with an error); the HIR gate dropped (a HIR that reported an error goes on to MIR); the
// settling dropped (a visibility the format cannot write reaches MIR unsettled, untold).

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "diagnostic_count.hpp"
#include "link/object_format_schema.hpp"
#include "program/model_to_hir.hpp"
#include "shipped_schema_or_throw.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using dss::DiagnosticBudget;
using dss::DiagnosticCode;
using dss::DiagnosticReporter;
using dss::ObjectFormatSchema;
using dss::SemanticModel;
using dss::test_support::countCode;

[[nodiscard]] SemanticModel analyzeC(std::string src) {
    dss::UnitBuilder builder{dss::test_support::shippedSchemaOrThrow("c"),
                             DiagnosticBudget::libraryDefault()};
    builder.addInMemory(std::move(src), "<mem>");
    auto cu = std::make_shared<dss::CompilationUnit>(std::move(builder).finish());
    return dss::analyze(cu, DiagnosticBudget::libraryDefault());
}

[[nodiscard]] std::shared_ptr<ObjectFormatSchema const> format(char const* name) {
    auto f = ObjectFormatSchema::loadShipped(name);
    if (!f) throw std::runtime_error(std::string{"loadShipped("} + name + ") failed");
    return *f;
}

}  // namespace

TEST(ModelToHir, AModelWithAnErrorIsNeverLoweredToHir) {
    // `va_list` with no header in scope is the program's own unknown name.
    SemanticModel model = analyzeC("void take(va_list ap) { }\n");
    ASSERT_GE(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 1u);
    DiagnosticReporter rep;
    auto const lowered = dss::lowerModelToHir(model, *format("elf64-x86_64-linux-exec"), rep);
    EXPECT_EQ(lowered.hir, nullptr) << "HIR is never entered on a model with an error";
    EXPECT_FALSE(lowered.lowersToMir);
    EXPECT_TRUE(rep.all().empty()) << "no tier past the analysis said anything";
}

TEST(ModelToHir, AHirThatReportedAnErrorNeverGoesOnToMir) {
    // A clean model whose HIR lowering refuses: the `sizeof` of a variable-length array
    // reached through a cast is one the HIR lowering cannot size (H_UnsupportedLoweringForKind).
    SemanticModel model = analyzeC("int f(int n) { int x[8] = {0}; return (int)sizeof *(int (*)[n])x; }\n");
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter rep;
    auto const lowered = dss::lowerModelToHir(model, *format("elf64-x86_64-linux-exec"), rep);
    ASSERT_NE(lowered.hir, nullptr) << "HIR is entered on a clean model";
    EXPECT_GE(countCode(rep, DiagnosticCode::H_UnsupportedLoweringForKind), 1u);
    EXPECT_FALSE(lowered.lowersToMir) << "a HIR lowering that reported an error stops the unit";
}

TEST(ModelToHir, ACleanUnitGoesOnToMirSettledForItsFormat) {
    constexpr char const* kProtected =
        "__attribute__((visibility(\"protected\"))) int dss_vp_fn(void) { return 1; }\n";
    {
        SCOPED_TRACE("pe64 writes a protected definition as a default one, and says so");
        SemanticModel model = analyzeC(kProtected);
        ASSERT_FALSE(model.hasErrors());
        DiagnosticReporter rep;
        auto const lowered = dss::lowerModelToHir(model, *format("pe64-x86_64-windows-exec"), rep);
        ASSERT_NE(lowered.hir, nullptr);
        EXPECT_TRUE(lowered.lowersToMir);
        EXPECT_EQ(countCode(rep, DiagnosticCode::H_VisibilityWrittenAsAnother), 1u)
            << "the HIR is settled for its format before any tier reads it";
    }
    {
        SCOPED_TRACE("ELF writes it as it is");
        SemanticModel model = analyzeC(kProtected);
        DiagnosticReporter rep;
        auto const lowered = dss::lowerModelToHir(model, *format("elf64-x86_64-linux-exec"), rep);
        ASSERT_NE(lowered.hir, nullptr);
        EXPECT_TRUE(lowered.lowersToMir);
        EXPECT_EQ(countCode(rep, DiagnosticCode::H_VisibilityWrittenAsAnother), 0u);
    }
}
