// c162 (D-FF1-READER-CONSUMER) -- the reader-consumer round-trip witness.
//
// This is the trigger that makes the FF5 `ingest()` binary-reader consumer
// LIVE. The three FF1 readers (ELF/PE/Mach-O export + ar) were built +
// hardened + green, producing a format-blind ImportSurface, but `ingest()`
// -- the consumer that calls the readers -- had NO production caller
// (externs resolved only via inline `extern` + shipped JSON descriptors).
//
// THE RESOLVING INSIGHT: DSS builds its OWN library binary, and for a
// user's OWN library there is NO JSON descriptor -- so reading its real
// export table is the ONLY way to link against it. That is a genuine,
// NON-DUPLICATIVE capability AND a self-contained trigger. The flow, proven
// end-to-end PER-PLATFORM NATIVE here:
//   1. DSS compiles lib.c (`int dss_lib_answer(void){ return 42; }`) into a
//      shared library -- `.so` (ELF, c150 writer) / `.dll` (PE, c152).
//   2. DSS compiles main.c (`extern int dss_lib_answer(void); int main(void)
//      { return dss_lib_answer(); }`) and RESOLVES its extern against that
//      library BY READING its export surface: the FF1 reader ->
//      ImportSurface -> the now-live `ingest()` -> FfiMetadata -> the
//      linker extern-bind path (DT_NEEDED / PE import descriptor) -- driven
//      by the `--resolve-library <path>` surface (Program::setResolveLibraries).
//   3. RUN main -> it calls into the library -> exit 42.
//
// HONEST SCOPE (the binary is NOT signature-authority): the read provides
// exactly (a) VALIDATION -- the declared extern EXISTS as an export (an
// absent one fails loud with F_FfiResolveLibrarySymbolAbsent naming the
// symbol + library, a compile-time error instead of a link/load failure);
// and (b) LIBRARY BINDING -- the true library to import from. The TYPE of
// the extern still comes from the inline `extern` declaration. This is
// NON-DUPLICATIVE of the shipped-JSON path precisely because a DSS-built
// library has no shipped descriptor.
//
// This is an INTEGRATION TEST (not an examples/ corpus entry) because the
// examples_runner is single-artifact-per-target and cannot express a
// two-artifact DEPENDENT build (build the library as artifact 1, then build
// main resolving against artifact 1). The runner multi-artifact extension is
// the named follow-up D-EXAMPLES-RUNNER-MULTI-ARTIFACT.
//
// Per-platform NATIVE round-trips (coordinator steer): Linux writes+reads a
// `.so` and RUNS it (ubuntu CI + local WSL); Windows writes+reads a `.dll`
// and RUNS it natively. macOS is deferred to c163 as a `.a` STATIC-archive
// round-trip (needs an ar WRITER + a static-link model DSS does not yet have)
// -- anchored D-FF1-AR-WRITER-STATIC-LINK.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "diagnostic_count.hpp"
#include "ffi/binary_reader.hpp"
#include "program/program.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::test_support;
namespace fs = std::filesystem;

