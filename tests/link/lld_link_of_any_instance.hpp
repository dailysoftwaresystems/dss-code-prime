// lld-link FROM ANY VISUAL STUDIO INSTANCE THAT CARRIES THE LLVM COMPONENT.
//
// A test that hands DSS objects to lld-link finds it at `<instance>\VC\Tools\Llvm\x64\bin\lld-link.exe` of ANY
// instance carrying Microsoft.VisualStudio.Component.VC.Llvm.Clang, not only the one the cl toolchain was found in
// (`-latest` picks one; a host's LLVM component can live in another). Empty, with `why` set, when no instance
// carries it — the caller SKIPS, naming why. One locator, so two tests cannot disagree about where lld-link is.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace dss::test_support {

[[nodiscard]] inline std::filesystem::path lldLinkOfAnyInstance(std::filesystem::path const& work, std::string& why) {
    namespace fs = std::filesystem;
    fs::path const vswhere{"C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"};
    std::error_code ec;
    if (!fs::is_regular_file(vswhere, ec)) {
        why = "no Visual Studio Installer (vswhere.exe) on this machine";
        return {};
    }
    fs::path const listed = work / "llvm-instances.txt";
    std::string const q = "\"\"" + vswhere.string() + "\" -products * -requires "
                          "Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath > \""
                        + listed.string() + "\"\"";
    if (std::system(q.c_str()) != 0) {
        why = "vswhere.exe is present but the query for the LLVM component failed: `" + q + "`";
        return {};
    }
    std::ifstream in{listed};
    std::string seen;
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        fs::path const lld = fs::path{line} / "VC" / "Tools" / "Llvm" / "x64" / "bin" / "lld-link.exe";
        if (fs::is_regular_file(lld, ec)) return lld;
        seen += " `" + lld.string() + "`";
    }
    why = seen.empty() ? std::string{"no Visual Studio instance carries Microsoft.VisualStudio.Component.VC.Llvm.Clang"}
                       : "vswhere listed instances with the LLVM component, but none holds lld-link.exe:" + seen;
    return {};
}

}  // namespace dss::test_support
