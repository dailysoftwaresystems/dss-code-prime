#pragma once

#include "core/types/grammar_schema.hpp"   // ConfigDiagnostic
#include "core/types/parse_diagnostic.hpp"

#include <string>
#include <utility>
#include <vector>

// Loader-side diagnostic accumulator used by every JSON config
// loader (TargetSchema, GrammarSchema, ObjectFormatSchema, ...).
//
// The shape is intentionally trivial — the loaders' value comes from
// their cross-field validate() rules and the loader-specific field
// extraction, not from this struct. Sharing it ELIMINATES the
// 3-copy verbatim duplication between schema loaders + makes
// "did this loader register an error?" the same question across
// every config kind. Refactor target: code-simplifier review of
// AS6 + LK4 (Finding 1).

namespace dss::substrate {

struct DiagnosticCollector {
    std::vector<ConfigDiagnostic> diagnostics;

    void emit(DiagnosticCode code, std::string path, std::string message,
              DiagnosticSeverity sev = DiagnosticSeverity::Error) {
        diagnostics.push_back({code, sev, std::move(path), std::move(message)});
    }

    [[nodiscard]] bool hasErrors() const noexcept {
        for (auto const& d : diagnostics) {
            if (d.severity == DiagnosticSeverity::Error) return true;
        }
        return false;
    }
};

} // namespace dss::substrate
