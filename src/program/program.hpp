#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "program/input_resolver.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dss {

class DSS_EXPORT Program {
public:
    Program() = default;
    ~Program() = default;

    /// Entry point for the CLI. Parses arguments and dispatches compilation.
    int run(int argc, char* argv[]);

    /// Compile a project file (.dsp). `reporterConfig` threads
    /// `--warnings-as-errors` + `--suppress=<code>` through every tier.
    /// (LK10 cycle 3 post-fold #2: overload pair collapsed to single
    /// signature with default — code-simplifier REQUIRED.)
    int compileProject(
        const std::string& projectFilePath,
        DiagnosticReporter::Config const& reporterConfig = {}
    );

    /// Compile explicit source files for a language to one or more
    /// targets. `reporterConfig` is applied run-wide.
    int compileFiles(
        const std::vector<std::string>& sourceFiles,
        const std::string& languageName,
        const std::vector<std::string>& targets,
        DiagnosticReporter::Config const& reporterConfig = {}
    );

    /// Rep-injection overload — caller owns `rep` and may inspect it
    /// after return. The Config-taking overload is a thin wrapper
    /// that constructs `rep` internally and forwards here.
    ///
    /// D-CAP-MARKER-MULTI-TARGET-E2E-PIN (eb2c6c7 audit-fold 2026-06-01):
    /// the single-chokepoint cap-marker contract cannot be pinned
    /// from outside without post-run reporter inspection — the
    /// original Track 3 test became structurally impossible when
    /// `D_TargetMachineCodeMismatch` joined `kUnsuppressableCodes`
    /// (cap-gates bypassed). This overload lets tests reach
    /// `rep.all()` / `countCode(rep, P_TooManyDiagnostics)` directly.
    int compileFiles(
        const std::vector<std::string>& sourceFiles,
        const std::string& languageName,
        const std::vector<std::string>& targets,
        DiagnosticReporter&             rep
    );

    /// Compile every matching source file in a directory.
    /// `mode` selects recursive vs flat scan (D-LK10-1 closure axis).
    int compileDirectory(
        const std::string& directoryPath,
        const std::string& languageName,
        const std::vector<std::string>& targets,
        InputResolver::Mode mode = InputResolver::Mode::Recursive,
        DiagnosticReporter::Config const& reporterConfig = {}
    );

    /// Source-to-source transpilation entry point — plan 10 owns
    /// the actual translation engine (`*.map.json` + HIR→HIR walker
    /// + target-CST builder + pretty-printer). v1: fails loud with
    /// `D_PlanNotLanded` citing plan 10. Plan 10 ships the engine
    /// behind this API as ST1..ST6.
    int transpile(
        const std::vector<std::string>& sourceFiles,
        const std::string& languageName,
        const std::vector<std::string>& targets,
        DiagnosticReporter::Config const& reporterConfig = {}
    );

    /// `--output <dir>` (D-LK10-ENTRY Slice C companion): routes
    /// emitted binaries into the named directory. When set, the
    /// output-path convention becomes `<outputDir>/<binary>` for
    /// single-target builds and `<outputDir>/<formatName>/<binary>`
    /// for multi-target builds (to disambiguate same-named outputs
    /// across formats). When unset (the default), the existing
    /// `<cwd>/target/<formatName>/<binary>` convention applies.
    ///
    /// Caller is responsible for ensuring the directory exists.
    /// The driver does NOT auto-mkdir for `--output` (mirrors the
    /// `writeImage` parent-dir contract — see writer.hpp).
    void setOutputDir(std::optional<std::filesystem::path> dir) {
        outputDir_ = std::move(dir);
    }
    [[nodiscard]] std::optional<std::filesystem::path> const&
    outputDir() const noexcept { return outputDir_; }

private:
    std::optional<std::filesystem::path> outputDir_;
};

} // namespace dss

// C-compatible API for FFI consumers (Python, C#, etc.)
extern "C" {
    DSS_EXPORT int dss_compile_project(const char* projectFilePath);
    DSS_EXPORT int dss_compile_directory(const char* directoryPath, const char* languageName,
                                          const char** targets, int targetCount);
    DSS_EXPORT const char* dss_version();
}