namespace {

// The library defines the answer; main declares it `extern` (TYPE from the
// declaration) and returns its value. The stem `dsslib` names the artifact
// (`dsslib.so` / `dsslib.dll`) -- the loader-resolvable identity the
// `--resolve-library` read records in main's DT_NEEDED / import descriptor.
constexpr std::string_view kLibSrc =
    "int dss_lib_answer(void){ return 42; }\n";
constexpr std::string_view kMainSrc =
    "extern int dss_lib_answer(void);\n"
    "int main(void){ return dss_lib_answer(); }\n";
// A GENUINELY-UNKNOWN symbol -- absent from the library AND from every shipped
// descriptor (not a real libc/system name). This is the typo case that MUST
// fail loud (distinct from a bare system extern, which falls through).
constexpr std::string_view kMainTypoSrc =
    "extern int dss_absent_symbol(void);\n"
    "int main(void){ return dss_absent_symbol(); }\n";

// The mixed program the silent-failure HIGH fold turns green: a BARE
// `extern int puts;` (a real libc/msvcrt symbol the user did NOT #include ->
// no libraryOverride -> binary-governed) ALONGSIDE the own-library extern.
// `puts` is absent from dsslib but IS a known system symbol -> falls through
// to the format-default library; `dss_lib_answer` binds to dsslib. Both
// resolve; prints "hi" + exits 42.
constexpr std::string_view kMixedSrc =
    "extern int puts(const char*);\n"
    "extern int dss_lib_answer(void);\n"
    "int main(void){ puts(\"hi\"); return dss_lib_answer(); }\n";
// A TU with NO source-declared externs -- nothing routes to ingest(). Used to
// pin the eager `--resolve-library` path validation (a bad path must still
// fail loud here, not be silently ignored).
constexpr std::string_view kNoExternSrc =
    "int main(void){ return 7; }\n";

fs::path writeSrc(fs::path const& dir, std::string_view name,
                  std::string_view text) {
    auto const p = dir / std::string{name};
    std::ofstream f(p);
    f << text;
    return p;
}

// Compile one source file to one target via the real production driver.
// `resolveLibs` non-empty threads the `--resolve-library` surface. Returns
// the compileFiles rc (0 == success).
int buildOne(fs::path const& outDir,
             std::vector<fs::path> const& resolveLibs,
             std::string const& srcPath,
             std::string const& target,
             DiagnosticReporter& rep) {
    Program p;
    p.setOutputDir(outDir);
    if (!resolveLibs.empty()) p.setResolveLibraries(resolveLibs);
    return p.compileFiles(std::vector<std::string>{srcPath},
                          "c-subset", std::vector<std::string>{target}, rep);
}

// The DSS-built library exports the symbol we resolve against -- proven by
// reading its real export surface with the SAME FF1 reader `ingest()` uses.
// This is the "validation source" the round-trip rests on.
[[nodiscard]] bool libraryExportsSymbol(fs::path const& libPath,
                                        std::string_view symbol) {
    DiagnosticReporter rep;
    auto surface = ffi::readImports(libPath, rep);
    if (!surface.has_value()) return false;
    return std::any_of(surface->begin(), surface->end(),
                       [&](ffi::ImportSurface const& row) {
                           return row.mangledName == symbol
                               && row.kind == ffi::SymbolKind::Function;
                       });
}

}  // namespace

// ── Validation fail-loud (all hosts) -- red-on-disable (a) ─────────────────
//
// A GENUINELY-UNKNOWN declared extern -- absent from the resolve-library's
// export surface AND from every shipped descriptor (a typo like
// `dss_absent_symbol`, NOT a bare system call) -- fails the compile LOUD with
// F_FfiResolveLibrarySymbolAbsent, naming the symbol + the library, and emits
// NO artifact. This is the meaningful typo-catch: reading a real export table
// proves an own-library symbol exists at compile time. RED-ON-DISABLE: drop
// the descriptor-aware fail-loud arm in compile_pipeline step 2.5 and this
// main would silently mis-bind to the format-default library (dangling import,
// caught only at link/load). Distinct from a bare system extern, which the
// MixedBareSystemExternAndOwnLibraryBothResolve test proves DOES fall through.
//
// Host-native dynamic format so the library builds cleanly on every leg
// (Windows -> PE .dll; else -> ELF .so; the reader + the validation are
// host-agnostic -- no artifact is RUN here, the compile fails first).
TEST(FfiResolveLibraryRoundTrip, GenuinelyUnknownExternFailsLoud) {
#if defined(_WIN32)
    std::string const libTarget  = "x86_64:pe64-x86_64-windows-dll";
    std::string const execTarget = "x86_64:pe64-x86_64-windows-exec";
    std::string const libArtifact = "dsslib.dll";
#else
    std::string const libTarget  = "x86_64:elf64-x86_64-linux-dyn";
    std::string const execTarget = "x86_64:elf64-x86_64-linux-exec";
    std::string const libArtifact = "dsslib.so";
#endif
    ScratchDir scratch{Location::InsideRepo, "ffi-resolve-lib"};
    auto const dir = scratch.path();
    auto const libSrc  = writeSrc(dir, "dsslib.c", kLibSrc);
    auto const mainSrc = writeSrc(dir, "main_typo.c", kMainTypoSrc);

    // Build the library.
    DiagnosticReporter libRep;
    ASSERT_EQ(buildOne(dir, {}, libSrc.string(), libTarget, libRep), 0)
        << "DSS must build the shared library from source";
    auto const libPath = dir / libArtifact;
    ASSERT_TRUE(fs::exists(libPath));
    // The library really exports dss_lib_answer (the read surface) but NOT
    // dss_absent_symbol -- the exact validation condition.
    EXPECT_TRUE(libraryExportsSymbol(libPath, "dss_lib_answer"));
    EXPECT_FALSE(libraryExportsSymbol(libPath, "dss_absent_symbol"));

    // Compile main_typo resolving against the library -> fail loud.
    DiagnosticReporter mainRep;
    int const rc = buildOne(dir, {libPath}, mainSrc.string(),
                            execTarget, mainRep);
    EXPECT_NE(rc, 0)
        << "a declared extern absent from the resolve-library must FAIL "
           "the compile (validation policy)";
    EXPECT_GT(::dss::test_support::countCode(
                  mainRep, DiagnosticCode::F_FfiResolveLibrarySymbolAbsent),
              0u)
        << "the fail-loud must be F_FfiResolveLibrarySymbolAbsent "
           "specifically -- reading the export table found no such symbol";
    EXPECT_FALSE(fs::exists(dir / "main_typo"))
        << "no ELF artifact on a failed compile";
    EXPECT_FALSE(fs::exists(dir / "main_typo.exe"))
        << "no PE artifact on a failed compile";
}

