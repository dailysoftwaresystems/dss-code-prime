// The USER-FACING symptom pin for `applySystemDirs`: an angle `#include` must
// resolve when the process cwd is OUTSIDE the source tree and `DSS_CONFIG_ROOT`
// names it.
//
// ── WHAT WAS BROKEN ────────────────────────────────────────────────────────
// `program.cpp::applySystemDirs` resolved the language's shipped system-include
// dirs (`semantics.shippedLibDirs`) by walking up 8 ancestors from cwd, and
// never read `DSS_CONFIG_ROOT`. Its comment said it mirrored `findShippedConfig`
// — it had copied the walk and dropped the override. MEASURED through the
// shipped CLI, one variable changed (cwd), the override SET IN BOTH ARMS, same
// binary, source = `#include <stdio.h>` + `int main(void){return 0;}`:
//     cwd inside the repo → rc 0, zero diagnostics
//     cwd `C:\`           → rc 1, `error[F001A]: got stdio.h`
// So the shipped compiler could not resolve an angle include from any working
// directory outside its own source tree. Every existing consumer masked it: the
// sqlite harness passes explicit `-I` dirs and the corpus runners sit in-tree.
//
// ── WHY THIS TEST AND NOT ONLY A UNIT TEST ─────────────────────────────────
// `tests/core/test_config_path_walk.cpp` covers the shared primitive
// (`findShippedConfigDir`) thoroughly, and it would NOT have caught this: the
// primitive was correct, the caller simply never called it. The defect only
// exists end-to-end — driver → grammar config → system dirs → include resolver
// — so the pin has to be end-to-end too.
//
// ── RED ON DISABLE ─────────────────────────────────────────────────────────
// Revert `applySystemDirs` to the cwd-only walk and `ResolvesAngleIncludeFrom
// CwdOutsideTheSourceTree` fails with the F_ShippedHeaderNotFound this file
// asserts against, while the in-tree control below stays green — which is
// precisely the asymmetry that made the bug invisible for so long.
//
// Compile-only + a fixed cross-target, so this is HOST-AGNOSTIC and runs on
// every leg (Windows / WSL x86_64 / arm64): no artifact is executed here.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "diagnostic_count.hpp"
#include "program/program.hpp"
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using namespace dss;
using namespace dss::test_support;
namespace fs = std::filesystem;

namespace {

// A fixed ELF x86_64 target: nothing is RUN, so this cross-compiles cleanly on
// every host. Same choice, same reason, as tests/program/test_include_dirs.cpp.
constexpr char const* kTarget = "x86_64:elf64-x86_64-linux-exec";

// The angle form is the whole point — `<stdio.h>` resolves ONLY through the
// shipped system-include dirs (`semantics.shippedLibDirs`), never through the
// including file's own directory or `-I`.
constexpr char const* kSource =
    "#include <stdio.h>\n"
    "int main(void){ return 0; }\n";

// Portable RAII env override. Kept LOCAL rather than hoisted for the same
// reason the copies in tests/core/test_config_path_walk.cpp and
// tests/test_support/test_repo_root.cpp are: a test that mutates the
// environment must not leak that mutation into a sibling binary's expectations.
class ScopedEnv {
public:
    ScopedEnv(char const* name, std::string const& value) : name_(name) {
        if (char const* prev = std::getenv(name)) { had_ = true; prev_ = prev; }
        set(value);
    }
    ~ScopedEnv() { had_ ? set(prev_) : clear(); }
    ScopedEnv(ScopedEnv const&)            = delete;
    ScopedEnv& operator=(ScopedEnv const&) = delete;

private:
    void set(std::string const& v) {
#ifdef _WIN32
        _putenv_s(name_.c_str(), v.c_str());
#else
        ::setenv(name_.c_str(), v.c_str(), 1);
#endif
    }
    void clear() {
#ifdef _WIN32
        _putenv_s(name_.c_str(), "");
#else
        ::unsetenv(name_.c_str());
#endif
    }
    std::string name_;
    bool        had_ = false;
    std::string prev_;
};

// RAII cwd pin. `ScratchDir::useAsCwd` deliberately REFUSES `Location::Temp`
// (a temp scratch is outside the repo, which used to break every schema
// lookup). That refusal protects its callers; here the temp cwd IS the
// experiment, so this test pins cwd itself.
class ScopedCwd {
public:
    explicit ScopedCwd(fs::path const& to) : prev_(fs::current_path()) {
        fs::current_path(to);
    }
    ~ScopedCwd() {
        std::error_code ec;
        fs::current_path(prev_, ec);   // a dtor must not throw
    }
    ScopedCwd(ScopedCwd const&)            = delete;
    ScopedCwd& operator=(ScopedCwd const&) = delete;

private:
    fs::path prev_;
};

fs::path writeSource(fs::path const& dir) {
    fs::path const p = dir / "main.c";
    std::ofstream(p) << kSource;
    return p;
}

// Compile `src` to `outDir` and report rc plus the shipped-header miss count.
struct CompileOutcome {
    int         rc = 0;
    std::size_t shippedHeaderMisses = 0;
};

CompileOutcome compileOne(fs::path const& src, fs::path const& outDir) {
    Program p;
    p.setOutputDir(outDir);
    DiagnosticReporter rep;
    CompileOutcome out;
    out.rc = p.compileFiles(std::vector<std::string>{src.string()}, "c-subset",
                            std::vector<std::string>{kTarget}, rep);
    out.shippedHeaderMisses =
        countCode(rep, DiagnosticCode::F_ShippedHeaderNotFound);
    return out;
}

}  // namespace

// THE REGRESSION. cwd is a temp directory with no `src/dss-config` anywhere in
// its ancestry; `DSS_CONFIG_ROOT` names the repo. The angle include must
// resolve — that is what the override is FOR, and what the shipped CLI needs in
// order to be usable from anywhere but its own checkout.
TEST(SystemDirsCwdIndependent, ResolvesAngleIncludeFromCwdOutsideTheSourceTree) {
    fs::path const repo = dss::test::repoRoot();   // resolve BEFORE moving cwd
    ScratchDir scratch(Location::Temp, "system-dirs-cwd");
    fs::path const src = writeSource(scratch.path());

    ScopedEnv env("DSS_CONFIG_ROOT", repo.string());
    ScopedCwd cwd(scratch.path());                 // outside the source tree

    CompileOutcome const got = compileOne(src, scratch.path() / "out");
    EXPECT_EQ(got.shippedHeaderMisses, 0u)
        << "`#include <stdio.h>` must resolve via DSS_CONFIG_ROOT — the shipped "
           "system-include dirs cannot depend on where the process was launched";
    EXPECT_EQ(got.rc, 0)
        << "the compile must succeed with cwd outside the source tree";
}

// THE CONTROL, and it is load-bearing rather than decorative: this arm passed
// throughout the defect's lifetime. If BOTH arms ever go red the cause is
// something else entirely (a broken stdio descriptor, a target/format change) —
// only the two DISAGREEING points at system-dir discovery.
TEST(SystemDirsCwdIndependent, ResolvesAngleIncludeFromCwdInsideTheSourceTree) {
    fs::path const repo = dss::test::repoRoot();
    ScratchDir scratch(Location::Temp, "system-dirs-cwd");
    fs::path const src = writeSource(scratch.path());

    ScopedEnv env("DSS_CONFIG_ROOT", repo.string());
    ScopedCwd cwd(repo);                           // the pre-fix happy path

    CompileOutcome const got = compileOne(src, scratch.path() / "out");
    EXPECT_EQ(got.shippedHeaderMisses, 0u);
    EXPECT_EQ(got.rc, 0);
}
