// AN ARTIFACT AT ANY LENGTH -- the closure of [[D-LINK-A-DEEP-OUTPUT-DIRECTORY-FAILS-THE-WINDOWS-IMAGE-WRITE]]
// (P69 H6).
//
// ✔MEASURED (lane xa, run 20261104-203219-c10af7f7, windows-x86_64-debug): DSS could not create an artifact whose full
// path is 260 characters or more -- its 267-character staging files, in a 201-character directory that WAS created,
// were refused, `K_ImageWriteOpenFailed`. Every Win32 file call took the plain spelling, which opens nothing at that
// length on a Windows that has not opted into long paths (probe-reference-cc runs 20261104-215014-7ba4f08f and
// 20261104-215144-4893b98f: CreateFileW, CreateDirectoryW, MoveFileExW, DeleteFileW, FindFirstFileExW, _wfopen and
// both libraries' std::filesystem alike). The registry switch that lifts it is a system setting, not a fix.
//
// What is pinned, each at a path of 300 characters or more AND, the control, below 260 -- on Windows, where the limit
// is (every other host compiles this file and has no such limit to pin):
//   * AnObjectIsWrittenReadBackAndLinkedIntoAnImageThatRuns -- a unit compiled to an OBJECT there (the writer's create,
//     staging and rename), that object READ BACK as the input of a link (the object reader), the IMAGE written there,
//     its bytes read back, and the image STARTED there (the application name past the OS's plain limit), by the
//     process spawner and by the test harness's runBinary alike -- exit 42;
//   * ACurrentDirectoryPastTheOsLimitIsRefusedNamingIt -- the one limit no spelling lifts: CreateProcessW takes no
//     current directory longer than MAX_PATH - 2 with its separator (✔MEASURED, run 20261104-220446-62443cb7,
//     ERROR_DIRECTORY in every spelling), so the spawn refuses it BEFORE any process exists, naming the limit.

#include "core/substrate/path_identity.hpp"
#include "core/substrate/process_spawn.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "program/program.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using dss::DiagnosticReporter;
using dss::Program;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

#if defined(_WIN32)

constexpr char const* kObjectTarget = "x86_64:pe64-x86_64-windows";
constexpr char const* kExecTarget   = "x86_64:pe64-x86_64-windows-exec";

constexpr char const* kLibSource  = "int dss_long_path_value(void) { return 42; }\n";
constexpr char const* kMainSource = "int dss_long_path_value(void);\nint main(void) { return dss_long_path_value(); }\n";

// `base` extended by 40-character directory names until it has `atLeast` characters.
[[nodiscard]] fs::path deepUnder(fs::path const& base, std::size_t atLeast) {
    fs::path p = base;
    while (p.native().size() < atLeast) p /= std::string(40, 'a');
    return p;
}

// `text` written to `file` -- through the extended-length spelling, as DSS writes.
[[nodiscard]] bool writeText(fs::path const& file, char const* text) {
    std::ofstream out{dss::core::extendedLengthSpelling(file), std::ios::binary};
    out << text;
    return out.good();
}

// The one regular file DSS left in `dir` (listed by the substrate, which lists at any length), or empty.
[[nodiscard]] fs::path onlyFileIn(fs::path const& dir) {
    std::vector<fs::path> files;
    auto const walk = dss::core::walkTree(dir, [&files](dss::core::TreeEntry const& e) {
        if (e.kind == dss::core::EntryKind::File) files.push_back(e.path);
        return dss::core::WalkStep::Prune;
    });
    return !walk.error && files.size() == 1 ? files.front() : fs::path{};
}

struct Cell {
    char const* name;
    std::size_t atLeast;   // 0: the control, below 260
};

#endif

}  // namespace