// ── Eager --resolve-library path validation (all hosts) -- MEDIUM fold ─────
//
// A nonexistent / unreadable `--resolve-library` path must fail loud
// F_FileOpenFailed EVEN WHEN the TU has NO source-declared externs (so nothing
// routes to the ingest() consumer). Before the fold this was SILENTLY IGNORED
// (the binary was reached only through ingest(), which the partition skipped)
// -- violating the documented contract "opened + read at compile time, fails
// loud on a missing/unreadable file". RED-ON-DISABLE: drop the eager open-probe
// in compile_pipeline step 2.5-pre and this compile succeeds (exit 0), silently
// ignoring the bad path.
TEST(FfiResolveLibraryRoundTrip, MissingResolveLibraryPathFailsLoudEvenWithNoExterns) {
#if defined(_WIN32)
    std::string const execTarget = "x86_64:pe64-x86_64-windows-exec";
#else
    std::string const execTarget = "x86_64:elf64-x86_64-linux-exec";
#endif
    ScratchDir scratch{Location::InsideRepo, "ffi-resolve-lib"};
    auto const dir = scratch.path();
    auto const mainSrc = writeSrc(dir, "noextern.c", kNoExternSrc);
    auto const missing = dir / "does_not_exist_lib.bin";  // never created

    DiagnosticReporter rep;
    int const rc = buildOne(dir, {missing}, mainSrc.string(), execTarget, rep);
    EXPECT_NE(rc, 0)
        << "a missing --resolve-library path must fail the compile even when "
           "the TU has no externs (nothing routes to ingest)";
    EXPECT_GT(::dss::test_support::countCode(
                  rep, DiagnosticCode::F_FileOpenFailed), 0u)
        << "must fire F_FileOpenFailed -- the eager path probe honors the "
           "documented open-at-compile-time contract";
}

// ── PE dynamic round-trip (Windows host) -- the native witness + (b) ───────

#if defined(_WIN32)
TEST(FfiResolveLibraryRoundTrip, PeDynamicRoundTripExitsFortyTwo) {
    ScratchDir scratch{Location::InsideRepo, "ffi-resolve-lib"};
    auto const dir = scratch.path();
    auto const libSrc  = writeSrc(dir, "dsslib.c", kLibSrc);
    auto const mainSrc = writeSrc(dir, "main.c", kMainSrc);

    // 1. DSS builds dsslib.dll (PE .dll export directory, c152).
    DiagnosticReporter libRep;
    ASSERT_EQ(buildOne(dir, {}, libSrc.string(),
                       "x86_64:pe64-x86_64-windows-dll", libRep), 0);
    auto const dllPath = dir / "dsslib.dll";
    ASSERT_TRUE(fs::exists(dllPath));
    ASSERT_TRUE(libraryExportsSymbol(dllPath, "dss_lib_answer"))
        << "dsslib.dll must export dss_lib_answer (the read validation "
           "source the whole round-trip rests on)";

    // 2. DSS builds main.exe RESOLVING its extern against dsslib.dll.
    DiagnosticReporter mainRep;
    ASSERT_EQ(buildOne(dir, {dllPath}, mainSrc.string(),
                       "x86_64:pe64-x86_64-windows-exec", mainRep), 0)
        << "main.exe must build, resolving dss_lib_answer against dsslib.dll";
    auto const exePath = dir / "main.exe";
    ASSERT_TRUE(fs::exists(exePath));

    // 3. RUN main.exe natively -> it imports dss_lib_answer from dsslib.dll
    //    (same dir) and returns 42.
    auto const r = runBinary(exePath, std::chrono::milliseconds{5000});
    ASSERT_TRUE(r.spawned)
        << "main.exe must spawn -- if not, the PE import descriptor to "
           "dsslib.dll is structurally invalid. " << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "THE acceptance criterion: exit 42 = dss_lib_answer() called "
           "through the dsslib.dll import bound by the reader-consumer.";
}

