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
// extraction, not from this struct. Sharing it eliminates verbatim
// duplication across schema loaders and makes "did this loader
// register an error?" the same question across every config kind.

namespace dss::substrate {

class DiagnosticCollector {
public:
    DiagnosticCollector() = default;

    // Construct a fresh diagnostic with a default Error severity.
    // The single emit pathway is the load-bearing invariant
    // `hasErrors()` depends on: it guarantees severity is set
    // explicitly (not default-constructed to whatever enum slot 0
    // happens to be).
    void emit(DiagnosticCode code, std::string path, std::string message,
              DiagnosticSeverity sev = DiagnosticSeverity::Error) {
        diagnostics_.push_back({code, sev, std::move(path), std::move(message)});
    }

    // Forward an already-constructed diagnostic (e.g. from a downstream
    // validator that returns `vector<ConfigDiagnostic>`). The severity
    // is whatever the source set — `hasErrors()` will respect it. Use
    // this in place of direct field access so the encapsulation
    // invariant ("severity scan is the single source of truth") holds.
    void emitRaw(ConfigDiagnostic problem) {
        diagnostics_.push_back(std::move(problem));
    }

    [[nodiscard]] bool hasErrors() const noexcept {
        for (auto const& d : diagnostics_) {
            if (d.severity == DiagnosticSeverity::Error) return true;
        }
        return false;
    }

    [[nodiscard]] bool empty() const noexcept { return diagnostics_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return diagnostics_.size(); }

    // Read-only access for the rare caller that needs to inspect
    // the accumulated diagnostics without taking ownership (e.g.
    // mid-load decisions like "stop on first fatal").
    [[nodiscard]] std::vector<ConfigDiagnostic> const& view() const noexcept {
        return diagnostics_;
    }

    // Extract the accumulated diagnostics into the loader's return
    // path. Rvalue-qualified so the collector is no longer usable
    // after release — prevents accidental reuse of a "drained" bag.
    [[nodiscard]] std::vector<ConfigDiagnostic> release() && noexcept {
        return std::move(diagnostics_);
    }

private:
    std::vector<ConfigDiagnostic> diagnostics_;
};

} // namespace dss::substrate