TEST(ArtifactAtAnyLength, AnObjectIsWrittenReadBackAndLinkedIntoAnImageThatRuns) {
#if defined(_WIN32)
    for (Cell const cell : {Cell{"below 260 (the control)", 0}, Cell{"300 characters or more", 300}}) {
        SCOPED_TRACE(cell.name);
        ScratchDir     scratch{Location::Temp, "artifact-at-any-length"};
        fs::path const dir = cell.atLeast == 0 ? scratch.path() / "s" : deepUnder(scratch.path(), cell.atLeast);
        ASSERT_TRUE(cell.atLeast == 0 ? dir.native().size() < 230 : dir.native().size() >= cell.atLeast)
            << dir.native().size() << " characters";
        std::error_code ec;
        static_cast<void>(dss::core::createDirectories(dir / "obj", ec));
        ASSERT_FALSE(ec) << ec.message();
        static_cast<void>(dss::core::createDirectories(dir / "exe", ec));
        ASSERT_FALSE(ec) << ec.message();
        ASSERT_TRUE(writeText(dir / "lib.c", kLibSource));
        ASSERT_TRUE(writeText(dir / "main.c", kMainSource));

        // (1) THE OBJECT, written there.
        DiagnosticReporter objRep;
        Program            objProg;
        objProg.setOutputDir(dir / "obj");
        ASSERT_EQ(objProg.compileFiles({(dir / "lib.c").string()}, "c", {kObjectTarget}, objRep), 0)
            << "the object was not written; errorCount=" << objRep.errorCount();
        fs::path const object = onlyFileIn(dir / "obj");
        ASSERT_FALSE(object.empty()) << "no single object was left in the object directory";

        // (2) THE OBJECT READ BACK as a link's input, and THE IMAGE written there.
        DiagnosticReporter exeRep;
        Program            exeProg;
        exeProg.setOutputDir(dir / "exe");
        ASSERT_EQ(exeProg.compileFiles({(dir / "main.c").string(), object.string()}, "c", {kExecTarget}, exeRep), 0)
            << "the object was not linked into an image; errorCount=" << exeRep.errorCount();
        fs::path const image = onlyFileIn(dir / "exe");
        ASSERT_FALSE(image.empty()) << "no single image was left in the image directory";
        if (cell.atLeast != 0) EXPECT_GE(image.native().size(), 300u) << "the image's own path is not past the limit";

        // (3) THE IMAGE'S BYTES read back.
        std::ifstream in{dss::core::extendedLengthSpelling(image), std::ios::binary};
        std::string const head{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        ASSERT_GE(head.size(), 2u) << "the image was not read back";
        EXPECT_EQ(head.substr(0, 2), "MZ");

        // (4) THE IMAGE STARTED there -- by the process spawner, and by the test harness's own CreateProcessW
        // (tests/test_support/run_binary.hpp), which takes the same rule from its one home.
        auto const run = dss::substrate::spawnAndWaitInherit({image.string()});
        ASSERT_TRUE(run.spawned) << run.diagnostic;
        EXPECT_EQ(run.exitCode, 42);
        auto const harnessRun = dss::test_support::runBinary(image);
        ASSERT_TRUE(harnessRun.spawned) << harnessRun.diagnostic;
        EXPECT_FALSE(harnessRun.timedOut);
        EXPECT_EQ(harnessRun.exitCode, 42u);
    }
#else
    GTEST_SKIP() << "the 260-character limit is Win32's: this host has none to pin";
#endif
}

TEST(ArtifactAtAnyLength, ACurrentDirectoryPastTheOsLimitIsRefusedNamingIt) {
#if defined(_WIN32)
    ScratchDir     scratch{Location::Temp, "artifact-at-any-length-cwd"};
    fs::path const deep = deepUnder(scratch.path(), 300);
    fs::path const shallow = scratch.path() / "s";
    std::error_code ec;
    static_cast<void>(dss::core::createDirectories(deep, ec));
    ASSERT_FALSE(ec) << ec.message();
    static_cast<void>(dss::core::createDirectories(shallow, ec));
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_TRUE(writeText(shallow / "main.c", "int main(void) { return 42; }\n"));
    DiagnosticReporter rep;
    Program            prog;
    prog.setOutputDir(shallow / "exe");
    ASSERT_EQ(prog.compileFiles({(shallow / "main.c").string()}, "c", {kExecTarget}, rep), 0);
    fs::path const image = onlyFileIn(shallow / "exe");
    ASSERT_FALSE(image.empty());

    auto const control = dss::substrate::spawnAndWaitInherit({image.string()}, shallow);
    ASSERT_TRUE(control.spawned) << "the control's current directory, below the limit, was refused: "
                                 << control.diagnostic;
    EXPECT_EQ(control.exitCode, 42);

    auto const refused = dss::substrate::spawnAndWaitInherit({image.string()}, deep);
    EXPECT_FALSE(refused.spawned) << "a current directory past the OS's limit started a process";
    EXPECT_NE(refused.diagnostic.find("Windows starts no process in a current directory longer than 258"),
              std::string::npos)
        << "the refusal does not name the OS's limit: " << refused.diagnostic;
#else
    GTEST_SKIP() << "the current directory's limit is Win32's: this host has none to pin";
#endif
}