// Red-on-disable (b): WITHOUT --resolve-library, dss_lib_answer has no
// binding source, falls through to the format-default library (msvcrt.dll),
// and the loader cannot find it there -> the process fails to start / exits
// non-42. The ONLY thing that makes main run is the binding discovered by
// READING dsslib.dll -- so this proves the library binding comes from the
// read, not from anywhere else.
TEST(FfiResolveLibraryRoundTrip, PeWithoutResolveLibraryMisbindsAtRuntime) {
    ScratchDir scratch{Location::InsideRepo, "ffi-resolve-lib"};
    auto const dir = scratch.path();
    auto const libSrc  = writeSrc(dir, "dsslib.c", kLibSrc);
    auto const mainSrc = writeSrc(dir, "main.c", kMainSrc);

    DiagnosticReporter libRep;
    ASSERT_EQ(buildOne(dir, {}, libSrc.string(),
                       "x86_64:pe64-x86_64-windows-dll", libRep), 0);

    // Build main WITHOUT resolveLibraries -> binds to the msvcrt default.
    DiagnosticReporter mainRep;
    ASSERT_EQ(buildOne(dir, {}, mainSrc.string(),
                       "x86_64:pe64-x86_64-windows-exec", mainRep), 0)
        << "main.exe still BUILDS (dss_lib_answer binds to the format "
           "default) -- the misbinding surfaces only at load/run.";
    auto const exePath = dir / "main.exe";
    ASSERT_TRUE(fs::exists(exePath));

    auto const r = runBinary(exePath, std::chrono::milliseconds{5000});
    // Mis-bound to msvcrt.dll (which does not export dss_lib_answer) -> the
    // loader fails to resolve the import; the process never returns 42.
    EXPECT_TRUE(!r.spawned || r.exitCode != 42u)
        << "WITHOUT the binary read, main must NOT exit 42 -- the binding "
           "that makes it run comes ONLY from reading dsslib.dll. "
           "spawned=" << r.spawned << " exit=" << r.exitCode;
}

// silent-failure HIGH fold (b): the MIXED program -- a bare `extern puts;`
// (a real msvcrt symbol NOT #included -> binary-governed, absent from dsslib,
// but a KNOWN system symbol -> falls through to msvcrt) ALONGSIDE the own-lib
// `dss_lib_answer` (-> dsslib.dll). BOTH resolve; the program prints "hi" and
// exits 42. Before the fold, --resolve-library wrongly failed loud on `puts`,
// rejecting this legitimate program. RED-ON-DISABLE: revert the
// descriptor-fallthrough (treat every unmatched governed extern as a typo) and
// this compile fails on `puts`.
TEST(FfiResolveLibraryRoundTrip, PeMixedBareSystemExternAndOwnLibraryExitFortyTwo) {
    ScratchDir scratch{Location::InsideRepo, "ffi-resolve-lib"};
    auto const dir = scratch.path();
    auto const libSrc  = writeSrc(dir, "dsslib.c", kLibSrc);
    auto const mainSrc = writeSrc(dir, "mixed.c", kMixedSrc);

    DiagnosticReporter libRep;
    ASSERT_EQ(buildOne(dir, {}, libSrc.string(),
                       "x86_64:pe64-x86_64-windows-dll", libRep), 0);
    auto const dllPath = dir / "dsslib.dll";

    DiagnosticReporter mainRep;
    ASSERT_EQ(buildOne(dir, {dllPath}, mainSrc.string(),
                       "x86_64:pe64-x86_64-windows-exec", mainRep), 0)
        << "the mixed bare-puts + own-lib program must BUILD -- puts falls "
           "through to msvcrt (known system symbol), dss_lib_answer binds to "
           "dsslib.dll. A fail-loud here is the HIGH bug.";
    auto const exePath = dir / "mixed.exe";
    ASSERT_TRUE(fs::exists(exePath));

    auto const r = runBinary(exePath, std::chrono::milliseconds{5000},
                             /*captureStdout=*/true);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "exit 42 = puts->msvcrt + dss_lib_answer->dsslib.dll both bound.";
    EXPECT_NE(r.capturedStdout.find("hi"), std::string::npos)
        << "puts must print 'hi' (it resolved to msvcrt); got: ["
        << r.capturedStdout << "]";
}
#endif  // _WIN32

