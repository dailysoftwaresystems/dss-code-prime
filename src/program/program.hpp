#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "program/input_resolver.hpp"

#include <string>
#include <vector>

namespace dss {

class DSS_EXPORT Program {
public:
    Program() = default;
    ~Program() = default;

    /// Entry point for the CLI. Parses arguments and dispatches compilation.
    int run(int argc, char* argv[]);

    /// Programmatic entry point for embedding (DLL consumers).
    /// Compile a project file (.dsp).
    int compileProject(const std::string& projectFilePath);

    /// Programmatic entry point for embedding (DLL consumers).
    /// Compile explicit source files for a language to one or more targets.
    int compileFiles(
        const std::vector<std::string>& sourceFiles,
        const std::string& languageName,
        const std::vector<std::string>& targets
    );

    /// Policy-aware overload (LK10 cycle 3 — D-LK10-7 closure). The
    /// `reporterConfig` is applied to the run-wide DiagnosticReporter,
    /// so `--warnings-as-errors` + `--suppress=<code>` propagate through
    /// every tier's drain.
    int compileFiles(
        const std::vector<std::string>& sourceFiles,
        const std::string& languageName,
        const std::vector<std::string>& targets,
        DiagnosticReporter::Config const& reporterConfig
    );

    /// Programmatic entry point for embedding (DLL consumers).
    /// Compile all matching source files in a directory (recursive scan).
    int compileDirectory(
        const std::string& directoryPath,
        const std::string& languageName,
        const std::vector<std::string>& targets
    );

    /// Policy-aware overload with explicit recursion policy (LK10
    /// cycle 3 — D-LK10-1 closure: --recursive / --no-recursive is
    /// the second policy axis that triggered the InputResolver hoist).
    int compileDirectory(
        const std::string& directoryPath,
        const std::string& languageName,
        const std::vector<std::string>& targets,
        InputResolver::Mode mode,
        DiagnosticReporter::Config const& reporterConfig
    );

    /// Source-to-source transpilation entry point — plan 10 owns
    /// the actual translation engine (`*.map.json` + HIR→HIR walker
    /// + target-CST builder + pretty-printer). v1: fails loud with
    /// `D_PlanNotLanded` citing plan 10. Plan 10 ships the engine
    /// behind this API as ST1..ST6.
    int transpile(
        const std::vector<std::string>& sourceFiles,
        const std::string& languageName,
        const std::vector<std::string>& targets
    );
};

} // namespace dss

// C-compatible API for FFI consumers (Python, C#, etc.)
extern "C" {
    DSS_EXPORT int dss_compile_project(const char* projectFilePath);
    DSS_EXPORT int dss_compile_directory(const char* directoryPath, const char* languageName,
                                          const char** targets, int targetCount);
    DSS_EXPORT const char* dss_version();
}
