#include "program/program.hpp"
#include "gen/link/targets/windows/target_windows_x86_64.hpp"
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace dss {

int Program::run(int argc, char* argv[]) {
    // Temporary: if called with "--demo-gui [output-dir]", generate a demo Windows exe
    // Output goes to: <output-dir>/target/windows-x86_64/hello.exe
    if (argc >= 2 && std::string(argv[1]) == "--demo-gui") {
        std::string baseDir = (argc >= 3) ? argv[2] : ".";

        TargetWindowsX86_64 target;
        fs::path targetDir = fs::path(baseDir) / "target" / target.name();
        fs::create_directories(targetDir);

        fs::path outputPath = targetDir / ("hello" + target.outputExtension());
        if (TargetWindowsX86_64::generateSimpleGui(outputPath.string())) {
            std::cout << "Generated: " << outputPath.string() << std::endl;
            return 0;
        }
        std::cerr << "Failed to generate: " << outputPath.string() << std::endl;
        return 1;
    }

    std::cout << "Hello, World! DSS Code Prime compiler ready." << std::endl;
    return 0;
}

int Program::compileProject(const std::string& projectFilePath) {
    // TODO: implement project file compilation
    return 0;
}

int Program::compileFiles(
    const std::vector<std::string>& sourceFiles,
    const std::string& languageName,
    const std::vector<std::string>& targets
) {
    // TODO: implement file list compilation
    return 0;
}

int Program::compileDirectory(
    const std::string& directoryPath,
    const std::string& languageName,
    const std::vector<std::string>& targets
) {
    // TODO: implement directory compilation
    return 0;
}

} // namespace dss

// C-compatible API implementations
extern "C" {
    int dss_compile_project(const char* projectFilePath) {
        dss::Program program;
        return program.compileProject(projectFilePath);
    }

    int dss_compile_directory(const char* directoryPath, const char* languageName,
                            const char** targets, int targetCount) {
        dss::Program program;
        std::vector<std::string> targetList(targets, targets + targetCount);
        return program.compileDirectory(directoryPath, languageName, targetList);
    }

    const char* dss_version() {
        return "0.1.0";
    }
}
