// DSS Code Prime — integration tests (Windows).
//
// Invokes the compiled `dss-code-prime` executable directly and
// validates user-facing behavior of CLI-level commands. Per-tier
// unit tests live under `tests/` (built via ctest); this binary
// exercises the user-visible CLI surface end-to-end with a real
// subprocess invocation.
//
// History: cycle-pre-2 tested an in-house `--demo-gui` flag that
// produced a hand-rolled Windows PE for plumb-through validation.
// LK10 cycle 2 retires that flag (the real compileFiles pipeline
// is the artifact source); the harness keeps the default-invocation
// smoke test and grows the real CLI compile flags once LK10 cycle
// 3 wires `--compile` / `--target` / `--output` routing.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

int passes  = 0;
int failures = 0;

void check(const std::string& description, bool condition,
           const std::string& detail = "") {
    if (condition) {
        std::cout << "  [PASS] " << description << std::endl;
        passes++;
    } else {
        std::cout << "  [FAIL] " << description;
        if (!detail.empty()) std::cout << " — " << detail;
        std::cout << std::endl;
        failures++;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: integrated_tests <path-to-dss-code-prime>"
                  << std::endl;
        return 1;
    }

    std::string compiler = fs::absolute(argv[1]).string();
    fs::path outputBase = fs::temp_directory_path() / "dss-integrated-tests";

    // Clean previous run
    fs::remove_all(outputBase);
    fs::create_directories(outputBase);

    std::cout << "=== DSS Code Prime - Integration Tests ===" << std::endl;
    std::cout << "Compiler: " << compiler << std::endl;
    std::cout << "Output:   " << outputBase.string() << std::endl;
    std::cout << std::endl;

    // -----------------------------------------------------------
    // Test 1: Default invocation prints ready message
    // -----------------------------------------------------------

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
              line.find("DSS Code Prime compiler ready.")
                  != std::string::npos,
              "got: " + line);
    }
    std::cout << std::endl;

    // -----------------------------------------------------------
    // Test 2: Unknown flag is rejected loudly (exit 2)
    // -----------------------------------------------------------

    std::cout << "[Test 2] Unknown flag is rejected" << std::endl;
    {
        std::string cmd = compiler + " --no-such-flag > " +
            (outputBase / "unknown_flag.txt").string() + " 2>&1";
        int rc = std::system(cmd.c_str());
        // `std::system` exit-code encoding varies (POSIX shifts;
        // Windows passes through); 2 OR shifted-2 are both accepted.
        check("Exit code indicates rejection",
              rc != 0,
              "got " + std::to_string(rc));

        std::ifstream f((outputBase / "unknown_flag.txt").string());
        std::string body((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
        check("Mentions unrecognized flag",
              body.find("unrecognized flag") != std::string::npos,
              "stderr: " + body);
    }
    std::cout << std::endl;

    // -----------------------------------------------------------
    // Summary
    // -----------------------------------------------------------

    std::cout << "===========================================" << std::endl;
    std::cout << "Results: " << passes << " passed, "
              << failures << " failed" << std::endl;

    // Cleanup
    fs::remove_all(outputBase);

    return failures > 0 ? 1 : 0;
}
