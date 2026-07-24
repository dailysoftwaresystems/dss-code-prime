// DSS Code Prime — integration tests.
//
// Drives the compiled `dss-code-prime` executable as a SUBPROCESS
// (vs the in-process `Program::compileFiles` path exercised by
// `tests/examples/examples_runner`). The two paths share the
// curated `examples/` corpus but exercise different surfaces:
//
//   * In-process (tests/examples/) — API + library link path.
//     Fast, runs every ctest cycle. Catches API/regression bugs.
//   * Subprocess (here) — full CLI surface (argv parsing, exit
//     codes, filesystem layout, output routing). Slower per
//     example but catches CLI-level regressions the in-process
//     path misses.
//
// **Always against current host platform** (user invariant
// 2026-06-02): each example's manifest declares a per-target
// `runOn` list naming the host OSes that may spawn the produced
// binary. This runner picks the FIRST target whose `runOn`
// includes the current host and uses THAT target spec; if no
// target matches the host, the example is skipped with a loud
// diagnostic (not silent — every example must run somewhere).
//
// User invariant (2026-06-02): strict asserts on every observable
// — exit codes EQ, no timeouts, no spawn failures. Wrong-value
// breaks the run.

#include "run_binary.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>  // std::sort (do not rely on a transitive include)
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>  // std::istreambuf_iterator (do not rely on a transitive include)
#include <optional>  // std::optional (per-target exitCode override)
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int passes  = 0;
int failures = 0;

void check(std::string const& description, bool condition,
           std::string const& detail = "") {
    if (condition) {
        std::cout << "  [PASS] " << description << "\n";
        ++passes;
    } else {
        std::cout << "  [FAIL] " << description;
        if (!detail.empty()) std::cout << " — " << detail;
        std::cout << "\n";
        ++failures;
    }
}

[[nodiscard]] std::string currentHostOs() noexcept {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

// D-LK10-ENTRY-ARM64 (v0.0.2 V2-1): host ARCH detection, returning the
// SAME identifier strings target specs use as their prefix (so a spec's
// target arch can be compared directly). The subprocess runner gates the
// cross-arch RUN exactly like the in-process examples_runner: a binary
// whose target arch differs from the host's needs an emulator, else the
// run is SKIPPED (never a spawn-failure). Without this gate an AArch64
// ELF reaches posix_spawn on an x86_64 host → ENOEXEC → false failure.
[[nodiscard]] std::string currentHostArch() noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "unknown";
#endif
}

// The target portion of a manifest spec ("arm64:elf64-aarch64-linux-exec"
// → "arm64"). This IS the arch identifier compared against
// currentHostArch().
[[nodiscard]] std::string specTargetArch(std::string const& spec) {
    auto const colon = spec.find(':');
    return colon == std::string::npos ? spec : spec.substr(0, colon);
}

