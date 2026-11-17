#pragma once
// ══ THE PIPELINE'S GATES FROM THE ANALYSIS TO HIR→MIR, IN ONE PLACE ══
// (P69 L2b2, lane `lm`)
//
// The build never lowers a model that carries an error, and never lowers to MIR a
// HIR that did not lower `ok` or whose lowering reported an error: it stops at
// either, so every later tier is written for an input that passed both. A HIR
// that passes is settled for the format before any tier reads it (a visibility
// the format cannot write becomes the one its document states).
//
// `buildCuHirImpl` (compile_pipeline.cpp) answers this here, and so does every
// test fixture that lowers SOURCE to MIR — a fixture that went past either gate
// on its own ran a tier the build never runs, on an input that tier was never
// written for. ✔MEASURED 2026-11-13: a fixture that lowered a model holding an
// unresolved `va_list` on to HIR and then MIR ABORTED its whole test binary
// (`TypeInterner::get: TypeId out of range`), where the build refuses the same
// source with S_UnknownType and lowers nothing.
//
// (A tier's OWN unit tests are not fixtures of the pipeline: a HIR built by hand
// and handed to `lowerToMir`, or a model handed to `lowerToHir` to pin that tier's
// answer on it, call the tier itself, because the tier is their subject.)

#include "core/export.hpp"
#include "hir/lowering/cst_to_hir.hpp"   // CstToHirResult

#include <memory>

namespace dss {

class DiagnosticReporter;
class ObjectFormatSchema;
class SemanticModel;

struct DSS_EXPORT ModelLoweredToHir {
    // The CST→HIR lowering's result: null when the model carried an error and HIR
    // was never entered.
    std::unique_ptr<CstToHirResult> hir;
    // Both gates passed and the HIR is settled for the format: the build goes on
    // to HIR→MIR. False ⇒ MIR is never entered.
    bool lowersToMir = false;
};

// Lower `model` to HIR behind the pipeline's two gates, reporting on `reporter`
// (the HIR lowering's and the settling's diagnostics; the model's own stay on the
// model). `format` is the object format the unit is built for.
[[nodiscard]] DSS_EXPORT ModelLoweredToHir lowerModelToHir(SemanticModel&            model,
                                                           ObjectFormatSchema const& format,
                                                           DiagnosticReporter&       reporter);

}  // namespace dss
