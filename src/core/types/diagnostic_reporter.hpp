#pragma once

#include "core/export.hpp"

// CP2 fills this in: ParseDiagnostic, DiagnosticReporter, DiagnosticPolicy,
// BufferRegistry, formatting helpers. For now an empty class lets Tree hold
// a unique_ptr<DiagnosticReporter> without complete-type issues.

namespace dss {

class DSS_EXPORT DiagnosticReporter {
    // Intentionally empty in checkpoint 1.
    // Plan §5.13–§5.14 specifies the full API; implementation lands in checkpoint 2.
};

} // namespace dss
