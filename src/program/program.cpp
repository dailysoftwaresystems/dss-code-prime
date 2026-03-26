#include "program/program.hpp"
#include <iostream>

namespace dss {

int Program::run(int argc, char* argv[]) {
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
