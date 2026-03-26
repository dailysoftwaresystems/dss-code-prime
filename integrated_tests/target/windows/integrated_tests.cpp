#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Minimal PE header validation
struct PeValidation {
    bool valid = false;
    std::string machine;
    uint16_t numSections = 0;
    uint16_t subsystem = 0;
    uint32_t entryPoint = 0;
    std::string error;
};

PeValidation validatePe(const std::string& path) {
    PeValidation result;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        result.error = "Cannot open file";
        return result;
    }

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());

    if (data.size() < 0x200) {
        result.error = "File too small (" + std::to_string(data.size()) + " bytes)";
        return result;
    }

    // DOS header: "MZ" magic
    if (data[0] != 'M' || data[1] != 'Z') {
        result.error = "Missing MZ signature";
        return result;
    }

    // e_lfanew at offset 0x3C (4 bytes LE)
    uint32_t peOffset = *reinterpret_cast<uint32_t*>(&data[0x3C]);
    if (peOffset + 4 > data.size()) {
        result.error = "Invalid PE offset";
        return result;
    }

    // PE signature: "PE\0\0"
    if (std::memcmp(&data[peOffset], "PE\0\0", 4) != 0) {
        result.error = "Missing PE signature";
        return result;
    }

    // COFF header starts at peOffset + 4
    uint32_t coffOffset = peOffset + 4;
    uint16_t machineType = *reinterpret_cast<uint16_t*>(&data[coffOffset]);
    result.numSections = *reinterpret_cast<uint16_t*>(&data[coffOffset + 2]);

    if (machineType == 0x8664)      result.machine = "AMD64";
    else if (machineType == 0x14C)  result.machine = "i386";
    else if (machineType == 0xAA64) result.machine = "ARM64";
    else result.machine = "unknown(0x" + std::to_string(machineType) + ")";

    // Optional header
    uint16_t optHeaderSize = *reinterpret_cast<uint16_t*>(&data[coffOffset + 16]);
    uint32_t optOffset = coffOffset + 20;

    if (optOffset + optHeaderSize > data.size()) {
        result.error = "Optional header truncated";
        return result;
    }

    uint16_t magic = *reinterpret_cast<uint16_t*>(&data[optOffset]);
    if (magic != 0x020B) {
        result.error = "Not PE32+ (magic=0x" + std::to_string(magic) + ")";
        return result;
    }

    result.entryPoint = *reinterpret_cast<uint32_t*>(&data[optOffset + 16]);
    result.subsystem = *reinterpret_cast<uint16_t*>(&data[optOffset + 68]);

    result.valid = true;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────

int failures = 0;
int passes = 0;

void check(const std::string& name, bool condition, const std::string& detail = "") {
    if (condition) {
        std::cout << "  PASS  " << name << std::endl;
        passes++;
    } else {
        std::cout << "  FAIL  " << name;
        if (!detail.empty()) std::cout << " — " << detail;
        std::cout << std::endl;
        failures++;
    }
}

// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: integrated_tests <path-to-dss-code-prime>" << std::endl;
        return 1;
    }

    std::string compiler = fs::absolute(argv[1]).string();
    fs::path outputBase = fs::temp_directory_path() / "dss-integrated-tests";

    // Clean previous run
    fs::remove_all(outputBase);
    fs::create_directories(outputBase);

    std::cout << "═══ DSS Code Prime — Integration Tests ═══" << std::endl;
    std::cout << "Compiler: " << compiler << std::endl;
    std::cout << "Output:   " << outputBase.string() << std::endl;
    std::cout << std::endl;

    // ═══════════════════════════════════════════════════════════════════════
    // Test 1: Default invocation prints ready message
    // ═══════════════════════════════════════════════════════════════════════

    std::cout << "[Test 1] Default invocation" << std::endl;
    {
        std::string cmd = compiler + " > " +
            (outputBase / "default_output.txt").string() + " 2>&1";
        int rc = std::system(cmd.c_str());
        check("Exit code is 0", rc == 0, "got " + std::to_string(rc));

        std::ifstream f((outputBase / "default_output.txt").string());
        std::string line;
        std::getline(f, line);
        check("Prints ready message",
              line.find("Hello, World! DSS Code Prime compiler ready.") != std::string::npos,
              "got: " + line);
    }
    std::cout << std::endl;

    // ═══════════════════════════════════════════════════════════════════════
    // Test 2: --demo-gui generates a valid Windows PE in correct path
    // ═══════════════════════════════════════════════════════════════════════

    std::cout << "[Test 2] --demo-gui generates Windows PE" << std::endl;
    {
        fs::path guiOutput = outputBase / "gui";
        std::string cmd = compiler + " --demo-gui " + guiOutput.string() +
            " > " + (outputBase / "gui_output.txt").string() + " 2>&1";
        int rc = std::system(cmd.c_str());
        check("Exit code is 0", rc == 0, "got " + std::to_string(rc));

        fs::path exePath = guiOutput / "target" / "windows-x86_64" / "hello.exe";
        check("Output file exists", fs::exists(exePath), exePath.string());

        if (fs::exists(exePath)) {
            auto fileSize = fs::file_size(exePath);
            check("File size is 2048 bytes", fileSize == 2048,
                  "got " + std::to_string(fileSize));

            // Validate PE structure
            auto pe = validatePe(exePath.string());
            check("PE structure is valid", pe.valid, pe.error);

            if (pe.valid) {
                check("Machine is AMD64", pe.machine == "AMD64",
                      "got " + pe.machine);
                check("Has 3 sections", pe.numSections == 3,
                      "got " + std::to_string(pe.numSections));
                check("Subsystem is WINDOWS_GUI (2)", pe.subsystem == 2,
                      "got " + std::to_string(pe.subsystem));
                check("Entry point is 0x1000", pe.entryPoint == 0x1000,
                      "got 0x" + std::to_string(pe.entryPoint));
            }
        }

        // Check output directory structure
        check("target/ directory created",
              fs::is_directory(guiOutput / "target"));
        check("windows-x86_64/ directory created",
              fs::is_directory(guiOutput / "target" / "windows-x86_64"));
    }
    std::cout << std::endl;

    // ═══════════════════════════════════════════════════════════════════════
    // Test 3: --demo-gui with default output directory (current dir)
    // ═══════════════════════════════════════════════════════════════════════

    std::cout << "[Test 3] --demo-gui with default directory" << std::endl;
    {
        fs::path workDir = outputBase / "default-dir-test";
        fs::create_directories(workDir);

        // Change CWD to workDir, run with no output path, then restore
        fs::path originalCwd = fs::current_path();
        fs::current_path(workDir);

        std::string cmd = compiler + " --demo-gui"
            " > " + (outputBase / "gui_default_output.txt").string() + " 2>&1";
        int rc = std::system(cmd.c_str());

        fs::current_path(originalCwd);

        check("Exit code is 0", rc == 0, "got " + std::to_string(rc));

        fs::path exePath = workDir / "target" / "windows-x86_64" / "hello.exe";
        check("Output file exists at ./target/windows-x86_64/hello.exe",
              fs::exists(exePath), exePath.string());
    }
    std::cout << std::endl;

    // ═══════════════════════════════════════════════════════════════════════
    // Summary
    // ═══════════════════════════════════════════════════════════════════════

    std::cout << "═══════════════════════════════════════════" << std::endl;
    std::cout << "Results: " << passes << " passed, " << failures << " failed" << std::endl;

    // Cleanup
    fs::remove_all(outputBase);

    return failures > 0 ? 1 : 0;
}
