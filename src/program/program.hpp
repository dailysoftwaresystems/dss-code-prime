#pragma once

#include "core/export.hpp"
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

    /// Programmatic entry point for embedding (DLL consumers).
    /// Compile all matching source files in a directory.
    int compileDirectory(
        const std::string& directoryPath,
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
