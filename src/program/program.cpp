#include "program/program.hpp"
#include "gen/link/targets/windows/target_windows_x86_64.hpp"
#include "lsp/lsp_server.hpp"
#include "lsp/schema_cache.hpp"
#include "lsp/thread_pool.hpp"
#include "lsp/transport.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace dss {

namespace {

// LSP mode is invoked by `dss-code-prime --lsp [--schema-dir=PATH]`.
// Order-independent.
struct LspFlags {
    bool                                 lspMode = false;
    std::optional<std::filesystem::path> schemaDir;
    std::vector<std::string>             unknown;
};

[[nodiscard]] LspFlags parseLspFlags(int argc, char* argv[]) {
    LspFlags out;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        if (a == "--lsp") {
            out.lspMode = true;
        } else if (a.starts_with("--schema-dir=")) {
            out.schemaDir = std::filesystem::path{
                std::string{a.substr(std::string_view{"--schema-dir="}.size())}};
        } else if (a.starts_with("--")) {
            // Track unknown long-flags so a typo (`--Lsp`) doesn't
            // silently fall through into a different code path.
            out.unknown.emplace_back(a);
        }
    }
    return out;
}

[[nodiscard]] int runLspMode(LspFlags const& flags) {
    lsp::SchemaCache cache{flags.schemaDir};
    auto transport = std::make_unique<lsp::StdioTransport>();
    // Use hardware concurrency, clamped to [1, 8]. The clamp avoids
    // spawning 64-thread pools on big servers — the parser is CPU-
    // bound on small files, so 4-8 workers is the sweet spot.
    const auto hw = static_cast<std::size_t>(std::thread::hardware_concurrency());
    const auto workers = std::clamp<std::size_t>(hw == 0 ? 4 : hw, 1, 8);
    auto executor = std::make_unique<lsp::ThreadPool>(workers);
    lsp::LspServer server{std::move(transport), std::move(executor), cache};
    return server.run();
}

} // namespace

int Program::run(int argc, char* argv[]) {
    const auto flags = parseLspFlags(argc, argv);
    if (flags.lspMode) {
        return runLspMode(flags);
    }
    // Reject unknown long-flags loudly when no positional/legacy
    // mode is taking over (--demo-gui consumes argv[1] directly).
    const bool legacyMode = argc >= 2 && std::string(argv[1]) == "--demo-gui";
    if (!flags.unknown.empty() && !legacyMode) {
        for (auto const& u : flags.unknown) {
            std::cerr << "unrecognized flag: " << u << std::endl;
        }
        std::cerr << "usage: dss-code-prime [--lsp [--schema-dir=PATH]]"
                     " | [--demo-gui [OUTDIR]]" << std::endl;
        return 2;
    }

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
