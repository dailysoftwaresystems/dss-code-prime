#pragma once

// THE IMAGE A TEST'S argv[0] NAMES (P69 H6c) -- named exactly as the product's caller names it
// (src/program/build_scripts.cpp). The spawn takes its image as a VALUE, apart from argv: a bare name stays a name,
// searched on PATH by the spawn, and any other spelling is the image itself. argv[0] is UTF-8, as every argv element is,
// and is decoded as the product decodes a manifest's program (`substrate::programPathFromUtf8`); bytes that are not
// UTF-8 name no image, so the spawn refuses argv[0] itself, naming why.
//
// A case that pins the spawn THROUGH argv takes its image from here, so it pins what a build script's spawn does; a
// case of the image rule itself hands the image apart from argv.

#include "core/substrate/host_path.hpp"
#include "core/substrate/process_spawn.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dss::test_support {

// The image argv[0] names; the empty value for an empty argv or bytes that are not UTF-8.
[[nodiscard]] inline dss::core::HostPath imageNamedBy(std::vector<std::string> const& argv) {
    if (argv.empty()) return {};
    std::optional<std::filesystem::path> const program = dss::substrate::programPathFromUtf8(argv.front());
    return program.has_value() ? dss::core::HostPath{*program} : dss::core::HostPath{};
}

// `spawnAndWaitInherit` with the image argv[0] names.
[[nodiscard]] inline dss::substrate::SpawnResult spawnArgvInherit(std::vector<std::string> const& argv,
                                                                  dss::core::HostPath const&      cwd      = {},
                                                                  std::vector<std::string> const& withheld = {}) {
    return dss::substrate::spawnAndWaitInherit(imageNamedBy(argv), argv, cwd, withheld);
}

// `spawnAndWaitRedirectStdout` with the image argv[0] names.
[[nodiscard]] inline dss::substrate::SpawnResult spawnArgvRedirectStdout(std::vector<std::string> const& argv,
                                                                         dss::core::HostPath const&      cwd,
                                                                         dss::core::HostPath const&      stdoutFile,
                                                                         std::vector<std::string> const& withheld = {}) {
    return dss::substrate::spawnAndWaitRedirectStdout(imageNamedBy(argv), argv, cwd, stdoutFile, withheld);
}

}  // namespace dss::test_support