// ── ELF dynamic round-trip (Linux host) -- the native witness ──────────────
//
// Gated on __linux__ AND an x86_64 host: the artifacts are built for the
// x86_64:elf64-x86_64-linux-{dyn,exec} targets, so the run needs an x86_64
// Linux host to execute them. On the ubuntu-x86_64 CI leg this runs for real;
// on the ubuntu-ARM64 leg it is compiled out (an x86_64 ELF cannot execute
// there -- ENOEXEC), matching how the Windows/macOS legs compile it out. There
// is no aarch64 `.so` (ET_DYN) format flavor yet, so the ELF DYNAMIC round-trip
// has no arm64 runtime form (follow-up D-LK-ELF-DYN-AARCH64-FLAVOR); the local
// WSL run is the dev-box witness.
#if defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
TEST(FfiResolveLibraryRoundTrip, ElfDynamicRoundTripExitsFortyTwo) {
    ScratchDir scratch{Location::InsideRepo, "ffi-resolve-lib"};
    auto const dir = scratch.path();
    auto const libSrc  = writeSrc(dir, "dsslib.c", kLibSrc);
    auto const mainSrc = writeSrc(dir, "main.c", kMainSrc);

    // 1. DSS builds dsslib.so (ELF ET_DYN, c150 -- real-named .dynsym export).
    DiagnosticReporter libRep;
    ASSERT_EQ(buildOne(dir, {}, libSrc.string(),
                       "x86_64:elf64-x86_64-linux-dyn", libRep), 0);
    auto const soPath = dir / "dsslib.so";
    ASSERT_TRUE(fs::exists(soPath));
    ASSERT_TRUE(libraryExportsSymbol(soPath, "dss_lib_answer"));

    // 2. DSS builds main RESOLVING its extern against dsslib.so.
    DiagnosticReporter mainRep;
    ASSERT_EQ(buildOne(dir, {soPath}, mainSrc.string(),
                       "x86_64:elf64-x86_64-linux-exec", mainRep), 0);
    auto const mainPath = dir / "main";
    ASSERT_TRUE(fs::exists(mainPath));

    // 3. RUN main -> ld.so resolves DT_NEEDED dsslib.so (LD_LIBRARY_PATH) +
    //    libc, binds dss_lib_answer, returns 42.
    ::setenv("LD_LIBRARY_PATH", dir.string().c_str(), /*overwrite=*/1);
    auto const r = runBinary(mainPath, std::chrono::milliseconds{5000});
    ASSERT_TRUE(r.spawned)
        << "main must spawn under ld.so. " << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "THE acceptance criterion: exit 42 = dss_lib_answer() called "
           "through the dsslib.so binding the reader-consumer discovered.";
}

// silent-failure HIGH fold (b), ELF leg: the mixed bare-puts + own-lib program
// runs under ld.so (puts -> libc.so.6 via descriptor-fallthrough,
// dss_lib_answer -> dsslib.so via the read). Prints "hi" + exits 42.
// Witnessed locally under WSL; runs for real on the ubuntu CI legs.
TEST(FfiResolveLibraryRoundTrip, ElfMixedBareSystemExternAndOwnLibraryExitFortyTwo) {
    ScratchDir scratch{Location::InsideRepo, "ffi-resolve-lib"};
    auto const dir = scratch.path();
    auto const libSrc  = writeSrc(dir, "dsslib.c", kLibSrc);
    auto const mainSrc = writeSrc(dir, "mixed.c", kMixedSrc);

    DiagnosticReporter libRep;
    ASSERT_EQ(buildOne(dir, {}, libSrc.string(),
                       "x86_64:elf64-x86_64-linux-dyn", libRep), 0);
    auto const soPath = dir / "dsslib.so";

    DiagnosticReporter mainRep;
    ASSERT_EQ(buildOne(dir, {soPath}, mainSrc.string(),
                       "x86_64:elf64-x86_64-linux-exec", mainRep), 0)
        << "the mixed bare-puts + own-lib program must BUILD -- puts falls "
           "through to libc.so.6, dss_lib_answer binds to dsslib.so.";
    auto const mainPath = dir / "mixed";
    ASSERT_TRUE(fs::exists(mainPath));

    ::setenv("LD_LIBRARY_PATH", dir.string().c_str(), /*overwrite=*/1);
    auto const r = runBinary(mainPath, std::chrono::milliseconds{5000},
                             /*captureStdout=*/true);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "exit 42 = puts->libc + dss_lib_answer->dsslib.so both bound.";
    EXPECT_NE(r.capturedStdout.find("hi"), std::string::npos)
        << "puts must print 'hi'; got: [" << r.capturedStdout << "]";
}
#endif  // __linux__