// Resolve an executable name against $PATH (e.g. "qemu-aarch64"),
// returning its full path or "" if not found. Gates cross-arch example
// runs on emulator availability — absent ⇒ skip (never a test failure).
// AGNOSTIC: the emulator name is supplied by the manifest, not hardcoded.
[[nodiscard]] std::string findOnPath(std::string const& exe) {
    char const* const pathEnv = std::getenv("PATH");
    if (pathEnv == nullptr) return {};
#if defined(_WIN32)
    char const sep = ';';
    std::vector<std::string> const exts = {"", ".exe"};
#else
    char const sep = ':';
    std::vector<std::string> const exts = {""};
#endif
    std::string const path = pathEnv;
    std::size_t start = 0;
    while (start <= path.size()) {
        auto const end = path.find(sep, start);
        std::string const dir = path.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
            for (auto const& ext : exts) {
                fs::path const cand = fs::path(dir) / (exe + ext);
                std::error_code ec;
                if (fs::exists(cand, ec) && fs::is_regular_file(cand, ec)) {
                    return cand.generic_string();
                }
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

// Quote a path for safe inclusion in a `std::system` command line
// — wraps in double quotes (PowerShell + cmd.exe + POSIX shells
// all honor "..." for argument grouping). The path's own contents
// don't need escaping for our cases (paths in test fixtures don't
// contain double quotes).
[[nodiscard]] std::string quote(std::string const& s) {
    return "\"" + s + "\"";
}

// Wrap a full command in the platform's `std::system()` shell
// conventions. On Windows, `std::system()` invokes `cmd /S /C`
// which strips a SINGLE leading + trailing pair of double quotes
// from the command line. When the command itself contains quoted
// tokens (e.g. an exe path with spaces), this strip eats the
// outer quotes we wanted preserved — the canonical fix is to add
// an extra outer pair. POSIX shells have no such stripping; the
// extra pair would be passed verbatim as args. Hence the
// platform-conditional wrap.
[[nodiscard]] std::string shellWrap(std::string const& cmd) {
#if defined(_WIN32)
    return "\"" + cmd + "\"";
#else
    return cmd;
#endif
}

// D-EXAMPLES-RUNNER-MULTI-ARTIFACT (c171): one prerequisite LIBRARY artifact a
// target build depends on — built FIRST, then threaded into the dependent
// build's `--resolve-library`. Mirrors the in-process examples_runner's
// `DependsOnArtifact` so BOTH corpus harnesses accept the same manifests.
struct DependsOnArtifact {
    std::vector<std::string> sources;
    bool                     multiCu = false;
    std::string              spec;
    std::string              artifact;
    // NESTED prerequisites this dependency ITSELF resolves against — built
    // FIRST (into the same out dir) and threaded into THIS dep's own
    // `--resolve-library`. Mirrors the in-process examples_runner's
    // D-EXAMPLES-RUNNER-NESTED-DEPENDSON so BOTH corpus harnesses execute a fat
    // `-staticlib` MERGE manifest (D-FF1-STATICLIB-FAT-ARCHIVE) identically:
    // without this the subprocess runner silently dropped the nested entry,
    // building the fat lib WITHOUT the merge (its member unresolved at link →
    // the produced exec fault-loaded at runtime). Empty (the default) ⇒ a
    // single-level dependency, EXACTLY the pre-nesting behavior.
    std::vector<DependsOnArtifact> dependsOn;
};

struct ExampleTarget {
    std::string              spec;
    std::string              artifact;
    std::vector<std::string> runOn;
    // D-LK10-ENTRY-ARM64: optional emulator command (e.g. "qemu-aarch64")
    // used when the target arch differs from the host arch. Empty ⇒
    // native execution only (the pre-V2-1 default).
    std::string              emulator;
    // C11/C23 6.4.5 (wchar_platform_width): optional PER-TARGET exit-code override
    // for a source whose return value is platform-divergent (e.g. sizeof(wchar_t)
    // is 2 on pe64, 4 on elf/mach-o). Absent ⇒ the manifest `exitCode` applies.
    // Mirrors the in-process examples_runner so BOTH corpus harnesses agree.
    std::optional<std::int64_t> exitCodeOverride;
    // D-EXAMPLES-RUNNER-MULTI-ARTIFACT (c171): prerequisite library artifacts
    // this target links against (built FIRST, threaded into `--resolve-library`).
    // Empty (the default) ⇒ a plain single-artifact build. Mirrors the
    // in-process examples_runner.
    std::vector<DependsOnArtifact> dependsOn;
};

// V2-4 Part C (D-DIAG-CLI-POSITION-RENDER-AND-ASSERT): one declared
// expected diagnostic for an EXPECT-ERROR example. Mirrors the in-process
// examples_runner so BOTH corpus harnesses accept the same manifests.
struct ExpectedDiagnostic {
    std::string   code;
    std::uint32_t line = 0;
    std::uint32_t col  = 0;
    // D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN (#4): true (default) ⇒ the diagnostic
    // carries a source span and the CLI's positioned renderer prints `:line:col`
    // (this harness greps for it). false ⇒ the diagnostic is emitted at a tier
    // with NO source span (e.g. `L_OverAlignedStackLocal` from the LIR calling-
    // convention frame layout), so the CLI renders it code-only
    // (`error[<code>]`) — the HONEST output of a span-less tier; this harness
    // then greps for that form instead of a fabricated position. Parsed from an
    // optional `"positioned": false` manifest key.
    bool          positioned = true;
};

struct ExampleManifest {
    std::string                language;
    // Multi-CU (CU6): "sources":[...] makes each file its OWN CompilationUnit; the CLI's
    // `--compile a.c b.c` links them into one image (gcc/clang semantics — separate TUs,
    // the linker resolves cross-file references, LK11). The single "source":"x.c" form is
    // one file. `sources` is the canonical file list for either form (single → a 1-element
    // list); `multiCu` records which spelling the manifest used (diagnostic only — the CLI
    // derives multi-vs-single from the file count, not from this flag).
    std::vector<std::string>   sources;
    bool                       multiCu = false;
    std::int64_t               exitCode = 0;
    std::vector<ExampleTarget> targets;
    // V2-4 Part C: NON-EMPTY ⇒ EXPECT-ERROR example. The CLI must REJECT
    // the malformed source (non-zero exit) and emit each diagnostic's
    // positioned `:line:col` (the Part A renderer) on stderr.
    std::vector<ExpectedDiagnostic> expectDiagnostics;
};

// D-EXAMPLES-RUNNER-MULTI-ARTIFACT + nested extension (CLI-subprocess mirror of
// the in-process examples_runner's parseDependsOnEntry): parse ONE `dependsOn`
// entry, RECURSING into its own nested `dependsOn` (a fat `-staticlib` that
// MERGES an input `-staticlib`). The SAME helper serves the target-level parse
// AND the recursion, so a dependency can nest at any depth. A missing
// `dependsOn` key leaves the nested vector empty ⇒ the pre-nesting single-level
// shape (every c171 example unchanged). Returns false (after a loud std::cerr)
// on any malformed field.
[[nodiscard]] bool parseDependsOnEntry(nlohmann::json const& d,
                                       fs::path const&       path,
                                       DependsOnArtifact&    out) {
    out.spec     = d.value("spec", "");
    out.artifact = d.value("artifact", "");
    out.multiCu  = d.value("multiCu", false);
    if (d.contains("sources") && d.at("sources").is_array()) {
        for (auto const& s : d.at("sources")) {
            if (s.is_string()) out.sources.push_back(s.get<std::string>());
        }
    }
    if (out.sources.empty() || out.spec.empty() || out.artifact.empty()) {
        std::cerr << "  'dependsOn' entry needs non-empty 'sources', 'spec', "
                     "and 'artifact' in " << path.generic_string() << "\n";
        return false;
    }
    // Nested `dependsOn`: this entry's OWN prerequisites (built first, resolved
    // into this entry's build). The recursion is the ONLY new behavior — a
    // missing key leaves the nested vector empty (the pre-nesting shape).
    if (d.contains("dependsOn")) {
        if (!d.at("dependsOn").is_array()) {
            std::cerr << "  'dependsOn' entry 'dependsOn' must be an array in "
                      << path.generic_string() << "\n";
            return false;
        }
        for (auto const& nested : d.at("dependsOn")) {
            DependsOnArtifact nestedDep;
            if (!parseDependsOnEntry(nested, path, nestedDep)) return false;
            out.dependsOn.push_back(std::move(nestedDep));
        }
    }
    return true;
}

[[nodiscard]] bool readManifest(fs::path const& path, ExampleManifest& out) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "  cannot open " << path.generic_string() << "\n";
        return false;
    }
    nlohmann::json j;
    try { in >> j; }
    catch (std::exception const& e) {
        std::cerr << "  JSON parse failed for " << path.generic_string()
                  << ": " << e.what() << "\n";
        return false;
    }
    out.language = j.value("language", "");
    // "sources":[...] (multi-CU, CU6) takes precedence over "source" (single file). Mirror
    // the in-process examples_runner so BOTH corpus harnesses accept the same manifests.
    if (j.contains("sources")) {
        if (!j.at("sources").is_array() || j.at("sources").empty()) {
            std::cerr << "  'sources' must be a non-empty array of file names in "
                      << path.generic_string() << "\n";
            return false;
        }
        for (auto const& s : j.at("sources")) {
            if (!s.is_string()) {
                std::cerr << "  'sources' entries must be strings in "
                          << path.generic_string() << "\n";
                return false;
            }
            out.sources.push_back(s.get<std::string>());
        }
        out.multiCu = true;
    } else if (auto single = j.value("source", std::string{}); !single.empty()) {
        out.sources = {single};
    } else {
        std::cerr << "  manifest requires 'source' (single CU) or 'sources' (multi-CU): "
                  << path.generic_string() << "\n";
        return false;
    }
    // V2-4 Part C: expect-error diagnostics (parsed first — their presence
    // relaxes the exitCode requirement, since no binary is produced/run).
    if (j.contains("expectDiagnostics")) {
        if (!j.at("expectDiagnostics").is_array() || j.at("expectDiagnostics").empty()) {
            std::cerr << "  'expectDiagnostics' must be a non-empty array in "
                      << path.generic_string() << "\n";
            return false;
        }
        for (auto const& d : j.at("expectDiagnostics")) {
            if (!d.is_object()
                || !d.contains("code") || !d.at("code").is_string()
                || !d.contains("line") || !d.at("line").is_number_unsigned()
                || !d.contains("col")  || !d.at("col").is_number_unsigned()) {
                std::cerr << "  each expectDiagnostics entry needs string 'code'"
                             " + unsigned 'line' + unsigned 'col' in "
                          << path.generic_string() << "\n";
                return false;
            }
            ExpectedDiagnostic ed;
            ed.code = d.at("code").get<std::string>();
            ed.line = d.at("line").get<std::uint32_t>();
            ed.col  = d.at("col").get<std::uint32_t>();
            // #4: optional — a span-less tier's diagnostic is code-only.
            if (d.contains("positioned")) {
                if (!d.at("positioned").is_boolean()) {
                    std::cerr << "  expectDiagnostics 'positioned' must be a "
                                 "boolean in " << path.generic_string() << "\n";
                    return false;
                }
                ed.positioned = d.at("positioned").get<bool>();
            }
            out.expectDiagnostics.push_back(std::move(ed));
        }
    }
    if (j.contains("exitCode")) {
        if (!j.at("exitCode").is_number_integer()) {
            std::cerr << "  'exitCode' must be an integer in "
                      << path.generic_string() << "\n";
            return false;
        }
        out.exitCode = j.at("exitCode").get<std::int64_t>();
    } else if (out.expectDiagnostics.empty()) {
        std::cerr << "  missing integer 'exitCode' (required unless"
                     " 'expectDiagnostics' is present) in "
                  << path.generic_string() << "\n";
        return false;
    }
    if (!j.contains("targets") || !j.at("targets").is_array()) {
        std::cerr << "  missing 'targets' array in "
                  << path.generic_string() << "\n";
        return false;
    }
    for (auto const& t : j.at("targets")) {
        ExampleTarget et;
        et.spec     = t.value("spec", "");
        et.artifact = t.value("artifact", "");
        et.emulator = t.value("emulator", "");
        if (t.contains("runOn") && t.at("runOn").is_array()) {
            for (auto const& s : t.at("runOn")) {
                if (s.is_string()) et.runOn.push_back(s.get<std::string>());
            }
        }
        // C11/C23 6.4.5: optional per-target exit-code override.
        if (t.contains("exitCode")) {
            if (!t.at("exitCode").is_number_integer()) {
                std::cerr << "  target 'exitCode' must be an integer in "
                          << path.generic_string() << "\n";
                return false;
            }
            et.exitCodeOverride = t.at("exitCode").get<std::int64_t>();
        }
        // D-EXAMPLES-RUNNER-MULTI-ARTIFACT (c171): optional prerequisite
        // library artifacts (mirrors the in-process examples_runner).
        if (t.contains("dependsOn")) {
            if (!t.at("dependsOn").is_array()) {
                std::cerr << "  target 'dependsOn' must be an array in "
                          << path.generic_string() << "\n";
                return false;
            }
            for (auto const& d : t.at("dependsOn")) {
                DependsOnArtifact dep;
                if (!parseDependsOnEntry(d, path, dep)) return false;
                et.dependsOn.push_back(std::move(dep));
            }
        }
        out.targets.push_back(std::move(et));
    }
    return true;
}

// D-EXAMPLES-RUNNER-MULTI-ARTIFACT + nested extension (CLI-subprocess mirror of
// the in-process examples_runner's buildDependencyArtifact): build ONE
// prerequisite LIBRARY via a `dss-code-prime` SUBPROCESS, RECURSIVELY building
// its own nested `dependsOn` FIRST (into the same out dir) and threading their
// paths into THIS dep's `--resolve-library`. So a fat `-staticlib` dep that
// nests an input `-staticlib` MERGES it (D-FF1-STATICLIB-FAT-ARCHIVE): the
// nested input `.lib`/`.a` is built, then the fat build resolves it and bundles
// its members in — the exact chain the CLI USER path performs. ORDER-CORRECT (a
// prerequisite exists on disk before its dependent build runs), arbitrary depth,
// no example-name special-casing. Emits a strict `check` per build; returns the
// built artifact path, or nullopt (a `check` already recorded the FAIL) on any
// build / artifact-missing failure. A dep with NO nested `dependsOn` is
// byte-identical to the pre-nesting single-level build (every existing example
// unchanged).
[[nodiscard]] std::optional<fs::path>
buildDependsOnArtifactCli(std::string const&       compiler,
                          DependsOnArtifact const& dep,
                          fs::path const&          exampleDir,
                          fs::path const&          outDir,
                          std::string const&       language,
                          std::string const&       exampleName) {
    // Nested prerequisites FIRST (order-correct): each must exist on disk
    // before this dep's own build resolves against it.
    std::string resolveArgs;
    for (auto const& nested : dep.dependsOn) {
        auto nestedPath = buildDependsOnArtifactCli(
            compiler, nested, exampleDir, outDir, language, exampleName);
        if (!nestedPath.has_value()) return std::nullopt;  // check already fired
        resolveArgs += " --resolve-library " + quote(nestedPath->string());
    }

    std::string depCompileArgs;
    for (auto const& s : dep.sources) {
        depCompileArgs += " " + quote((exampleDir / s).string());
    }
    auto const depLog = outDir / (dep.artifact + ".buildlog");
    std::string const depCmd = quote(compiler)
        + " --compile"   + depCompileArgs
        + " --language " + language
        + " --target "   + dep.spec
        + resolveArgs
        + " --output "   + quote(outDir.string())
        + " > " + quote(depLog.string()) + " 2>&1";
    int const depRc = std::system(shellWrap(depCmd).c_str());
    auto const depArtifact = outDir / dep.artifact;
    bool const depOk = (depRc == 0) && fs::exists(depArtifact)
                    && fs::file_size(depArtifact) > 0u;
    check(exampleName + ": dependsOn library " + dep.spec + " built ("
          + depArtifact.generic_string() + ", buildlog: "
          + depLog.generic_string() + ")", depOk);
    if (!depOk) return std::nullopt;
    return depArtifact;
}

// Drive ONE example through the CLI subprocess path:
//   1. spawn `dss-code-prime --compile <src> --language <l> --target <spec> --output <outdir>`
//   2. check rc == 0
//   3. check artifact file exists at outdir/<artifact>
//   4. spawn artifact, capture exit code via run_binary.hpp
//   5. ASSERT exit code == manifest.exitCode
//
// All checks are strict — wrong values fail the test.
void runExampleViaCli(std::string const& compiler,
                      fs::path const&    exampleDir,
                      fs::path const&    outputBase,
                      ExampleManifest const& m) {
    auto const host = currentHostOs();
    ExampleTarget const* target = nullptr;
    for (auto const& t : m.targets) {
        for (auto const& osName : t.runOn) {
            if (osName == host) {
                target = &t;
                break;
            }
        }
        if (target != nullptr) break;
    }
    if (target == nullptr) {
        std::cout << "  [SKIP] " << exampleDir.filename().generic_string()
                  << " — no target's runOn includes host=" << host
                  << " (cross-host compile-only is exercised by"
                  << " tests/examples/ in-process runner)\n";
        return;
    }

    auto const exampleName = exampleDir.filename().generic_string();
    // Target spec format is `<cpu>:<format>`; the `:` is illegal
    // in Windows path components. Substitute `_` to derive a
    // filesystem-safe sub-directory name. The substitution is
    // local to disk layout and never leaks back into the CLI's
    // --target argument (which keeps the canonical `:`-form).
    auto const specDir = [&]() {
        std::string s = target->spec;
        for (auto& c : s) if (c == ':') c = '_';
        return s;
    }();
    auto const outDir =
        outputBase / "ex" / exampleName / specDir;
    fs::create_directories(outDir);

    // D-EXAMPLES-RUNNER-MULTI-ARTIFACT (c171): build each prerequisite LIBRARY
    // artifact FIRST (into the same out dir) via a separate CLI invocation,
    // then thread its path into the dependent build's `--resolve-library`.
    // Mirrors the in-process examples_runner; a dep build failure is a test
    // failure (the dependent build could not resolve its externs otherwise).
    // D-EXAMPLES-RUNNER-MULTI-ARTIFACT + nested extension: build each
    // prerequisite library (recursively building its OWN nested dependsOn
    // first — the fat-archive merge chain) and thread the produced path into
    // this target's `--resolve-library`. A dep build failure is a test failure
    // (buildDependsOnArtifactCli fired the strict `check`).
    std::string resolveArgs;
    for (auto const& dep : target->dependsOn) {
        auto depArtifact = buildDependsOnArtifactCli(
            compiler, dep, exampleDir, outDir, m.language, exampleName);
        if (!depArtifact.has_value()) return;  // check already fired
        resolveArgs += " --resolve-library " + quote(depArtifact->string());
    }

    // Build the CLI invocation. The compiler binary path may
    // contain spaces (Visual Studio tooling drops it under
    // "C:\Program Files (x86)\..."); quote both the binary AND
    // every path argument. Redirect stdout+stderr to a file so
    // failures retain the compiler diagnostics for diagnosis.
    //
    // `--compile <file>...` takes a space-separated file list in ONE invocation; a multi-CU
    // example passes ALL its sources here and the CLI links each as its own translation unit
    // (the driver routes >1 file to compileUnits — gcc/clang semantics). Each path is quoted
    // independently.
    std::string compileArgs;
    for (auto const& s : m.sources) {
        compileArgs += " " + quote((exampleDir / s).string());
    }
    auto const cliLog = outDir / "cli.log";
    std::string cmd = quote(compiler)
        + " --compile"   + compileArgs
        + " --language " + m.language
        + " --target "   + target->spec
        + resolveArgs
        + " --output "   + quote(outDir.string())
        + " > " + quote(cliLog.string()) + " 2>&1";

    int const sysRc = std::system(shellWrap(cmd).c_str());

    auto const compileOk = [&]() -> bool {
        if (sysRc == 0) return true;
        check(exampleName + ": compile rc == 0 (rc=" + std::to_string(sysRc)
              + ", cli log: " + cliLog.generic_string() + ")",
              false);
        return false;
    }();
    if (!compileOk) return;
    check(exampleName + ": compile exits 0", true);

    auto const artifactPath = outDir / target->artifact;
    bool const artifactExists = fs::exists(artifactPath);
    check(exampleName + ": artifact exists at "
          + artifactPath.generic_string(), artifactExists);
    if (!artifactExists) return;

    bool const artifactNonEmpty =
        artifactExists && fs::file_size(artifactPath) > 0u;
    check(exampleName + ": artifact non-empty", artifactNonEmpty);
    if (!artifactNonEmpty) return;

    // D-LK10-ENTRY-ARM64: cross-ARCH execution gate. The runOn match
    // above reconciled the host OS; now reconcile the host ARCH. A
    // binary whose target arch differs from the host's cannot exec
    // natively — it needs the manifest's emulator (e.g. qemu-aarch64
    // for an AArch64 ELF on x86_64). Absent emulator (or not on PATH)
    // ⇒ SKIP the run (never a spawn-failure — the compile already
    // asserted clean). Mirrors the in-process examples_runner so the
    // subprocess path does not false-fail on an x86_64 CI host that
    // lacks qemu; the native-arm64 CI leg runs it for real.
    std::vector<std::string> launcherPrefix;
    if (std::string const targetArch = specTargetArch(target->spec);
        !targetArch.empty() && targetArch != currentHostArch()) {
        if (target->emulator.empty()) {
            std::cout << "  [SKIP] " << exampleName << " — target arch '"
                      << targetArch << "' != host '" << currentHostArch()
                      << "' with no emulator declared\n";
            return;
        }
        auto const emuPath = findOnPath(target->emulator);
        if (emuPath.empty()) {
            std::cout << "  [SKIP] " << exampleName << " — emulator '"
                      << target->emulator
                      << "' not found on PATH (cross-arch run)\n";
            return;
        }
        launcherPrefix.push_back(emuPath);
    }

    auto const result = dss::test_support::runBinary(
        artifactPath, std::chrono::milliseconds{5000}, false, launcherPrefix);
    check(exampleName + ": spawn succeeded (diag='"
          + result.diagnostic + "')", result.spawned);
    if (!result.spawned) return;

    check(exampleName + ": no timeout", !result.timedOut);
    if (result.timedOut) return;

    // C11/C23 6.4.5: the per-target override (when present) is the authority for
    // THIS target's exit code; otherwise the manifest-level `exitCode`.
    std::int64_t const expectedExit =
        target->exitCodeOverride.has_value() ? *target->exitCodeOverride : m.exitCode;
    bool const exitMatches =
        static_cast<std::int64_t>(result.exitCode) == expectedExit;
    check(exampleName + ": OS exit code == "
          + std::to_string(expectedExit)
          + " (got " + std::to_string(result.exitCode) + ")",
          exitMatches);
}

// V2-4 Part C: drive an EXPECT-ERROR example through the CLI SUBPROCESS.
// The malformed source MUST be REJECTED (non-zero exit) and the CLI's
// positioned renderer (Part A) MUST print each diagnostic's `:line:col`
// on stderr. The front-end error is target-independent, so the first
// declared target's spec drives the compile (no runOn host gate needed —
// nothing is spawned). The CODE is asserted by name in the in-process
// examples_runner; here we pin the format-stable positioned coordinate.
void runErrorExampleViaCli(std::string const& compiler,
                           fs::path const&    exampleDir,
                           fs::path const&    outputBase,
                           ExampleManifest const& m) {
    auto const exampleName = exampleDir.filename().generic_string();
    if (m.targets.empty()) {
        check(exampleName + ": expect-error example declares a target spec", false);
        return;
    }
    auto const& spec = m.targets.front().spec;

    auto const specDir = [&]() {
        std::string s = spec;
        for (auto& c : s) if (c == ':') c = '_';
        return s;
    }();
    auto const outDir = outputBase / "ex" / exampleName / specDir;
    fs::create_directories(outDir);

    std::string compileArgs;
    for (auto const& s : m.sources) {
        compileArgs += " " + quote((exampleDir / s).string());
    }
    auto const cliLog = outDir / "cli.log";
    std::string cmd = quote(compiler)
        + " --compile"   + compileArgs
        + " --language " + m.language
        + " --target "   + spec
        + " --output "   + quote(outDir.string())
        + " > " + quote(cliLog.string()) + " 2>&1";
    int const sysRc = std::system(shellWrap(cmd).c_str());

    // The CLI MUST reject the malformed source (a successful compile of a
    // known-bad source is a real regression).
    check(exampleName + ": CLI rejects malformed source (rc != 0)",
          sysRc != 0,
          "rc=" + std::to_string(sysRc) + ", cli log: "
          + cliLog.generic_string());

    std::ifstream f(cliLog.string());
    std::string const body((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    for (auto const& e : m.expectDiagnostics) {
        if (e.positioned) {
            std::string const posn = ":" + std::to_string(e.line)
                                   + ":" + std::to_string(e.col);
            check(exampleName + ": CLI emits positioned diagnostic " + e.code
                  + " at " + posn,
                  body.find(posn) != std::string::npos,
                  "cli.log lacks '" + posn + "':\n" + body);
        } else {
            // #4: a span-less-tier diagnostic renders code-only as
            // `error[<code>]` (drainDiagnosticsToStderr routes a buffer-less
            // diagnostic to the code-only one-liner). Assert THAT honest form
            // rather than a fabricated `:line:col`.
            std::string const band = "error[" + e.code + "]";
            check(exampleName + ": CLI emits code-only diagnostic " + band,
                  body.find(band) != std::string::npos,
                  "cli.log lacks '" + band + "':\n" + body);
        }
    }
}

void runAllExamples(std::string const& compiler,
                    fs::path const&    examplesRoot,
                    fs::path const&    outputBase) {
    if (!fs::is_directory(examplesRoot)) {
        std::cerr << "[ERROR] examples root not a directory: "
                  << examplesRoot.generic_string() << "\n";
        ++failures;
        return;
    }
    // Walk examples/<lang>/<name>/expected.json. The tree depth
    // is fixed (2 levels) so a recursive iterator is overkill;
    // a nested loop is clearer.
    std::vector<fs::path> manifestPaths;
    for (auto const& langEntry : fs::directory_iterator(examplesRoot)) {
        if (!langEntry.is_directory()) continue;
        for (auto const& nameEntry :
             fs::directory_iterator(langEntry.path())) {
            if (!nameEntry.is_directory()) continue;
            auto const mp = nameEntry.path() / "expected.json";
            if (fs::exists(mp)) manifestPaths.push_back(mp);
        }
    }
    // Deterministic order for reproducible logs.
    std::sort(manifestPaths.begin(), manifestPaths.end());

    if (manifestPaths.empty()) {
        std::cerr << "[ERROR] no examples found under "
                  << examplesRoot.generic_string() << "\n";
        ++failures;
        return;
    }
    std::cout << "[Test 3] Examples corpus via CLI subprocess ("
              << manifestPaths.size() << " manifests)\n";
    for (auto const& mp : manifestPaths) {
        ExampleManifest m;
        if (!readManifest(mp, m)) {
            ++failures;
            continue;
        }
        // V2-4 Part C: an expectDiagnostics manifest asserts a rejected
        // compile + positioned CLI diagnostics; otherwise the standard
        // compile + run path.
        if (m.expectDiagnostics.empty()) {
            runExampleViaCli(compiler, mp.parent_path(), outputBase, m);
        } else {
            runErrorExampleViaCli(compiler, mp.parent_path(), outputBase, m);
        }
    }
    std::cout << "\n";
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: integrated_tests <path-to-dss-code-prime> "
                  << "<path-to-examples-root>\n";
        return 1;
    }

    std::string const compiler = fs::absolute(argv[1]).string();
    fs::path const examplesRoot = fs::absolute(argv[2]);
    fs::path const outputBase   =
        fs::temp_directory_path() / "dss-integrated-tests";

    fs::remove_all(outputBase);
    fs::create_directories(outputBase);

    std::cout << "=== DSS Code Prime — Integration Tests ===\n"
              << "Compiler:      " << compiler << "\n"
              << "Examples root: " << examplesRoot.generic_string() << "\n"
              << "Output:        " << outputBase.string() << "\n"
              << "Host OS:       " << currentHostOs() << "\n\n";

    // ── Test 1: Default invocation prints ready message ──
    std::cout << "[Test 1] Default invocation\n";
    {
        std::string cmd = quote(compiler) + " > "
            + quote((outputBase / "default_output.txt").string())
            + " 2>&1";
        int const rc = std::system(shellWrap(cmd).c_str());
        check("Exit code is 0", rc == 0, "got " + std::to_string(rc));
        std::ifstream f((outputBase / "default_output.txt").string());
        std::string line;
        std::getline(f, line);
        check("Prints ready message",
              line.find("DSS Code Prime compiler ready.")
                  != std::string::npos,
              "got: " + line);
    }
    std::cout << "\n";

    // ── Test 2: Unknown flag rejected with non-zero exit ──
    std::cout << "[Test 2] Unknown flag is rejected\n";
    {
        std::string cmd = quote(compiler) + " --no-such-flag > "
            + quote((outputBase / "unknown_flag.txt").string())
            + " 2>&1";
        int const rc = std::system(shellWrap(cmd).c_str());
        // `std::system` exit-code encoding varies across hosts
        // (POSIX shifts; Windows passes through). Either form
        // satisfies "non-zero" — that's what we pin.
        check("Exit code indicates rejection",
              rc != 0,
              "got " + std::to_string(rc));
        std::ifstream f((outputBase / "unknown_flag.txt").string());
        std::string body((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
        check("Mentions 'unknown flag'",
              body.find("unknown flag") != std::string::npos,
              "stderr: " + body);
    }
    std::cout << "\n";

    // ── Test 3: Examples corpus via CLI subprocess ──
    runAllExamples(compiler, examplesRoot, outputBase);

    std::cout << "===========================================\n"
              << "Results: " << passes << " passed, "
              << failures << " failed\n";

    fs::remove_all(outputBase);

    return failures > 0 ? 1 : 0;
}
