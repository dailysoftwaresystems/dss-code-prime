#include "program/model_to_hir.hpp"

#include "analysis/semantic/semantic_model.hpp"
#include "core/substrate/phase_timers.hpp"   // the LowerHir window, owned here
#include "core/types/diagnostic_reporter.hpp"
#include "hir/attributes/visibility_written_as.hpp"   // settleVisibilitiesWrittenAs (P69 fold f26b)
#include "link/object_format_schema.hpp"

#include <string>

namespace dss {

ModelLoweredToHir lowerModelToHir(SemanticModel&            model,
                                  ObjectFormatSchema const& format,
                                  DiagnosticReporter&       reporter) {
    ModelLoweredToHir out;
    // The model gate: a model that carries an error is never lowered. (The driver
    // stops earlier still when its reporter's policy promotes one of the model's
    // warnings to an error — a fact of the driver's reporter, decided where the
    // model's diagnostics are copied onto it.)
    if (model.hasErrors()) return out;

    // CST → HIR. The gate after it is the snapshot rule every tier of the build
    // keeps (`tierClean`): the HIR lowering is clean when it added no error to the
    // reporter it shares, whatever that reporter carried before.
    auto const hirEntry = reporter.errorCount();
    {
        substrate::PhaseTimers::Scope const phase{substrate::CompilePhase::LowerHir};
        out.hir = lowerToHir(model, reporter);
    }
    if (!out.hir || !out.hir->ok || reporter.errorCount() != hirEntry) return out;

    // P69 fold f26b: a visibility this format cannot write becomes the one its
    // document states, before any tier reads it, and the program is told.
    settleVisibilitiesWrittenAs(*out.hir, format, format.name(),
                                [&](SymbolId s) {
                                    SymbolRecord const* r = model.recordFor(s);
                                    return r != nullptr ? r->name : std::string{};
                                },
                                reporter);
    out.lowersToMir = true;
    return out;
}

}  // namespace dss
