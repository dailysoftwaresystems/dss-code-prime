// `examples/` curated-source ctest harness. One entry per
// `examples/<lang>/<name>/expected.json` (registered at cmake-time
// via `file(GLOB_RECURSE ...)` in this dir's CMakeLists.txt) drives
// the full DSS pipeline via `Program::compileFiles`, then spawns
// the produced binary via `run_binary.hpp` and asserts the exact
// OS exit code. All asserts are STRICT — wrong-diagnostic, missing-
// artifact, spawn-failure, timeout, or wrong-exit-code all break
// the test loud.
//
// PROJECT MODE (D-EXAMPLES-RUNNER-PROJECT-MANIFEST): a manifest may name a
// `.dss-project.json` via the top-level `"project"` key INSTEAD of
// `source`/`sources`, and the build then goes through
// `Program::compileProject` — the only driver entry point that expands the
// manifest's source GLOBS and runs its `preBuildScripts`/`postBuildScripts`
// hooks. Three consequences are handled explicitly and are documented at their
// sites: the project manifest's own `targets[]` is the build authority (a
// corpus target's `spec` is a MIRROR, cross-checked); the artifact lands under
// a per-format subdirectory; and the compile must run with the mirrored scratch
// dir as the PROCESS working directory, because that is what a generated source
// resolves against. The CLI-subprocess sibling implements the same three.
//
// User invariant (verbatim, 2026-06-02): "please don't forget to
// perform strict asserts on the example harness run results....
// this is important". Hence ASSERT_EQ on every observable, never
// EXPECT_GT.
//
// Cross-host policy: examples whose target spec produces a binary
// for a different host OS (e.g. ELF-Linux when running on Windows)
// compile but SKIP the run step. The compile step still asserts
// strict zero-diagnostic success — a regression in cross-format
// emission surfaces even on the wrong host. The `runOn` array in
// `expected.json` lists host OS names ("windows" / "linux" /
// "darwin") on which the binary should spawn.
//
// A SKIP IS NOT A PASS (D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT). Every
// declared arm — every target x every optimizedPipelines arm — lands in an
// `ArmVerdictLedger` with a named reason, and the entry prints the accounting
// on stdout. Skips whose cause is the MACHINE rather than the manifest (a
// declared `emulator` that is not on PATH) are a warning by default and a hard
// failure under `DSS_STRICT_ARM_VERDICTS=1`. The vocabulary, the strict parse
// and the emulator lint are shared with the CLI-subprocess sibling
// (`integrated_tests/runner.cpp`) via `tests/test_support/arm_verdict_ledger.hpp`
// — a capability in one corpus harness and not the other is a silent harness
// bug, which is how this defect survived in both for as long as it did.

#include "arm_verdict_ledger.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "opt/optimizer.hpp"
#include "program/program.hpp"
#include "repo_root.hpp"   // the single-definition-site lint reads BOTH runners
                           // and the shared header off disk
#include "run_binary.hpp"
#include "scratch_dir.hpp"
#include "stage_tree.hpp"  // recursive corpus staging — ONE copy, shared with
                           // integrated_tests/runner.cpp

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>  // std::error_code (do not rely on a transitive
                         // include from <filesystem>)
#include <vector>

#if !defined(_WIN32)
  #include <sys/resource.h>  // RLIMIT_STACK assertion in the harness-wiring pin
#endif

namespace fs = std::filesystem;
using namespace dss;
using namespace dss::test_support;

namespace {

// D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT: `currentHostOs`, `currentHostArch`,
// `specTargetArch` and `findOnPath` used to live here AND, byte-identically, in
// `integrated_tests/runner.cpp`. They now come from the shared
// `arm_verdict_ledger.hpp`, so the two corpus harnesses cannot disagree about
// what host they are on or whether a declared emulator exists.
using ::dss::test_support::ArmVerdict;
using ::dss::test_support::ArmVerdictLedger;
using ::dss::test_support::currentHostArch;
using ::dss::test_support::currentHostOs;
using ::dss::test_support::findOnPath;
using ::dss::test_support::qemuSysrootHint;
using ::dss::test_support::specTargetArch;

// D-EXAMPLES-RUNNER-MULTI-ARTIFACT (c171): one PREREQUISITE artifact a
// target build depends on — a LIBRARY the runner builds FIRST, then threads
// into the dependent build's `--resolve-library` (the c162 reader-consumer
// surface). PER-TARGET (on `ExampleTarget`) so one example can express the
// round-trip on each platform with a matching library format (a PE `.lib` for
// the pe64 target, an ELF `.a` for the elf64 target, ...). `sources` are file
// names in the SAME example dir (copied to the scratch build dir like the main
// sources); `spec` is the library's own `<target>:<format>` (typically a
// `-staticlib` or `-dyn`/`-dll` format); `artifact` is its output file name
// (e.g. "dsslib.lib" / "libdsslib.a"). `multiCu` selects compileUnits vs
// compileFiles for the library, exactly as the top-level example does.
struct DependsOnArtifact {
    std::vector<std::string> sources;
    bool                     multiCu = false;
    std::string              spec;
    std::string              artifact;
    // NESTED prerequisites this dependency ITSELF resolves against — built
    // FIRST (into the same scratch out dir) and threaded into THIS dep's own
    // `--resolve-library`. Enables a fat `-staticlib` dep that MERGES a nested
    // input `-staticlib` (D-FF1-STATICLIB-FAT-ARCHIVE): to witness the merge a
    // dependency must itself resolve an INPUT archive. Empty (the default) ⇒ a
    // single-level dependency, EXACTLY the pre-nesting behavior (every c171
    // example unchanged). Arbitrary depth; fully generic (no example-name
    // special-casing).
    std::vector<DependsOnArtifact> dependsOn;
};

struct ExampleTarget {
    std::string                  spec;
    std::string                  artifact;
    std::vector<std::string>     runOn;  // host OS names allowed to spawn
    // D-EXAMPLES-RUNNER-MULTI-ARTIFACT (c171): prerequisite LIBRARY artifacts
    // this target links against. The runner builds each FIRST (into the same
    // scratch out dir) and passes their output paths to the dependent build's
    // `--resolve-library` (`Program::setResolveLibraries`). Empty (the
    // default) ⇒ a plain single-artifact build (every pre-c171 example).
    std::vector<DependsOnArtifact> dependsOn;
    // D-LK10-ENTRY-ARM64: optional emulator command (e.g.
    // "qemu-aarch64") used when the target arch differs from the host
    // arch. Empty ⇒ native execution only (the pre-V2-1 default).
    std::string                  emulator;
    // Model 3 (2026-06-09): optional PER-TARGET expected stdout. When present it
    // OVERRIDES the manifest-level `expectedStdout` for THIS target only — needed
    // when one source prints platform-divergent bytes (e.g. `puts` emits
    // "hello\r\n" via Windows msvcrt CRLF translation but "hello\n" on
    // linux/macos). Absent ⇒ the manifest-level pin applies (existing behavior).
    std::optional<std::string>   expectedStdoutOverride;
    // C11/C23 6.4.5 (wchar_platform_width): optional PER-TARGET expected exit code.
    // When present it OVERRIDES the manifest-level `exitCode` for THIS target only —
    // needed when one source returns a platform-divergent value (e.g.
    // `sizeof(wchar_t)` is 2 on pe64 but 4 on elf/mach-o). Absent ⇒ the manifest
    // `exitCode` applies (existing behavior). The differential (optimized) arm still
    // compares to the baseline, so it follows this per-target choice for free.
    std::optional<std::int64_t>  exitCodeOverride;
};

// D-OPT1-DIFFERENTIAL-VERIFY-RUNNER (OPT2 cycle 1): a per-manifest
// declaration of an OPTIMIZED arm whose binary must produce the
// SAME exit code + stdout as the baseline (Identity-only) arm. The
// 5 corpus negative pins (dce_negative_pin, const_fold_inside_expr,
// copy_prop_across_join, licm_conditional_mutation, cse_noncommutative)
// list the buggy-opt exit codes they would produce — making any
// regression bisectable via the diff between the two arms.
// An arm declares EXACTLY ONE OF:
//   * `passes`: an inline PassId-name array (resolved via
//     optPassIdFromName) → an ad-hoc pipeline (maxIterations 1,
//     default inlineThreshold), OR
//   * `shippedPipeline`: the NAME of a shipped config
//     (src/dss-config/pipelines/<name>.pipeline.json) → the arm runs
//     the SHIPPED pipeline ITSELF (name/maxIterations/inlineThreshold
//     loaded from the file — ZERO drift between corpus + shipped config).
// `buildPipeline` enforces the exactly-one-of (both / neither → fail).
struct OptimizedArm {
    std::string                  label;   // diagnostic-rendering name (free-form)
    std::vector<std::string>     passes;  // PassId names (inline-array form)
    bool                         hasPasses = false;          // `passes` key present
    std::optional<std::string>   shippedPipeline;            // shipped-config form
    // D-TEST-A-RELEASE-ARM-BYTE-IDENTICAL-TO-BASELINE-ASSERTS-NOTHING: the
    // ESCALATION LEVER. False (the default) ⇒ a byte-identical optimized image
    // is REPORTED; true ⇒ it is a hard RED. Opt-in PER ARM rather than per
    // manifest because the two live side by side in one file: a
    // `{"passes":["ConstFold"]}` arm over a source with nothing to fold is
    // honestly a no-op, while the `{"shippedPipeline":"release"}` arm on the
    // same manifest may be the ONLY thing witnessing the optimizer x feature
    // composition. A manifest-level flag would have to red both or neither.
    // See the ★ DECISION note above `judgeOptimizedArtifactIdentity`.
    bool                         mustDifferFromBaseline = false;
};

// V2-4 Part C (D-DIAG-CLI-POSITION-RENDER-AND-ASSERT): one declared
// expected diagnostic for an EXPECT-ERROR example. `code` is the
// DiagnosticCode NAME (e.g. "S_UndeclaredIdentifier"); line/col are the
// 1-based start position the compiler's own `SourceBuffer::lineCol` must
// resolve the diagnostic's span to. This is the driver/e2e-tier twin of
// the analyzer-tier golden harness — it pins a SPECIFIC diagnostic at a
// SPECIFIC location through the full `Program::compileFiles` path.
struct ExpectedDiagnostic {
    std::string   code;
    std::uint32_t line = 0;
    std::uint32_t col  = 0;
    // D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN (#4): false ⇒ the diagnostic is emitted
    // at a tier with NO source span (e.g. `L_OverAlignedStackLocal` from the LIR
    // calling-convention frame layout). Such a diagnostic's default span resolves
    // to 1:1, but pinning a fabricated coordinate would be dishonest — instead the
    // set-equality below matches the CODE only for these (position-independent).
    // Default true = a real positioned diagnostic (every existing expect-error
    // example). Parsed from an optional `"positioned": false` manifest key.
    bool          positioned = true;
};

struct ExampleManifest {
    std::string                  language;
    std::string                  source;
    // D-EXAMPLES-RUNNER-PROJECT-MANIFEST: PROJECT MODE. Present ⇒ this example is
    // built by `Program::compileProject` from the named `.dss-project.json` (a
    // path relative to the example dir) instead of by `compileFiles`/
    // `compileUnits` over `source`/`sources`. MUTUALLY EXCLUSIVE with both of
    // those (readManifest fails loud on the ambiguity) because the two spellings
    // answer the same question — WHAT DOES THIS EXAMPLE COMPILE — and a manifest
    // that answers it twice has one answer silently ignored.
    //
    // ★ WHY THE KEY IS TOP-LEVEL AND THE PER-TARGET `spec` BECOMES A MIRROR.
    // `Program::compileProject` takes NO targets argument: the
    // `.dss-project.json`'s own `targets[]` is the authority, so a corpus
    // target's `spec` CANNOT drive a project build. Two shapes were available:
    //   (a) ONE project manifest per corpus target, named from the target entry.
    //       Each would carry a single-element `targets[]`, and the drive/mirror
    //       problem disappears — but an example would then ship N near-identical
    //       manifests that re-state the language, the sources and every build
    //       script N times. `.dss-project.json` is a USER-FACING artifact and a
    //       real project ships exactly ONE listing all its targets; N copies
    //       would make the example a demonstration of a harness workaround
    //       rather than of the feature, and would let the N copies drift.
    //   (b) ONE project manifest for the example, listing every target, with the
    //       corpus target entries keeping their existing job — declaring
    //       `runOn` / `emulator` / `artifact` / per-target overrides for the RUN.
    // (b) is what this implements. Its one cost is that `spec` no longer drives,
    // and that cost is paid off rather than tolerated: the runner CROSS-CHECKS
    // every corpus `spec` against the project manifest's own `targets[]` and
    // fails loud on a miss, so a stale mirror is caught AT the mirror instead of
    // surfacing three steps later as a baffling missing artifact.
    std::optional<std::string>   project;
    // Multi-CU (CU6): "sources":[...] makes each file its OWN CompilationUnit, and the
    // linker MERGES them into one image (Program::compileUnits). The single "source":"x.c"
    // form stays one CU5 multi-file unit (compileFiles). `sources` is the canonical file
    // list for either form; `multiCu` selects the driver entry. Cross-file references in a
    // multi-CU example resolve at LINK time (a sibling CU's definition or a library).
    std::vector<std::string>     sources;
    bool                         multiCu = false;
    std::int64_t                 exitCode = 0;
    // FF6 Slice 3 (2026-06-02): optional captured-stdout pin. When
    // present, the harness routes the child's STDOUT through an
    // anonymous pipe (`captureStdout=true`) and asserts the
    // drained bytes match this string exactly. When absent (the
    // pre-FF6 pattern), the child inherits the parent's stdio
    // and only the exit code is asserted — preserving every
    // existing example's behavior. Empty-string is a VALID pin
    // (asserts the binary printed nothing); the `has_value()`
    // gate distinguishes "no pin" from "pin to empty".
    std::optional<std::string>   expectedStdout;
    std::vector<ExampleTarget>   targets;
    // Optional differential-verify arms. Each compiles the same
    // source with the listed pipeline + asserts baseline-equal
    // exit code + stdout.
    std::vector<OptimizedArm>    optimizedPipelines;
    // V2-4 Part C: when NON-EMPTY this is an EXPECT-ERROR example — the
    // source is malformed, the compile MUST fail, and the produced
    // diagnostics MUST equal this declared set EXACTLY (code + 1-based
    // line:col). Mutually exclusive with the run path: no binary is
    // spawned and `exitCode` is not required.
    std::vector<ExpectedDiagnostic> expectDiagnostics;
};

// D-EXAMPLES-RUNNER-MULTI-ARTIFACT (c171) + nested extension: parse ONE
// `dependsOn` entry. RECURSIVE — an entry may itself carry a nested
// `dependsOn` (e.g. a fat `-staticlib` that resolves an input `-staticlib` to
// MERGE it). This SAME helper serves both the target-level parse AND the
// recursion, so a dependency can have its own dependency at any depth. Returns
// nullopt (ADD_FAILURE already fired) on any malformed field. A missing
// `dependsOn` key leaves the nested vector empty ⇒ the pre-nesting shape.
[[nodiscard]] std::optional<DependsOnArtifact>
parseDependsOnEntry(nlohmann::json const& d, fs::path const& manifestPath) {
    DependsOnArtifact dep;
    dep.spec     = d.value("spec", "");
    dep.artifact = d.value("artifact", "");
    dep.multiCu  = d.value("multiCu", false);
    if (d.contains("sources") && d.at("sources").is_array()) {
        for (auto const& s : d.at("sources")) {
            if (s.is_string()) dep.sources.push_back(s.get<std::string>());
        }
    }
    if (dep.sources.empty() || dep.spec.empty() || dep.artifact.empty()) {
        ADD_FAILURE() << "manifest " << manifestPath.generic_string()
                      << " dependsOn entry needs non-empty 'sources', 'spec',"
                         " and 'artifact'";
        return std::nullopt;
    }
    // Nested `dependsOn`: this entry's OWN prerequisites (built first, resolved
    // into this entry's build). The recursion is the ONLY new behavior — a
    // missing key leaves `dep.dependsOn` empty (the pre-nesting single-level
    // shape, every existing c171 example unchanged).
    if (d.contains("dependsOn")) {
        if (!d.at("dependsOn").is_array()) {
            ADD_FAILURE() << "manifest " << manifestPath.generic_string()
                          << " dependsOn entry 'dependsOn' must be an array";
            return std::nullopt;
        }
        for (auto const& nested : d.at("dependsOn")) {
            auto parsed = parseDependsOnEntry(nested, manifestPath);
            if (!parsed.has_value()) return std::nullopt;  // already reported
            dep.dependsOn.push_back(std::move(*parsed));
        }
    }
    return dep;
}

[[nodiscard]] ExampleManifest readManifest(fs::path const& path) {
    std::ifstream in(path);
    if (!in) {
        ADD_FAILURE() << "cannot open manifest " << path.generic_string();
        return {};
    }
    nlohmann::json j;
    try {
        in >> j;
    } catch (std::exception const& e) {
        ADD_FAILURE() << "manifest " << path.generic_string()
                      << " JSON parse failed: " << e.what();
        return {};
    }
    ExampleManifest m;
    m.language = j.value("language", "");
    m.source   = j.value("source", "");

    // ── WHAT DOES THIS EXAMPLE COMPILE? EXACTLY ONE SPELLING ANSWERS IT ─────
    //
    // Three keys can answer, and declaring two of them is a manifest DEFECT,
    // not a precedence question:
    //   * `project`  — a `.dss-project.json` drives the build (PROJECT MODE);
    //   * `sources`  — an array, one CompilationUnit per file (multi-CU, CU6);
    //   * `source`   — the single-file form.
    //
    // ★ `sources` USED TO "TAKE PRECEDENCE OVER" `source`, which is the polite
    // spelling of SILENTLY DROPPING ONE OF THEM. MEASURED over the corpus: ZERO
    // of the 580 manifests declare both, so the precedence rule protected
    // nothing and only stood ready to swallow a future typo — the author who
    // renames `source` to `sources` and forgets to delete the old key gets a
    // green suite compiling a file they no longer meant to compile. Both
    // runners now reject the pair.
    bool const hasSource  = j.contains("source");
    bool const hasSources = j.contains("sources");
    if (j.contains("project")) {
        if (!j.at("project").is_string()
            || j.at("project").get<std::string>().empty()) {
            ADD_FAILURE() << "manifest " << path.generic_string()
                          << " 'project' must be a NON-EMPTY string naming a"
                             " .dss-project.json relative to the example dir";
            return m;
        }
        if (hasSource || hasSources) {
            ADD_FAILURE() << "manifest " << path.generic_string()
                          << " declares BOTH 'project' and '"
                          << (hasSources ? "sources" : "source")
                          << "'. A project build's inputs come from the"
                             " .dss-project.json's own 'sources' (which a"
                             " preBuildScripts hook may GENERATE), so the"
                             " expected.json list would be silently ignored —"
                             " delete one.";
            return m;
        }
        m.project = j.at("project").get<std::string>();
    }
    if (hasSource && hasSources) {
        ADD_FAILURE() << "manifest " << path.generic_string()
                      << " declares BOTH 'source' and 'sources' — one names a"
                         " single CU, the other a multi-CU list, and honoring"
                         " either would silently discard the other. Delete one.";
        return m;
    }
    if (hasSources) {
        if (!j.at("sources").is_array() || j.at("sources").empty()) {
            ADD_FAILURE() << "manifest " << path.generic_string()
                          << " 'sources' must be a non-empty array of file names";
            return m;
        }
        for (auto const& s : j.at("sources")) {
            if (!s.is_string()) {
                ADD_FAILURE() << "manifest " << path.generic_string()
                              << " 'sources' entries must be strings";
                return m;
            }
            m.sources.push_back(s.get<std::string>());
        }
        m.multiCu = true;
    } else if (hasSource) {
        if (!j.at("source").is_string() || m.source.empty()) {
            ADD_FAILURE() << "manifest " << path.generic_string()
                          << " 'source' must be a non-empty string";
            return m;
        }
        m.sources = {m.source};  // single-CU: the one "source" file
    } else if (!m.project.has_value()) {
        // RELAXED, not deleted: a manifest that names NONE of the three still
        // fails exactly as loudly as it always did. Only the project-mode case
        // is exempt, and only because its inputs live in the other file.
        ADD_FAILURE() << "manifest " << path.generic_string()
                      << " requires 'source' (single CU), 'sources' (multi-CU),"
                         " or 'project' (a .dss-project.json build)";
        return m;
    }
    // V2-4 Part C: parse the expect-error diagnostics FIRST — their
    // presence makes this an error manifest, which relaxes the exitCode
    // requirement (no binary is produced or run).
    if (j.contains("expectDiagnostics")) {
        if (!j.at("expectDiagnostics").is_array() || j.at("expectDiagnostics").empty()) {
            ADD_FAILURE() << "manifest " << path.generic_string()
                          << " 'expectDiagnostics' must be a non-empty array";
            return m;
        }
        for (auto const& d : j.at("expectDiagnostics")) {
            if (!d.is_object()
                || !d.contains("code") || !d.at("code").is_string()
                || !d.contains("line") || !d.at("line").is_number_unsigned()
                || !d.contains("col")  || !d.at("col").is_number_unsigned()) {
                ADD_FAILURE() << "manifest " << path.generic_string()
                              << " each expectDiagnostics entry needs string 'code'"
                                 " + unsigned 'line' + unsigned 'col'";
                return m;
            }
            ExpectedDiagnostic ed;
            ed.code = d.at("code").get<std::string>();
            ed.line = d.at("line").get<std::uint32_t>();
            ed.col  = d.at("col").get<std::uint32_t>();
            // #4: optional — a span-less-tier diagnostic is matched by code only.
            if (d.contains("positioned")) {
                if (!d.at("positioned").is_boolean()) {
                    ADD_FAILURE() << "manifest " << path.generic_string()
                                  << " expectDiagnostics 'positioned' must be a boolean";
                    return m;
                }
                ed.positioned = d.at("positioned").get<bool>();
            }
            m.expectDiagnostics.push_back(std::move(ed));
        }
    }
    // PROJECT MODE has no expect-error branch, and saying so LOUDLY is the
    // point: `runErrorTarget` reads `m.sources.front()`, which a project
    // manifest never populates. Rejecting the combination in the PARSER means
    // the unsupported shape is named where the author can fix it, rather than
    // reaching an assertion on an empty vector.
    if (m.project.has_value() && !m.expectDiagnostics.empty()) {
        ADD_FAILURE() << "manifest " << path.generic_string()
                      << " declares BOTH 'project' and 'expectDiagnostics'."
                         " The expect-error path compiles a single named source"
                         " so a diagnostic's offset maps unambiguously; a"
                         " project build has no such source. Implement the"
                         " project-mode expect-error path in BOTH runners"
                         " before declaring this pair.";
        return m;
    }
    // exitCode: required for a run example; not for an expect-error one.
    if (j.contains("exitCode")) {
        if (!j.at("exitCode").is_number_integer()) {
            ADD_FAILURE() << "manifest " << path.generic_string()
                          << " 'exitCode' must be an integer";
            return m;
        }
        m.exitCode = j.at("exitCode").get<std::int64_t>();
    } else if (m.expectDiagnostics.empty()) {
        ADD_FAILURE() << "manifest " << path.generic_string()
                      << " missing integer 'exitCode' (required unless"
                         " 'expectDiagnostics' is present)";
        return m;
    }
    if (j.contains("expectedStdout")) {
        if (!j.at("expectedStdout").is_string()) {
            ADD_FAILURE() << "manifest " << path.generic_string()
                          << " 'expectedStdout' must be a string";
            return m;
        }
        m.expectedStdout = j.at("expectedStdout").get<std::string>();
    }
    if (!j.contains("targets") || !j.at("targets").is_array()
        || j.at("targets").empty()) {
        ADD_FAILURE() << "manifest " << path.generic_string()
                      << " requires non-empty 'targets' array";
        return m;
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
        // Model 3: optional per-target stdout override (a string; empty-string is
        // a VALID pin — asserts the binary printed nothing on this target).
        if (t.contains("expectedStdout")) {
            if (!t.at("expectedStdout").is_string()) {
                ADD_FAILURE() << "manifest " << path.generic_string()
                              << " target 'expectedStdout' must be a string";
                return m;
            }
            et.expectedStdoutOverride = t.at("expectedStdout").get<std::string>();
        }
        // C11/C23 6.4.5: optional per-target exit-code override (an integer) — a
        // source whose return value is platform-divergent (e.g. sizeof(wchar_t)).
        if (t.contains("exitCode")) {
            if (!t.at("exitCode").is_number_integer()) {
                ADD_FAILURE() << "manifest " << path.generic_string()
                              << " target 'exitCode' must be an integer";
                return m;
            }
            et.exitCodeOverride = t.at("exitCode").get<std::int64_t>();
        }
        // D-EXAMPLES-RUNNER-MULTI-ARTIFACT (c171): optional prerequisite
        // library artifacts this target links against (built FIRST, threaded
        // into `--resolve-library`). Each entry is parsed by the SHARED
        // recursive helper, so a dep may carry its OWN nested `dependsOn`
        // (a fat `-staticlib` merging an input `-staticlib`).
        if (t.contains("dependsOn")) {
            if (!t.at("dependsOn").is_array()) {
                ADD_FAILURE() << "manifest " << path.generic_string()
                              << " target 'dependsOn' must be an array";
                return m;
            }
            for (auto const& d : t.at("dependsOn")) {
                auto parsed = parseDependsOnEntry(d, path);
                if (!parsed.has_value()) return m;  // ADD_FAILURE already fired
                et.dependsOn.push_back(std::move(*parsed));
            }
        }
        m.targets.push_back(std::move(et));
    }
    // D-OPT1-DIFFERENTIAL-VERIFY-RUNNER. Manifest shape — each arm
    // declares EXACTLY ONE OF `passes` (inline array) or `shippedPipeline`
    // (a config name); the exactly-one-of is enforced in `buildPipeline`:
    //   "optimizedPipelines": [
    //     {"label": "constfold-only", "passes": ["ConstFold"]},
    //     {"label": "release", "shippedPipeline": "release"}
    //   ]
    if (j.contains("optimizedPipelines")) {
        if (!j.at("optimizedPipelines").is_array()) {
            ADD_FAILURE() << "manifest " << path.generic_string()
                          << " 'optimizedPipelines' must be an array";
            return m;
        }
        for (auto const& arm : j.at("optimizedPipelines")) {
            if (!arm.is_object()
                || !arm.contains("label") || !arm.at("label").is_string()) {
                ADD_FAILURE() << "manifest " << path.generic_string()
                              << " each optimizedPipelines entry needs string"
                                 " 'label' + exactly one of 'passes' /"
                                 " 'shippedPipeline'";
                return m;
            }
            OptimizedArm oa;
            oa.label = arm.at("label").get<std::string>();
            if (arm.contains("passes")) {
                if (!arm.at("passes").is_array()) {
                    ADD_FAILURE() << "manifest " << path.generic_string()
                                  << " optimizedPipelines 'passes' must be an"
                                     " array";
                    return m;
                }
                oa.hasPasses = true;
                for (auto const& p : arm.at("passes")) {
                    if (!p.is_string()) {
                        ADD_FAILURE() << "manifest " << path.generic_string()
                                      << " optimizedPipelines.passes entries"
                                         " must be strings";
                        return m;
                    }
                    oa.passes.push_back(p.get<std::string>());
                }
            }
            if (arm.contains("shippedPipeline")) {
                if (!arm.at("shippedPipeline").is_string()) {
                    ADD_FAILURE() << "manifest " << path.generic_string()
                                  << " optimizedPipelines 'shippedPipeline'"
                                     " must be a string";
                    return m;
                }
                oa.shippedPipeline = arm.at("shippedPipeline").get<std::string>();
            }
            // D-TEST-A-RELEASE-ARM-BYTE-IDENTICAL-TO-BASELINE-ASSERTS-NOTHING:
            // optional per-arm escalation. Absent ⇒ false ⇒ report-only.
            if (arm.contains("mustDifferFromBaseline")) {
                if (!arm.at("mustDifferFromBaseline").is_boolean()) {
                    ADD_FAILURE() << "manifest " << path.generic_string()
                                  << " optimizedPipelines"
                                     " 'mustDifferFromBaseline' must be a"
                                     " boolean";
                    return m;
                }
                oa.mustDifferFromBaseline =
                    arm.at("mustDifferFromBaseline").get<bool>();
            }
            m.optimizedPipelines.push_back(std::move(oa));
        }
    }
    return m;
}

// ── PROJECT MODE support ───────────────────────────────────────────────────

// The FORMAT half of a manifest spec ("x86_64:pe64-x86_64-windows-exec" →
// "pe64-x86_64-windows-exec"). Empty ⇒ the spec carries no ':' and the caller
// must fail loud rather than compose a wrong path.
//
// This is the SUBDIRECTORY a project build routes its artifact into:
// `Program::compileProject` forces `setPerFormatOutputSubdir(true)`
// (src/program/program.cpp:1760), so a project artifact lands at
// `<outDir>/<formatName>/<name><ext>` even for a single-target build, where a
// `--compile` build would have put it flat at `<outDir>/<name><ext>`.
//
// `specFormatName` is the shared `dss::test_support` helper beside its twin
// `specTargetArch` (arm_verdict_ledger.hpp) — reached via the `using namespace`
// above. It was briefly duplicated here and in `integrated_tests/runner.cpp`
// when project mode landed; both copies were folded into the shared header so
// the two runners cannot disagree about how a spec splits.

// The FACTS this harness needs out of a `.dss-project.json`.
//
// ★ DELIBERATELY NOT A SECOND `parseProjectConfig`. The DRIVER owns that parse
// and fails loud on every structural error in it; a harness that re-validated
// the file would grow a second opinion about a shape it does not own, and the
// two would drift. Three questions only, each with a harness-side reason:
//
//   * `language` + `targets[]` — what the expected.json entries MIRROR.
//     `Program::compileProject` takes NO targets argument, so a corpus target's
//     `spec` cannot DRIVE a project build; cross-checking it here is what keeps
//     the mirror honest. Without the check, a spec that names a target the
//     project never builds surfaces three steps later as "artifact missing",
//     which reads like a codegen defect.
//   * argv[0] of every APPLICABLE build script — so an absent interpreter is
//     `SkippedBuildInputMissing` (named, ledgered, and a hard failure under
//     DSS_STRICT_ARM_VERDICTS=1) rather than a `D_FileNotFound` on a source
//     glob that matched nothing, which also reads like a compiler defect.
//
// Anything malformed here fires ADD_FAILURE and returns nullopt: the caller
// poisons the arm. A project manifest this harness cannot read is a corpus
// defect, never a skip.
struct ProjectFacts {
    std::string              language;
    std::vector<std::string> targets;
    // argv[0] of each pre/post-build script whose `runOn` admits THIS host.
    std::vector<std::string> applicableScriptPrograms;
};

[[nodiscard]] std::optional<ProjectFacts>
readProjectFacts(fs::path const& projectPath) {
    std::ifstream in(projectPath);
    if (!in) {
        ADD_FAILURE() << "cannot open project manifest "
                      << projectPath.generic_string();
        return std::nullopt;
    }
    nlohmann::json j;
    try {
        in >> j;
    } catch (std::exception const& e) {
        ADD_FAILURE() << "project manifest " << projectPath.generic_string()
                      << " JSON parse failed: " << e.what();
        return std::nullopt;
    }
    ProjectFacts f;
    if (!j.contains("language") || !j.at("language").is_string()) {
        ADD_FAILURE() << "project manifest " << projectPath.generic_string()
                      << " needs a string 'language'";
        return std::nullopt;
    }
    f.language = j.at("language").get<std::string>();
    if (!j.contains("targets") || !j.at("targets").is_array()
        || j.at("targets").empty()) {
        ADD_FAILURE() << "project manifest " << projectPath.generic_string()
                      << " needs a non-empty 'targets' array";
        return std::nullopt;
    }
    for (auto const& t : j.at("targets")) {
        if (!t.is_string()) {
            ADD_FAILURE() << "project manifest " << projectPath.generic_string()
                          << " 'targets' entries must be strings";
            return std::nullopt;
        }
        f.targets.push_back(t.get<std::string>());
    }
    // BOTH hook arrays: a post-build script's interpreter is as absent as a
    // pre-build one's, and a build that runs the compile and then fails to run
    // its post step is the same environmental fact.
    auto const host = currentHostOs();
    for (char const* key : {"preBuildScripts", "postBuildScripts"}) {
        if (!j.contains(key)) continue;
        if (!j.at(key).is_array()) {
            ADD_FAILURE() << "project manifest " << projectPath.generic_string()
                          << " '" << key << "' must be an array";
            return std::nullopt;
        }
        for (auto const& e : j.at(key)) {
            if (!e.is_object() || !e.contains("run") || !e.at("run").is_array()
                || e.at("run").empty() || !e.at("run").at(0).is_string()) {
                ADD_FAILURE() << "project manifest "
                              << projectPath.generic_string() << " '" << key
                              << "' entries need a non-empty string array 'run'";
                return std::nullopt;
            }
            // An ABSENT `runOn` means EVERY host (the driver's own default —
            // src/program/build_scripts.cpp `appliesToHost`). Reading absent as
            // "matches nothing" would invert the default and silently exempt
            // every unfiltered hook from this probe.
            bool applies = true;
            if (e.contains("runOn")) {
                if (!e.at("runOn").is_array()) {
                    ADD_FAILURE() << "project manifest "
                                  << projectPath.generic_string() << " '" << key
                                  << "' entry 'runOn' must be an array";
                    return std::nullopt;
                }
                applies = false;
                for (auto const& o : e.at("runOn")) {
                    if (o.is_string() && o.get<std::string>() == host) {
                        applies = true;
                    }
                }
            }
            if (applies) {
                f.applicableScriptPrograms.push_back(
                    e.at("run").at(0).get<std::string>());
            }
        }
    }
    return f;
}

// Is a build script's argv[0] present on THIS machine?
//
// Mirrors the contract of the driver's own
// `dss::substrate::resolveExecutableOnPath` closely enough for a PRESENCE
// probe: a name carrying a directory separator is taken AS A PATH (no search),
// anything else is a PATH lookup through the SAME `findOnPath` the emulator
// gate uses — so the two skip classes cannot disagree about what "missing"
// means. This is a probe, not a re-implementation: the driver still spawns, and
// still emits `D_ScriptSpawnFailed` if it is wrong.
[[nodiscard]] bool buildScriptProgramPresent(std::string const& argv0) {
    if (argv0.find('/') != std::string::npos
        || argv0.find('\\') != std::string::npos) {
        std::error_code ec;
        bool const there = fs::exists(fs::path{argv0}, ec);
        return there && !ec;
    }
    return !findOnPath(argv0).empty();
}

// Resolve a list of pass-name strings into an OptPipeline. Fails the
// test loud (ADD_FAILURE) if any name is unrecognised — surfaces typos
// in manifests at runtime + catches drift between the JSON vocab and
// the PassId enum.
[[nodiscard]] std::optional<::dss::opt::OptPipeline>
buildPipeline(OptimizedArm const& arm, fs::path const& manifestPath) {
    // EXACTLY-ONE-OF `passes` / `shippedPipeline` — fail loud on both or
    // neither (a manifest typo must surface, never silently pick a
    // default). `hasPasses` (not `passes`-non-empty) is the presence
    // signal: an empty inline `passes: []` is still "the inline form".
    bool const hasPasses  = arm.hasPasses;
    bool const hasShipped = arm.shippedPipeline.has_value();
    if (hasPasses == hasShipped) {
        ADD_FAILURE() << "manifest " << manifestPath.generic_string()
                      << ": optimizedPipelines arm '" << arm.label
                      << "' must declare EXACTLY ONE OF 'passes' or"
                         " 'shippedPipeline' (got "
                      << (hasPasses ? "both" : "neither") << ")";
        return std::nullopt;
    }

    // Shipped-config form: load the pipeline ITSELF (name/maxIterations/
    // inlineThreshold from the file — ZERO drift between the corpus arm
    // and the shipped configuration it claims to exercise).
    if (hasShipped) {
        auto loaded = ::dss::opt::loadShippedPipeline(*arm.shippedPipeline);
        if (!loaded.has_value()) {
            std::ostringstream diag;
            for (auto const& d : loaded.error()) diag << "\n    " << d.message;
            ADD_FAILURE() << "manifest " << manifestPath.generic_string()
                          << ": arm '" << arm.label
                          << "' shippedPipeline '" << *arm.shippedPipeline
                          << "' failed to load:" << diag.str();
            return std::nullopt;
        }
        return std::move(*loaded);
    }

    // Inline-array form: resolve each PassId name.
    ::dss::opt::OptPipeline p;
    p.name = arm.label;
    p.passes.reserve(arm.passes.size());
    for (auto const& name : arm.passes) {
        auto resolved = ::dss::opt::optPassIdFromName(name);
        if (!resolved.has_value()) {
            ADD_FAILURE() << "manifest " << manifestPath.generic_string()
                          << ": unknown PassId '" << name << "' in arm '"
                          << arm.label << "'";
            return std::nullopt;
        }
        p.passes.push_back(*resolved);
    }
    return p;
}

// Per-arm compile+spawn outcome. The states distinguish every reason the
// caller might receive no exit-code/stdout pair to compare. Conflating them
// (the original `skipped: bool` shape) would let a baseline that secretly
// failed to compile produce a silently-bypassed differential-verify assertion.
//
// D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT: the single `SkippedCrossHost` that
// used to cover all three skip reasons is GONE. It was the defect: a
// `runOn`-excluded arm (structural, expected), an arm whose manifest declares
// no emulator (a manifest defect) and an arm whose declared emulator is absent
// from THIS machine (environmental, and the one strict mode reds) are three
// different facts, and one name for them made all three unreportable. The
// verdict vocabulary now lives in the shared `ArmVerdict`.
struct ArmResult {
    ArmVerdict  verdict = ArmVerdict::Poisoned;
    int         exitCode = 0;
    std::string capturedStdout;
    // WHY, in the manifest's own vocabulary — carried into the ledger so the
    // summary can name the runOn list / the missing emulator rather than
    // reprint a bare status.
    std::string detail;
    // D-TEST-A-RELEASE-ARM-BYTE-IDENTICAL-TO-BASELINE-ASSERTS-NOTHING: the
    // WHOLE produced artifact, read off disk before this arm returns.
    //
    // ⚠ CAPTURED HERE, NOT COMPARED-BY-PATH LATER, and that is forced rather
    // than chosen: each arm builds inside its OWN `ScratchDir`, which is an
    // RAII sandbox destroyed when `compileAndRunArm` returns. By the time
    // `runOneTarget` holds two ArmResults, neither artifact exists on disk any
    // more. Bytes rather than a hash: a hash collision's failure direction is a
    // SILENT MISS — exactly the class of defect this exists to end — and corpus
    // artifacts are single-digit KB, so the exact comparison costs nothing.
    std::string artifactBytes;
};

// ── D-TEST-A-RELEASE-ARM-BYTE-IDENTICAL-TO-BASELINE-ASSERTS-NOTHING ─────────
//
// THE DEFECT. An `optimizedPipelines` arm compiles the example a SECOND time
// under a named pipeline and asserts the binary still produces the baseline's
// exit code and stdout. ✔MEASURED (AP6, 2026-08-14): the ArtifactLink corpus
// example read `return dss_fold_twice(argc * 20);`, and release vs debug
// produced a BYTE-IDENTICAL pe64 image (`18EF3364…BFAE08FA` both ways) — the
// only call was external, so the optimizer had nothing to transform and the arm
// asserted that a no-op stayed a no-op. ★ The arm LOOKED correct the whole
// time: it was declared, it ran, it passed, and it witnessed nothing. Nothing
// in either harness detected that, so every `optimizedPipelines` arm in the
// corpus was trusted ON DECLARATION ALONE.
//
// WHAT IS COMPARED, AND WHY IT IS AGNOSTIC. Two artifacts' BYTES — not a
// section table, not an instruction count, not a symbol, not an entry point.
// Nothing below knows what a language, a processor or an object format is, and
// nothing below may learn: the corpus spans three formats and several arches,
// and any structural opinion would be a fourth place for target identity to
// leak into a harness that has none.
//
// WHAT THE TWO OUTCOMES ARE WORTH, stated precisely because the asymmetry is
// the whole reason this is a report and not an assert:
//   * IDENTICAL bytes are CONCLUSIVE — the pipeline transformed nothing, so the
//     arm's exit-code/stdout comparison is `x == x` and cannot fail.
//   * DIFFERING bytes are NECESSARY BUT NOT SUFFICIENT — they prove the
//     pipeline changed the image, not that it changed anything a reader would
//     call an optimization. This check therefore RAISES the floor from "an arm
//     was declared" to "an arm changed the output"; it does not certify that a
//     specific transform fired, and it must never be cited as if it did.
//
// DETERMINISM IS A PRECONDITION, AND IT IS OBSERVED RATHER THAN ASSUMED. Two
// compiles of one source in two different scratch directories must produce
// equal bytes when the pipeline changes nothing — i.e. no embedded timestamp,
// no embedded build path, no address-space randomness in the emitters. The AP6
// measurement above IS that observation: the pe64 images matched exactly across
// two independent builds. If a future emitter breaks it, the failure direction
// is a FALSE QUIET (every arm looks like it differed), never a false red.

// The exact bytes of a produced artifact. `nullopt` ⇒ the file could not be
// read; the caller POISONS the arm rather than continuing, because an arm whose
// artifact cannot be read has no identity to compare and must never be allowed
// to reach the rule below as an empty string.
[[nodiscard]] std::optional<std::string> readArtifactBytes(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::string bytes{std::istreambuf_iterator<char>(in),
                      std::istreambuf_iterator<char>{}};
    if (!in.good() && !in.eof()) return std::nullopt;
    return bytes;
}

// A short, greppable image fingerprint for the REPORT LINE ONLY. FNV-1a/64.
//
// ⚠ The judgement below compares BYTES, never this. A digest exists so a
// reader can eyeball two ledger lines and so an operator can grep a CI log for
// one image — and if it ever became the comparison, a collision would silently
// re-open the exact defect this file is closing.
[[nodiscard]] std::string artifactFingerprint(std::string_view bytes) {
    std::uint64_t h = 14695981039346656037ull;
    for (char const ch : bytes) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        h *= 1099511628211ull;
    }
    std::string out(16, '0');
    for (std::size_t i = 16; i-- > 0;) {
        out[i] = "0123456789ABCDEF"[static_cast<std::size_t>(h & 0xFu)];
        h >>= 4;
    }
    return out;
}

enum class ArtifactIdentity {
    Differed,        // the two arms' images differ — the pipeline did something
    Identical,       // byte-identical — this arm cannot assert anything
    CaptureMissing,  // one side produced no bytes at all: a HARNESS defect
};

struct ArtifactIdentityJudgement {
    ArtifactIdentity outcome = ArtifactIdentity::CaptureMissing;
    // Does this outcome RED the entry? See the ★ DECISION note below.
    bool             red = false;
    // Rendered, greppable, and EMPTY when there is nothing to say — so the
    // caller's summary does not have to know the rule's vocabulary.
    std::string      note;
};

// ★ THE DECISION: REPORT BY DEFAULT, HARD RED WHEN THE ARM OPTS IN.
//
// ⚠ The registry row leaves this open on purpose ("some features legitimately
// produce identical bytes, so decide report-vs-red deliberately"), so the
// reasoning is recorded here rather than in a commit message.
//
// ✔MEASURED on this corpus before choosing (2026-08-15, Windows leg, every one
// of the 474 manifests that declares `optimizedPipelines` swept through this
// very check): 559 optimized arms ran, 559 were compared, and **77 produced a
// BYTE-IDENTICAL image** — spread over 70 distinct examples. By arm label:
// 36 `release`, 31 `full-release-like`, 6 `constfold-only`, 3 `full`,
// 1 `dce-only`. A blanket red is therefore not a strictness setting, it is a
// 77-way corpus break, and this lane could not have fixed one of them honestly:
// making an arm differ means EDITING THE EXAMPLE'S SOURCE until the optimizer
// has something to chew on, which is a per-example authoring judgement (AP6 did
// exactly that, by hand, for ONE example). The realistic response to 77 reds is
// to DELETE the offending arms, which lowers coverage — the precise opposite of
// what this row wants.
//
// Three further reasons the default is a report:
//
//   1. IDENTITY IS HOST- AND TARGET-DEPENDENT, so a red would make GREEN depend
//      on which leg you are standing on. Which arms run at all is gated by
//      `runOn` — the 559 arms above are the pe64 ones this host can spawn, and
//      an elf64 or macho64 leg compares a DIFFERENT set. An allowlist of
//      known-identical arms would have to be measured per leg and would red on
//      every leg it was not measured on.
//   2. THE HONEST CASES ARE REAL, and the sweep found them rather than
//      predicting them: 7 of the 77 are `asm/` examples, whose source IS
//      assembly. A pipeline has nothing to transform there BY CONSTRUCTION, and
//      the arm is still worth running — it pins that the pipeline does not
//      BREAK the example. Reddening those punishes an honest manifest for the
//      shape of its subject, and no edit to the example could ever clear it.
//   3. AN ARM THAT KNOWS IT MUST WITNESS CAN SAY SO. The failure the row
//      describes is specifically an arm whose JOB is the optimizer x feature
//      composition. That is a property only the manifest knows, so the manifest
//      is where it is declared: `"mustDifferFromBaseline": true` turns the
//      report into a hard red for that arm alone.
//
// FAILURE DIRECTION IF THIS CHOICE IS WRONG — a FALSE GREEN. An arm added
// tomorrow that witnesses nothing still passes, carrying a printed note that a
// reader can miss (ctest hides a passing test's stdout). That is the SAME CLASS
// as today's defect, and the reason it is nonetheless the right trade is that
// the fact stops being INVISIBLE: it is measured on every run, printed with a
// stable `[artifact-identity]` tag, counted per entry, and one manifest key
// away from a red. The opposite mistake — blanket red — fails as 300 FALSE REDS
// plus standing pressure to weaken manifests, and a corpus that has been
// weakened to get green cannot be un-weakened by deleting a check.
//
// ⚠ THE LEVER SHIPS UNARMED. No corpus manifest sets `mustDifferFromBaseline`
// yet, because `examples/**` is outside this lane. The examples whose arms
// exist to witness the optimizer (the ArtifactLink example of the row above
// first among them) should opt in; that is corpus work, reported upward, not
// done here.
//
// `requireDiffers` is the arm's opt-in. `CaptureMissing` ALWAYS reds regardless
// of it: two empty strings compare equal, so a broken capture would otherwise
// masquerade as the strongest possible finding.
[[nodiscard]] ArtifactIdentityJudgement
judgeOptimizedArtifactIdentity(std::string const& baselineBytes,
                               std::string const& optimizedBytes,
                               bool               requireDiffers) {
    ArtifactIdentityJudgement j;
    if (baselineBytes.empty() || optimizedBytes.empty()) {
        j.outcome = ArtifactIdentity::CaptureMissing;
        j.red     = true;
        j.note    = "artifact bytes were not captured (baseline="
                  + std::to_string(baselineBytes.size()) + " bytes, optimized="
                  + std::to_string(optimizedBytes.size())
                  + " bytes) — the identity check cannot run, and an"
                    " uncomparable arm must never be read as a compared one";
        return j;
    }
    if (baselineBytes != optimizedBytes) {
        j.outcome = ArtifactIdentity::Differed;
        j.red     = false;
        return j;  // nothing to say: the arm changed the image
    }
    j.outcome = ArtifactIdentity::Identical;
    j.red     = requireDiffers;
    j.note    = "optimized image is BYTE-IDENTICAL to the baseline ("
              + std::to_string(baselineBytes.size()) + " bytes, fnv1a="
              + artifactFingerprint(baselineBytes)
              + ") — the pipeline transformed nothing, so this arm's"
                " exit-code/stdout comparison is x==x and cannot fail";
    if (requireDiffers) {
        j.note += ". The arm declares \"mustDifferFromBaseline\": true, so this"
                  " is a FAILURE: either the source has nothing for the pipeline"
                  " to transform (give it one — an inlinable helper, an"
                  " accumulator, a loop) or the pipeline regressed";
    }
    return j;
}

// Per-ENTRY accounting for the check above. Threaded like `ArmVerdictLedger`
// so the summary at the end of the entry can print one line whatever happened.
//
// ★ `armsThatRan` and `compared` are incremented at DIFFERENT SITES on purpose,
// and the assertion that they are equal is what keeps this check from becoming
// the very thing it polices. A check whose only output is a printed note goes
// silent — and therefore green — if its call site is deleted or if capture
// starts returning nothing. Counting the arms that reached `Ran` separately
// from the comparisons actually performed makes that silence LOUD.
struct ArtifactIdentityTally {
    std::size_t              armsThatRan = 0;  // optimized arms that produced a run
    std::size_t              compared    = 0;  // identity judgements performed
    std::size_t              identical   = 0;  // ... of which byte-identical
    std::vector<std::string> notes;            // one per non-silent judgement
};

// D-EXAMPLES-RUNNER-MULTI-ARTIFACT (c171) + nested extension: build ONE
// prerequisite LIBRARY artifact into `outDir`, RECURSIVELY building its own
// nested `dependsOn` FIRST (into the same `outDir`) and threading their output
// paths into THIS dep's `--resolve-library`. So a `-staticlib` dep listing a
// nested `-staticlib` dep MERGES it (the fat archive, D-FF1-STATICLIB-FAT-
// ARCHIVE): the nested input `.a` is built, then the fat `.a` build resolves it
// and bundles its members in. ORDER-CORRECT (a dep's prerequisites exist on
// disk before its own build runs) and fully generic (arbitrary depth, no
// example-name special-casing). `dep.sources` resolve against `scratchPath`
// (the example's mirrored file neighborhood). Returns the built artifact's
// path, or nullopt (ADD_FAILURE already fired) on any source-missing / compile
// / artifact-missing failure — the caller poisons the arm. A dep with NO nested
// `dependsOn` never calls setResolveLibraries ⇒ byte-identical to the
// pre-nesting single-level build (every existing c171 example unchanged).
[[nodiscard]] std::optional<fs::path>
buildDependencyArtifact(DependsOnArtifact const& dep,
                        fs::path const&          scratchPath,
                        fs::path const&          outDir,
                        fs::path const&          exampleDir,
                        std::string const&       language) {
    // Nested prerequisites FIRST (order-correct): each nested artifact must
    // exist on disk before this dep's own build resolves against it.
    std::vector<fs::path> nestedLibs;
    nestedLibs.reserve(dep.dependsOn.size());
    for (auto const& nested : dep.dependsOn) {
        auto nestedPath = buildDependencyArtifact(nested, scratchPath, outDir,
                                                  exampleDir, language);
        if (!nestedPath.has_value()) return std::nullopt;  // already reported
        nestedLibs.push_back(std::move(*nestedPath));
    }

    // This dep's own sources (mirrored into the scratch dir alongside the main
    // sources at the top of compileAndRunArm).
    std::vector<std::string> depSrcPaths;
    depSrcPaths.reserve(dep.sources.size());
    for (auto const& s : dep.sources) {
        auto const sp = scratchPath / s;
        if (!fs::exists(sp)) {
            ADD_FAILURE() << "dependsOn source '" << s
                          << "' not found in example dir "
                          << exampleDir.generic_string();
            return std::nullopt;
        }
        depSrcPaths.push_back(sp.generic_string());
    }

    Program            depProg;
    DiagnosticReporter depRep;
    depProg.setOutputDir(outDir);
    // Thread resolve-libraries ONLY when this dep has nested prerequisites — an
    // empty set skips the call, keeping every single-level example's build
    // byte-identical to the pre-nesting path.
    if (!nestedLibs.empty()) {
        depProg.setResolveLibraries(nestedLibs);
    }
    int const depRc = dep.multiCu
        ? depProg.compileUnits(depSrcPaths, language, {dep.spec}, depRep)
        : depProg.compileFiles(depSrcPaths, language, {dep.spec}, depRep);
    if (depRc != 0 || depRep.errorCount() != 0u) {
        std::ostringstream depDump;
        for (auto const& d : depRep.all()) {
            depDump << "\n  " << diagnosticCodeName(d.code)
                    << " (severity=" << static_cast<int>(d.severity)
                    << "): " << d.actual;
        }
        ADD_FAILURE() << "dependsOn library build failed for spec=" << dep.spec
                      << " (artifact=" << dep.artifact
                      << ", example=" << exampleDir.generic_string() << ")"
                      << depDump.str();
        return std::nullopt;
    }
    auto const depArtifact = outDir / dep.artifact;
    if (!fs::exists(depArtifact)) {
        ADD_FAILURE() << "dependsOn artifact missing at "
                      << depArtifact.generic_string() << " (spec=" << dep.spec
                      << ")";
        return std::nullopt;
    }
    return depArtifact;
}

// The corpus neighbour-staging primitive now lives ONCE, in
// `tests/test_support/stage_tree.hpp`, and this runner and its CLI-subprocess
// sibling both include it (see the ★★ hoist note at the top of that header).
// It used to sit here duplicated VERBATIM — ✔MEASURED 14,765 bytes between the
// twin markers, `cmp`-identical against the sibling's copy — and was
// held together by a run-time byte-compare lint that the hoist deleted along
// with the duplication it guarded. What guards the shared home now is
// `ExamplesCorpusLint.StagingPrimitiveLivesOnlyInTheSharedHeader` below, which
// reds if a copy comes BACK into either runner.

[[nodiscard]] ArmResult
compileAndRunArm(fs::path const& exampleDir,
                 ExampleManifest const& m,
                 ExampleTarget const& t,
                 ::dss::opt::OptPipeline const* pipelineOverride,
                 char const* armLabel) {
    SCOPED_TRACE(std::string{"arm="} + armLabel);
    ArmResult armResult;
    ScratchDir scratch{Location::InsideRepo, "examples"};
    // Mirror the EXAMPLE DIR's file neighborhood into the scratch dir
    // (every file except the top-level manifest) — not just the declared
    // sources. The CLI-subprocess runner (integrated_tests) compiles IN
    // the example dir, where a quote include resolves against the
    // includer's directory; the in-process scratch must offer the same
    // neighbor files or an include-bearing example (e.g.
    // include_typedef_cast's myint.h) false-fails here only. The entry
    // files passed to the driver stay exactly `m.sources`.
    //
    // CONTRACT, and it is now the RECURSIVE one: the mirror carries whole
    // SUBDIRECTORIES across, relative subpaths intact. It used to be the
    // immediate dir only, with a `continue` on every non-regular entry — so
    // an example whose dependency lives in `<example>/dep_module/` had that
    // directory dropped in silence and then died on a missing-file error
    // naming the manifest. The walk is `stageExampleTree` from the shared
    // `tests/test_support/stage_tree.hpp` — the SAME function the CLI sibling
    // calls, not a copy held equal to it — so the two runners cannot stage
    // different trees.
    if (std::string const err = stageExampleTree(exampleDir, scratch.path());
        !err.empty()) {
        // A staging failure POISONS the arm rather than falling through to a
        // compile: the sources are simply not on disk, so every diagnostic the
        // driver would then emit describes the harness, not the example.
        ADD_FAILURE() << "staging the neighbor files of "
                      << exampleDir.generic_string() << " into "
                      << scratch.path().generic_string() << " failed: " << err;
        armResult.verdict = ArmVerdict::Poisoned;
        return armResult;
    }
    // Every DECLARED source must have been among the copied files — a
    // manifest typo fails loud here, not as a confusing driver error.
    std::vector<std::string> srcPaths;
    srcPaths.reserve(m.sources.size());
    for (auto const& s : m.sources) {
        auto const sp = scratch.path() / s;
        if (!fs::exists(sp)) {
            ADD_FAILURE() << "manifest source '" << s
                          << "' not found in example dir "
                          << exampleDir.generic_string();
            armResult.verdict = ArmVerdict::Poisoned;
            return armResult;
        }
        srcPaths.push_back(sp.generic_string());
    }
    scratch.useAsCwd();
    auto const outDir = scratch.path() / "out";

    // ── PROJECT MODE: resolve + validate the `.dss-project.json` ────────────
    //
    // The scratch dir is ALREADY the process cwd (the `useAsCwd()` above), and
    // that is load-bearing for a project build rather than incidental: the
    // manifest's `sources[]` globs expand against the PROCESS working directory
    // (Program::compileProject, D-AP2-SOURCES-GLOB), and a `preBuildScripts`
    // hook that GENERATES a source writes it into the directory the driver
    // spawns it in. Both must be this scratch dir, or the generator writes into
    // one tree and the glob searches another. The CLI-subprocess sibling had to
    // GROW this (its CwdGuard wrapped only the RUN) — see the note there.
    fs::path projectPath;
    if (m.project.has_value()) {
        projectPath = scratch.path() / *m.project;
        if (!fs::exists(projectPath)) {
            ADD_FAILURE() << "manifest 'project' file '" << *m.project
                          << "' not found in example dir "
                          << exampleDir.generic_string()
                          << " — the mirror above is RECURSIVE and preserves"
                             " relative subpaths, so a project manifest in a"
                             " SUBDIRECTORY does reach the scratch tree; an"
                             " absent one now means the path is wrong in"
                             " expected.json, not that the staging dropped it";
            armResult.verdict = ArmVerdict::Poisoned;
            return armResult;
        }
        auto const facts = readProjectFacts(projectPath);
        if (!facts.has_value()) {  // ADD_FAILURE already fired
            armResult.verdict = ArmVerdict::Poisoned;
            return armResult;
        }
        // THE MIRROR CHECK. `compileProject` takes no targets argument, so this
        // `spec` does not drive the build — it declares which of the project's
        // OWN targets this arm runs, gates `runOn`/`emulator`, and names the
        // artifact subdirectory. A spec the project never builds must fail HERE.
        if (std::find(facts->targets.begin(), facts->targets.end(), t.spec)
            == facts->targets.end()) {
            std::ostringstream declared;
            for (auto const& s : facts->targets) declared << "\n    " << s;
            ADD_FAILURE() << "target spec '" << t.spec
                          << "' is not declared by the project manifest '"
                          << *m.project << "'. A project build's targets come"
                             " from that file, so this arm would assert an"
                             " artifact nothing was asked to produce."
                             " The project declares:" << declared.str();
            armResult.verdict = ArmVerdict::Poisoned;
            return armResult;
        }
        if (!m.language.empty() && m.language != facts->language) {
            ADD_FAILURE() << "expected.json declares language '" << m.language
                          << "' but the project manifest '" << *m.project
                          << "' declares '" << facts->language
                          << "'. In project mode the project manifest is the"
                             " authority; the expected.json copy is a mirror and"
                             " must agree with it.";
            armResult.verdict = ArmVerdict::Poisoned;
            return armResult;
        }
        if (specFormatName(t.spec).empty()) {
            ADD_FAILURE() << "target spec '" << t.spec
                          << "' has no '<arch>:<format>' separator, so the"
                             " per-format artifact subdirectory a project build"
                             " routes into cannot be derived";
            armResult.verdict = ArmVerdict::Poisoned;
            return armResult;
        }
        // ENVIRONMENTAL, and the ONLY skip this mode may produce. A declared
        // build script whose interpreter this machine does not have is a BUILD
        // INPUT the manifest asked for and the machine could not supply —
        // exactly `SkippedBuildInputMissing`. Probed BEFORE the compile so the
        // ledger says "this box has no <interpreter>" instead of letting the
        // build fail with a source glob that matched nothing, which names the
        // compiler for the machine's shortfall.
        for (auto const& argv0 : facts->applicableScriptPrograms) {
            if (buildScriptProgramPresent(argv0)) continue;
            std::ostringstream why;
            why << "build script program '" << argv0 << "' declared by '"
                << *m.project << "' is not present on this machine (host="
                << currentHostOs() << ")";
            GTEST_LOG_(INFO) << "spec=" << t.spec << " arm=" << armLabel
                             << ' ' << why.str() << " — skipping build";
            armResult.verdict = ArmVerdict::SkippedBuildInputMissing;
            armResult.detail  = why.str();
            return armResult;
        }
    }

    // D-EXAMPLES-RUNNER-MULTI-ARTIFACT (c171) + nested extension: build each
    // prerequisite LIBRARY artifact FIRST (into the same out dir), recursively
    // building any nested `dependsOn` before it, then thread their paths into
    // the dependent build's `--resolve-library`. A dep (or nested-dep) build
    // failure poisons the arm with the SAME strict compile-side checks as the
    // main build (buildDependencyArtifact fires ADD_FAILURE + returns nullopt).
    std::vector<fs::path> resolveLibs;
    resolveLibs.reserve(t.dependsOn.size());
    for (auto const& dep : t.dependsOn) {
        auto depArtifact = buildDependencyArtifact(dep, scratch.path(), outDir,
                                                   exampleDir, m.language);
        if (!depArtifact.has_value()) {
            armResult.verdict = ArmVerdict::Poisoned;
            return armResult;
        }
        resolveLibs.push_back(std::move(*depArtifact));
    }

    Program            prog;
    DiagnosticReporter rep;
    prog.setOutputDir(outDir);
    if (!resolveLibs.empty()) {
        prog.setResolveLibraries(resolveLibs);
    }
    if (pipelineOverride != nullptr) {
        prog.setOptimizerPipelineOverride(*pipelineOverride);
    }
    // PROJECT MODE → compileProject (the manifest supplies language, targets,
    // sources, flags AND the build-lifecycle hooks — this is the ONLY driver
    // entry point that runs them). Otherwise: multi-CU example → compileUnits
    // (one CU per file, merged at link); single → compileFiles.
    //
    // ⚠ `{t.spec}` is deliberately NOT passed in the project branch, and there
    // is nowhere to pass it: `compileProject` has no targets parameter. The arm
    // therefore builds EVERY target the project declares and asserts only the
    // one this arm owns — which is why the mirror check above is mandatory. A
    // pipeline override still applies (it is Program state, read by the
    // delegated compileFiles/compileUnits), so an `optimizedPipelines` arm
    // remains meaningful in project mode.
    int const rc = m.project.has_value()
        ? prog.compileProject(projectPath.generic_string(), rep)
        : (m.multiCu
               ? prog.compileUnits(srcPaths, m.language, {t.spec}, rep)
               : prog.compileFiles(srcPaths, m.language, {t.spec}, rep));

    // Strict compile-side checks. EXPECT_ (not ASSERT_) because the
    // helper is non-void and must hand control back to the caller's
    // arm-comparison logic via a poisoned ArmResult on failure.
    std::ostringstream diagDump;
    for (auto const& d : rep.all()) {
        diagDump << "\n  " << diagnosticCodeName(d.code)
                 << " (severity=" << static_cast<int>(d.severity)
                 << "): " << d.actual;
    }
    EXPECT_EQ(rc, 0)
        << "compile failed for spec=" << t.spec
        << " arm=" << armLabel
        << " example=" << exampleDir.generic_string()
        << " diagnostics:" << diagDump.str();
    EXPECT_EQ(rep.errorCount(), 0u)
        << "expected zero error-severity diagnostics for spec="
        << t.spec << " arm=" << armLabel << diagDump.str();
    if (rc != 0 || rep.errorCount() != 0u) {
        armResult.verdict = ArmVerdict::Poisoned;
        return armResult;
    }

    // D-AP2-OUTPUT-ROUTING: a PROJECT build forces `setPerFormatOutputSubdir(true)`
    // (src/program/program.cpp:1760), so its artifact is at
    // `<outDir>/<formatName>/<name><ext>`, NOT at `<outDir>/<artifact>`.
    //
    // COMPUTED EXPLICITLY rather than absorbed into the manifest string. Writing
    // the target's `artifact` as `"pe64-x86_64-windows-exec/main.exe"` composes
    // correctly through `fs::path` and was the cheaper edit — and it would make
    // `artifact` mean a FILENAME in the 1,930 target entries that carry it today
    // and a PATH in the project-mode ones, with the routing rule hidden inside a
    // string that no reader of the manifest can connect to
    // `setPerFormatOutputSubdir`. One key, one meaning; the runner owns the
    // routing rule because the runner is what has to know it.
    auto const artifactPath = m.project.has_value()
        ? outDir / specFormatName(t.spec) / t.artifact
        : outDir / t.artifact;
    if (!fs::exists(artifactPath)) {
        ADD_FAILURE() << "artifact missing at " << artifactPath.generic_string()
                      << " (arm=" << armLabel << ")";
        armResult.verdict = ArmVerdict::Poisoned;
        return armResult;
    }
    // Use the no-throw overload — a non-existent path past the
    // EXPECT above would otherwise throw filesystem_error and abort
    // the gtest process rather than poison the arm cleanly. Per
    // [fs.op.file_size], on error sz is `static_cast<uintmax_t>(-1)`
    // = UINT64_MAX, so an unguarded `EXPECT_GT(sz, 0u)` would
    // spuriously PASS on file_size failure. Gate on sz_ec to make
    // both checks honest.
    std::error_code sz_ec;
    auto const sz = fs::file_size(artifactPath, sz_ec);
    if (sz_ec) {
        ADD_FAILURE() << "file_size failed: " << sz_ec.message()
                      << " (arm=" << armLabel << ")";
        armResult.verdict = ArmVerdict::Poisoned;
        return armResult;
    }
    if (sz == 0u) {
        ADD_FAILURE() << "artifact is empty: " << artifactPath.generic_string()
                      << " (arm=" << armLabel << ")";
        armResult.verdict = ArmVerdict::Poisoned;
        return armResult;
    }

    // D-TEST-A-RELEASE-ARM-BYTE-IDENTICAL-TO-BASELINE-ASSERTS-NOTHING: take the
    // image NOW. `scratch` is destroyed when this function returns, so this is
    // the last moment the artifact exists. Deliberately BEFORE the `runOn` and
    // emulator gates below: whether an image was TRANSFORMED is a question
    // about the COMPILE, and a machine that cannot spawn the binary can still
    // answer it. (The comparison itself still only happens when both arms ran —
    // an optimized arm is not even compiled when the baseline did not run, which
    // is D-TEST-EXAMPLES-CROSS-HOST-RELEASE-ARM-NEVER-COMPILED's business, not
    // this row's.)
    auto captured = readArtifactBytes(artifactPath);
    // ★ CROSS-CHECKED AGAINST `file_size`, not merely non-empty. A SHORT READ is
    // the capture failure this mechanism is least able to survive: it produces a
    // plausible prefix, and two arms' prefixes are far likelier to match than
    // their whole images — so a truncating reader would manufacture "identical"
    // findings out of thin air. `sz` is an independent measurement of the same
    // file, so requiring the two to agree turns that into a loud failure.
    if (!captured.has_value() || captured->empty()
        || static_cast<std::uintmax_t>(captured->size()) != sz) {
        // FAIL LOUD, never fall through with a partial or empty string: an
        // uncaptured image reaching the identity rule as "" would compare equal
        // to any other uncaptured image and report the strongest possible
        // finding on no evidence at all.
        ADD_FAILURE() << "could not read the produced artifact for the"
                         " optimized-vs-baseline identity check: "
                      << artifactPath.generic_string() << " (arm=" << armLabel
                      << ", file_size reported " << sz << " bytes, read "
                      << (captured.has_value()
                              ? std::to_string(captured->size())
                              : std::string{"<open failed>"})
                      << ')';
        armResult.verdict = ArmVerdict::Poisoned;
        return armResult;
    }
    armResult.artifactBytes = std::move(*captured);

    auto const host = currentHostOs();
    bool const shouldRun = std::any_of(t.runOn.begin(), t.runOn.end(),
        [&](std::string const& s) { return s == host; });
    if (!shouldRun) {
        std::ostringstream why;
        why << "runOn=[";
        for (std::size_t i = 0; i < t.runOn.size(); ++i) {
            if (i != 0) why << ',';
            why << t.runOn[i];
        }
        why << "] excludes host=" << host;
        GTEST_LOG_(INFO) << "spec=" << t.spec << " arm=" << armLabel
                         << " produced an artifact but " << why.str();
        armResult.verdict = ArmVerdict::SkippedByRunOn;
        armResult.detail  = why.str();
        return armResult;
    }

    // D-LK10-ENTRY-ARM64: cross-ARCH execution. The runOn gate matched
    // the host OS; now reconcile the host ARCH. A binary whose target
    // arch differs from the host's cannot exec natively — it needs an
    // emulator (e.g. qemu-aarch64 for an AArch64 ELF on x86_64). The
    // emulator is declared per-target in the manifest.
    //
    // D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT: the two outcomes below used to
    // share one status and one shrug. They do not share a meaning. NO EMULATOR
    // DECLARED is a property of the MANIFEST — no machine can fix it, and
    // `lintDeclaredEmulators` reds it host-independently rather than leaving it
    // to whichever leg happens to notice. EMULATOR MISSING is a property of
    // this MACHINE — a developer without qemu still gets a green suite, while
    // the gate sets DSS_STRICT_ARM_VERDICTS=1 and the same skip becomes a RED.
    // Both are ledgered either way; neither can be read as a pass again.
    std::vector<std::string> launcherPrefix;
    if (std::string const targetArch = specTargetArch(t.spec);
        !targetArch.empty() && targetArch != currentHostArch()) {
        if (t.emulator.empty()) {
            std::ostringstream why;
            why << "target arch '" << targetArch << "' != host arch '"
                << currentHostArch() << "' and the manifest declares no"
                   " 'emulator'";
            GTEST_LOG_(INFO) << "spec=" << t.spec << " arm=" << armLabel
                             << ' ' << why.str() << " — skipping run";
            armResult.verdict = ArmVerdict::SkippedNoEmulatorDeclared;
            armResult.detail  = why.str();
            return armResult;
        }
        auto const emuPath = findOnPath(t.emulator);
        if (emuPath.empty()) {
            std::ostringstream why;
            why << "declared emulator '" << t.emulator
                << "' is not on PATH (target arch '" << targetArch
                << "' != host arch '" << currentHostArch() << "')";
            GTEST_LOG_(INFO) << "spec=" << t.spec << " arm=" << armLabel
                             << ' ' << why.str() << " — skipping cross-arch run";
            armResult.verdict = ArmVerdict::SkippedEmulatorMissing;
            armResult.detail  = why.str();
            return armResult;
        }
        launcherPrefix.push_back(emuPath);
    }

    // ── D-TEST-QEMU_LD_PREFIX-AMBIENT-ONLY, closing-work item (2), first half ──
    //
    // ⚠ This is a PARTIAL discharge of an OPEN row, not its closure. That row
    // asks for two things: (1) the harness DERIVES the guest sysroot instead of
    // inheriting it ambiently, and (2) an exit-255 under an emulator is
    // CLASSIFIED as an environment failure and quotes the loader line rather
    // than being presented as an exit-code mismatch. What follows is the cheap,
    // no-masking half of (2): the mismatch is still reported as a mismatch, but
    // it now carries the remedy. Full classification still owes the row.
    //
    // Being on PATH is not the same as being ABLE TO RUN, and the gap between
    // those two facts costs an operator an afternoon. `qemu-aarch64` resolves
    // the guest's ELF interpreter (`/lib/ld-linux-aarch64.so.1`) against the
    // HOST filesystem unless `QEMU_LD_PREFIX` points at a guest sysroot. On a
    // box with qemu installed but no sysroot on that path, EVERY emulated arm
    // dies at exit 255 before the guest's first instruction — so a perfectly
    // correct binary reds, and it reds ~450 times at once, which reads as a
    // catastrophic compiler regression rather than a missing package.
    //
    // ✔MEASURED 2026-08-12 on this project's WSL leg: a raw `ctest` with
    // QEMU_LD_PREFIX unset reported 468 of 826 failing; with
    // `QEMU_LD_PREFIX=/usr/aarch64-linux-gnu` exported, 826 of 826 passed. The
    // repo already knew — `real-examples/c/sqlite/build-and-test.sh` sets it
    // per-leg and its own comment says "without it qemu cannot find the guest
    // loader and EVERY exec dies at exit 255, which would read as 14 DSS
    // failures on a binary that is completely fine", and notes the variable
    // "lived for months as an operational workaround rather than as a checked
    // prerequisite". The ctest corpus runner never got that treatment.
    //
    // ★ THIS DELIBERATELY DOES NOT SKIP, SUPPRESS OR AUTO-SET ANYTHING. The
    // arm still runs and still reds — masking a cross-arch failure is exactly
    // the workaround the bar forbids, and guessing a sysroot path would be a
    // host-identity assumption in a harness that has none. It only makes the
    // red SAY what is most likely wrong. Turning this into a proper
    // `SkippedLauncherPrerequisiteMissing` verdict needs the guest's PT_INTERP
    // read out of the artifact and probed; that is the registry row's work.
    // The remedy text this condition earns is attached where the mismatch is
    // ASSERTED, in `runOneTarget` — see `qemuSysrootHint` below.

    // Model 3: capture stdout when EITHER the manifest-level pin OR this target's
    // override is present (so a per-target override alone still routes the pipe).
    bool const captureStdout =
        m.expectedStdout.has_value() || t.expectedStdoutOverride.has_value();
    // kRunBudget bounds the PROGRAM's runtime only — runBinary absorbs the
    // one-time OS process-admission cost in an untimed warm-up exec first
    // (TF-C84; see run_binary.hpp for the measurements). Before that split
    // this site was a bare `5000` literal that had to cover both, and the
    // OS half — a Gatekeeper notarization round trip that serializes across
    // concurrent execs — pushed 35/737 tests over it under `ctest -j8`.
    auto const result = runBinary(artifactPath,
                                  kRunBudget,
                                  captureStdout,
                                  launcherPrefix);
    EXPECT_TRUE(result.spawned)
        << "spawn failed for " << artifactPath.generic_string()
        << " (arm=" << armLabel << ") diag=" << result.diagnostic;
    EXPECT_FALSE(result.timedOut)
        << "spawn timed out for " << artifactPath.generic_string()
        << " (arm=" << armLabel << ") diag=" << result.diagnostic;
    // Poison the arm if spawn failed or timed out — the binary never
    // actually ran, so result.exitCode + result.capturedStdout are
    // garbage. Without this, the differential-verify ASSERT_EQ would
    // compare two arms' bogus exit codes and could spuriously pass
    // when both arms time out (silent-bypass re-opens the very gap
    // the ArmVerdict states were introduced to close).
    if (!result.spawned || result.timedOut) {
        armResult.verdict = ArmVerdict::Poisoned;
        armResult.detail  = result.spawned ? "spawn timed out" : "spawn failed";
        return armResult;
    }
    armResult.verdict        = ArmVerdict::Ran;
    armResult.exitCode       = result.exitCode;
    armResult.capturedStdout = result.capturedStdout;
    return armResult;
}

void runOneTarget(fs::path const&        exampleDir,
                  ExampleManifest const& m,
                  ExampleTarget const&   t,
                  std::string const&     exampleId,
                  ArmVerdictLedger&      ledger,
                  ArtifactIdentityTally& identity) {
    ASSERT_FALSE(t.spec.empty())
        << "target spec missing in manifest";
    ASSERT_FALSE(t.artifact.empty())
        << "artifact filename missing in target";

    // Baseline arm: no pipeline override; compile_pipeline picks the
    // default. Non-Ran outcomes are distinguished by ArmVerdict:
    //   Poisoned → compile failed; EXPECT already fired; return.
    //   Skipped* → compile clean; the binary won't run on this host for a
    //              named reason; arm-comparison is N/A → return.
    auto const baseline = compileAndRunArm(exampleDir, m, t,
                                           /*pipelineOverride*/ nullptr,
                                           "baseline");
    ledger.record(exampleId, t.spec, "baseline", baseline.verdict,
                  baseline.detail);
    if (baseline.verdict != ArmVerdict::Ran) {
        // D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT: the declared optimized arms
        // are NOT compiled when the baseline does not run (the early return is
        // also what D-TEST-EXAMPLES-CROSS-HOST-RELEASE-ARM-NEVER-COMPILED owns).
        // They are DECLARED work, so they get a verdict too rather than
        // vanishing from the accounting — carrying the baseline's reason, and
        // saying plainly that not even the compile was attempted.
        for (auto const& arm : m.optimizedPipelines) {
            ledger.record(exampleId, t.spec, arm.label, baseline.verdict,
                          baseline.detail
                              + " (compile not attempted: baseline arm did not run)");
        }
        return;
    }

    // Model 3: the per-target override (when present) is the authority for THIS
    // target's stdout; otherwise the manifest-level pin applies. The differential
    // arm below compares the optimized arm to the BASELINE's captured stdout, so it
    // follows this choice for free.
    std::optional<std::string> const effectiveStdout =
        t.expectedStdoutOverride.has_value() ? t.expectedStdoutOverride
                                             : m.expectedStdout;
    // The per-target exit-code override (when present) is the authority for THIS
    // target; otherwise the manifest-level `exitCode`. The differential arm below
    // compares the optimized arm to the BASELINE exit code, so it follows this
    // choice for free.
    std::int64_t const effectiveExit =
        t.exitCodeOverride.has_value() ? *t.exitCodeOverride : m.exitCode;

    // Baseline strict pins against the manifest (per-target override applied).
    ASSERT_EQ(static_cast<std::int64_t>(baseline.exitCode), effectiveExit)
        << "baseline exit-code mismatch (expected=" << effectiveExit
        << "; OS=" << baseline.exitCode << ")"
        << qemuSysrootHint(t.spec, t.emulator);
    if (effectiveStdout.has_value()) {
        ASSERT_EQ(baseline.capturedStdout, *effectiveStdout)
            << "baseline stdout mismatch (expected=" << effectiveStdout->size()
            << " bytes; OS=" << baseline.capturedStdout.size() << " bytes)";
    }

    // D-OPT1-DIFFERENTIAL-VERIFY-RUNNER: each declared optimized arm
    // produces an artifact whose exit code + stdout MUST match the
    // baseline. Corpus negative pins (plan 22 §3.1) drive this — a
    // broken pass produces divergent output and the assert names
    // the pipeline.
    for (auto const& arm : m.optimizedPipelines) {
        SCOPED_TRACE("optimizedPipeline=" + arm.label);
        auto const pipeline = buildPipeline(arm, exampleDir / "expected.json");
        if (!pipeline.has_value()) {
            // ADD_FAILURE already fired; the arm still gets a verdict so the
            // ledger's total matches the manifest's declared arm count.
            ledger.record(exampleId, t.spec, arm.label, ArmVerdict::Poisoned,
                          "optimizedPipelines arm could not be resolved");
            continue;
        }
        auto const optResult = compileAndRunArm(exampleDir, m, t, &*pipeline,
                                                 arm.label.c_str());
        ledger.record(exampleId, t.spec, arm.label, optResult.verdict,
                      optResult.detail);
        // The two arms must share the run outcome: both ran (compare) or both
        // skipped. A skipped baseline already returned above; here the
        // optimized arm must agree.
        ASSERT_EQ(static_cast<int>(optResult.verdict),
                  static_cast<int>(ArmVerdict::Ran))
            << "differential-verify: optimized arm '" << arm.label
            << "' verdict=" << armVerdictName(optResult.verdict)
            << " — baseline ran but optimized arm did not";
        // Counted HERE — at the verdict, not at the comparison — so that a
        // deleted or short-circuited comparison shows up as a count mismatch in
        // the entry summary instead of as a check that quietly stopped looking.
        ++identity.armsThatRan;

        // ── D-TEST-A-RELEASE-ARM-BYTE-IDENTICAL-TO-BASELINE-ASSERTS-NOTHING ──
        //
        // Both arms ran, so both images exist as bytes. Ask whether the
        // pipeline changed anything at all BEFORE asserting that the program's
        // behaviour is unchanged: if it did not, the two assertions below are
        // comparing an artifact with itself and the arm is coverage in name
        // only. Report by default; RED when the arm declared it must differ.
        {
            auto const j = judgeOptimizedArtifactIdentity(
                baseline.artifactBytes, optResult.artifactBytes,
                arm.mustDifferFromBaseline);
            ++identity.compared;
            if (j.outcome == ArtifactIdentity::Identical) ++identity.identical;
            if (!j.note.empty()) {
                identity.notes.push_back("spec=" + t.spec + " arm=" + arm.label
                                         + ": " + j.note);
            }
            if (j.red) {
                ADD_FAILURE()
                    << "ARTIFACT IDENTITY: " << exampleId << " spec=" << t.spec
                    << " arm='" << arm.label << "' — " << j.note;
            }
        }

        ASSERT_EQ(optResult.exitCode, baseline.exitCode)
            << "differential-verify FAIL: optimized arm '" << arm.label
            << "' produced exit code " << optResult.exitCode
            << " vs baseline " << baseline.exitCode
            << " — pipeline regression";
        if (effectiveStdout.has_value()) {
            ASSERT_EQ(optResult.capturedStdout, baseline.capturedStdout)
                << "differential-verify FAIL: optimized arm '" << arm.label
                << "' stdout differs from baseline";
        }
    }
}

// V2-4 Part C (D-DIAG-CLI-POSITION-RENDER-AND-ASSERT): drive the FULL
// Program::compileFiles path on a malformed source and assert the EXACT
// positioned diagnostic set. The compile MUST fail; no binary is spawned.
// AGNOSTIC: the expected diagnostics are JSON-declared and compared
// generically (code NAME + 1-based line:col) — no language/code hardcoded.
void runErrorTarget(fs::path const&        exampleDir,
                    ExampleManifest const& m,
                    ExampleTarget const&   t,
                    std::string const&     exampleId,
                    ArmVerdictLedger&      ledger) {
    // D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT: an expect-error arm is VERIFIED
    // without ever being spawned — its assertion is the exact diagnostic set
    // below, which is host-independent. Recorded up-front (rather than after
    // the asserts) so that an ASSERT_ returning early still leaves the arm
    // accounted for; a failed assertion is already the loud signal, and a
    // MISSING ledger row would understate the declared work.
    ledger.record(exampleId, t.spec, "expect-error",
                  ArmVerdict::ExpectErrorAsserted,
                  "compile must fail with the declared diagnostic set"
                  " (nothing is spawned)");
    ASSERT_FALSE(t.spec.empty())
        << "expect-error target needs a 'spec' to drive the compile";
    // Single-source keeps position resolution unambiguous: one source
    // buffer, so a diagnostic's span offset maps to exactly this file.
    ASSERT_EQ(m.sources.size(), 1u)
        << "expectDiagnostics examples must be single-source";

    ScratchDir scratch{Location::InsideRepo, "examples"};
    auto const srcRel  = m.sources.front();
    auto const srcPath = scratch.path() / srcRel;
    fs::copy_file(exampleDir / srcRel, srcPath,
                  fs::copy_options::overwrite_existing);
    scratch.useAsCwd();

    Program            prog;
    DiagnosticReporter rep;
    prog.setOutputDir(scratch.path() / "out");
    int const rc =
        prog.compileFiles({srcPath.generic_string()}, m.language, {t.spec}, rep);

    // A malformed source MUST be rejected — both the driver return code and
    // the error-severity count confirm the compile failed (not a silent pass).
    EXPECT_NE(rc, 0)
        << "expect-error example compiled with rc=0 (should be rejected): "
        << exampleDir.generic_string();
    EXPECT_GT(rep.errorCount(), 0u)
        << "expect-error example produced no error-severity diagnostics: "
        << exampleDir.generic_string();

    // Resolve every diagnostic's START position through the SAME
    // SourceBuffer::lineCol the compiler uses, so the asserted 1-based
    // line:col matches its convention exactly (read binary so the byte
    // offsets line up with the compiler's own buffer).
    std::ifstream in(srcPath, std::ios::binary);
    std::string const srcBytes{std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>()};
    auto const srcBuf = SourceBuffer::fromString(srcBytes, srcRel);

    // #4: codes declared `positioned:false` are emitted at a span-less tier, so
    // their position is not asserted — both sides render them code-only. Every
    // other code keeps its precise `code line:col` pin.
    std::set<std::string> codeOnly;
    for (auto const& e : m.expectDiagnostics)
        if (!e.positioned) codeOnly.insert(e.code);

    auto const render = [&codeOnly](std::string_view code,
                                    std::uint32_t line, std::uint32_t col) {
        std::string s(code);
        if (codeOnly.count(s) != 0) return s;   // position-independent match
        s += ' ';
        s += std::to_string(line);
        s += ':';
        s += std::to_string(col);
        return s;
    };

    std::vector<std::string> actual;
    actual.reserve(rep.all().size());
    for (auto const& d : rep.all()) {
        auto const lc = srcBuf->lineCol(d.span.start());
        actual.push_back(render(diagnosticCodeName(d.code), lc.line, lc.column));
    }
    std::vector<std::string> expected;
    expected.reserve(m.expectDiagnostics.size());
    for (auto const& e : m.expectDiagnostics) {
        expected.push_back(render(e.code, e.line, e.col));
    }
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());

    auto const join = [](std::vector<std::string> const& v) {
        std::string s;
        for (auto const& x : v) { s += "\n    "; s += x; }
        return s;
    };
    // EXACT set equality — the strongest pin (the user's strict-assert
    // invariant): every produced diagnostic is declared and vice versa,
    // each at its precise positioned line:col.
    ASSERT_EQ(actual, expected)
        << "expect-error diagnostic-set mismatch for "
        << exampleDir.generic_string()
        << "\n  expected:" << join(expected)
        << "\n  actual:"   << join(actual);
}

} // namespace

// argv[1] = absolute path to the example dir (registered by cmake).
//
// The harness reads `<dir>/expected.json`, drives every target spec
// in the manifest, and exits 0 only if every assertion passed.
TEST(Examples, RunFromManifest) {
    auto const argv0Dir =
        ::testing::internal::GetArgvs();
    ASSERT_GE(argv0Dir.size(), 2u)
        << "examples_runner needs the example directory as argv[1]";
    fs::path const exampleDir = argv0Dir[1];
    ASSERT_TRUE(fs::is_directory(exampleDir))
        << "argv[1] is not a directory: " << exampleDir.generic_string();
    auto const manifestPath = exampleDir / "expected.json";
    ASSERT_TRUE(fs::exists(manifestPath))
        << "expected.json missing in " << exampleDir.generic_string();
    auto const m = readManifest(manifestPath);
    ASSERT_FALSE(m.targets.empty()) << "manifest declared no targets";

    // `<lang>/<name>` — the SAME example id the CLI-subprocess runner prints,
    // so a ledger line from either harness is greppable the same way.
    auto const exampleId =
        exampleDir.parent_path().filename().generic_string() + "/"
        + exampleDir.filename().generic_string();

    ArmVerdictLedger      ledger;
    ArtifactIdentityTally identity;
    for (auto const& t : m.targets) {
        SCOPED_TRACE("target spec=" + t.spec);
        // V2-4 Part C: an `expectDiagnostics` manifest asserts a failed
        // compile + positioned diagnostics; otherwise the source must
        // compile + run cleanly.
        if (m.expectDiagnostics.empty()) {
            runOneTarget(exampleDir, m, t, exampleId, ledger, identity);
        } else {
            runErrorTarget(exampleDir, m, t, exampleId, ledger);
        }
    }


    // ── D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT: the ledger ───────────────
    //
    // EVERY declared arm is now in `ledger`, and it is printed unconditionally.
    // ctest hides a passing test's stdout by default, which is precisely why
    // the ledger is not the enforcement — the STRICT GATE below is. The print
    // is what makes `ctest -V` / `--output-on-failure` answer "what did this
    // entry actually verify?" without re-deriving it from the manifest.
    std::cout << "[arm-ledger] " << exampleId << ": "
              << ledger.renderCountsLine() << '\n'
              << ledger.renderSkipDetail("[arm-ledger]   ");

    // Nothing declared, nothing verified: an entry that produced NO verdict at
    // all is the defect in its purest form, so it is a failure on every host
    // regardless of strict mode.
    ASSERT_GT(ledger.total(), 0u)
        << "no arm produced a verdict for " << exampleId
        << " — a ctest entry that verifies nothing must never read as a pass";

    // Strict mode. ENVIRONMENTAL skips only: a `runOn` exclusion is the
    // manifest's intent and a missing `emulator` KEY is a manifest defect the
    // corpus lint reds host-independently, but "the manifest asked for a run
    // and this machine could not supply it" is exactly the condition the gate
    // must not be allowed to call green.
    auto const strict = ::dss::test_support::readStrictArmVerdicts();
    ASSERT_FALSE(strict.malformed)
        << ::dss::test_support::kStrictArmVerdictsEnv << " has unexpected value '"
        << strict.raw << "' — use '1' to require every declared arm to run,"
           " unset (or '0') otherwise. Refusing to interpret it, because a"
           " typo that silently disabled the gate is the failure this variable"
           " exists to prevent.";
    for (auto const& r : ledger.environmentalSkips()) {
        if (strict.on) {
            ADD_FAILURE()
                << "STRICT ARM VERDICTS: " << r.example << " spec=" << r.spec
                << " arm=" << r.arm << " did not run — " << r.detail
                << ". This machine cannot supply what the manifest declared;"
                   " install the emulator or unset "
                << ::dss::test_support::kStrictArmVerdictsEnv << '.';
        } else {
            std::cout << "[arm-ledger] WARNING " << r.example
                      << " spec=" << r.spec << " arm=" << r.arm
                      << " did not run — " << r.detail
                      << " (set "
                      << ::dss::test_support::kStrictArmVerdictsEnv
                      << "=1 to make this a failure)\n";
        }
    }

    // ── D-TEST-A-RELEASE-ARM-BYTE-IDENTICAL-TO-BASELINE-ASSERTS-NOTHING ─────
    //
    // LAST in the body, deliberately: the assertion below is fatal, and the arm
    // ledger and the strict-verdict gate above are what a reader triages a red
    // entry with. An assertion that suppressed them would answer its own
    // question by hiding everyone else's.
    //
    // Printed UNCONDITIONALLY and with a stable tag, for the same reason the
    // arm ledger is: this is the accounting that answers "what did this entry's
    // optimized arms actually witness?", and it is worthless if it only appears
    // when something is wrong. `compared=0` on an entry with no optimized arms
    // is a real and expected line — most of the corpus has none.
    std::cout << "[artifact-identity] " << exampleId
              << ": optimized-arms-ran=" << identity.armsThatRan
              << " compared=" << identity.compared
              << " byte-identical=" << identity.identical << '\n';
    for (auto const& n : identity.notes) {
        std::cout << "[artifact-identity]   " << exampleId << ' ' << n << '\n';
    }

    // ★ THE CHECK'S OWN NON-VACUITY GUARD, and it is a hard failure on every
    // host regardless of any opt-in. The report above is advisory BY DESIGN,
    // which makes it exactly the kind of mechanism that can rot into silence —
    // delete the comparison block, or let capture start handing back nothing,
    // and every entry keeps printing a cheerful zero. So the two counts are
    // taken at DIFFERENT SITES (`armsThatRan` at the arm's verdict,
    // `compared` inside the comparison) and reconciled here: an optimized arm
    // that RAN and was not COMPARED is a harness defect, not a quiet day.
    ASSERT_EQ(identity.compared, identity.armsThatRan)
        << "artifact-identity accounting mismatch for " << exampleId << ": "
        << identity.armsThatRan
        << " optimized arm(s) ran but only " << identity.compared
        << " were compared against their baseline image. The optimized-vs-"
           "baseline byte comparison is what keeps a declared arm from counting"
           " as coverage while witnessing nothing; a comparison that stopped"
           " happening must never present as a passing entry.";
}

// D-ASM-AARCH64-FRAME-OFFSET-BEYOND-16MIB (harness-wiring pin): the shared
// spawn chokepoint `runBinary` must apply the generous-stack bump so BOTH
// harnesses that spawn through it — this in-process runner AND the separate
// `integrated_tests` CLI-subprocess runner — inherit it. The integrated_tests
// runner originally LACKED the bump, so the native arm64-Linux leg SIGSEGV'd
// (exit 139) on the ~20 MB-frame `large_frame_beyond_16mib` example, while
// every other leg skipped that target as cross-arch/cross-format and stayed
// green. This pin is the EVERY-LEG wiring guard (the native arm64 leg running
// the example to exit 42 is the runtime witness for the load-bearing rlimit
// raise). Red-on-disable: drop the `ensureGenerousSpawnStack()` call in
// `runBinary` → QEMU_STACK_SIZE unset here AND the native large-frame run
// crashes on the arm64 leg.
TEST(RunHarnessStack, GenerousSpawnStackBumpIsWired) {
    dss::test_support::ensureGenerousSpawnStack();

    char const* const qss = std::getenv("QEMU_STACK_SIZE");
    ASSERT_NE(qss, nullptr)
        << "the qemu cross-arch corpus path needs QEMU_STACK_SIZE set";
    EXPECT_STREQ(qss, "268435456");

#if !defined(_WIN32)
    // The load-bearing mechanism for the NATIVE large-frame run: the parent's
    // RLIMIT_STACK soft limit is raised toward 256 MiB so a posix_spawn child
    // inherits a stack large enough for the ~20 MB frame.
    struct rlimit rl{};
    ASSERT_EQ(::getrlimit(RLIMIT_STACK, &rl), 0);
    rlim_t const want = static_cast<rlim_t>(268435456);  // 256 MiB
    rlim_t const atLeast =
        (rl.rlim_max == RLIM_INFINITY) ? want
                                       : std::min<rlim_t>(want, rl.rlim_max);
    EXPECT_GE(rl.rlim_cur, atLeast)
        << "RLIMIT_STACK soft limit must be raised toward 256 MiB";
#endif
}

// ── D-TEST-MANIFEST-ARM64-ARM-WITHOUT-EMULATOR: the corpus emulator lint ───
//
// Registered inside the ONE extra ctest entry (`examples/corpus-lints`, which
// was named `examples/manifest-emulator-lint` until AP6 renamed it for the
// SUITE once the suite outgrew this one lint)
// rather than running inside all 558 per-example entries: the question is
// corpus-wide, the answer is identical for every entry, and re-deriving it 558
// times would cost the suite a corpus re-walk per test for no new information.
// The per-example entries exclude it by `--gtest_filter`; this entry selects it
// and is handed the corpus ROOT as argv[1] (see tests/examples/CMakeLists.txt).
//
// It deliberately reuses THIS file's `readManifest` — the same parser the
// in-process runner drives the corpus with — so the lint can never be reading a
// different manifest shape than the run does. The subprocess runner's copy of
// the lint does the same with ITS parser, and both call the one shared rule in
// `lintDeclaredEmulators`.
//
// Host-independent BY CONSTRUCTION: nothing below consults the host OS or arch,
// so the omission is caught on whichever leg the author happens to run rather
// than only on a leg whose arch differs from the arm's.
TEST(ExamplesCorpusLint, EveryArmAgreesWithItsSiblingsOnTheEmulator) {
    auto const argv = ::testing::internal::GetArgvs();
    ASSERT_GE(argv.size(), 2u)
        << "the corpus lint entry needs the examples ROOT as argv[1]";
    fs::path const corpusRoot = argv[1];
    ASSERT_TRUE(fs::is_directory(corpusRoot))
        << "argv[1] is not a directory: " << corpusRoot.generic_string();

    std::vector<::dss::test_support::DeclaredArm> arms;
    std::size_t manifestCount = 0;
    for (auto const& langEntry : fs::directory_iterator(corpusRoot)) {
        if (!langEntry.is_directory()) continue;
        for (auto const& nameEntry : fs::directory_iterator(langEntry.path())) {
            if (!nameEntry.is_directory()) continue;
            auto const mp = nameEntry.path() / "expected.json";
            if (!fs::exists(mp)) continue;
            ++manifestCount;
            auto const m = readManifest(mp);
            auto const exampleId =
                langEntry.path().filename().generic_string() + "/"
                + nameEntry.path().filename().generic_string();
            for (auto const& t : m.targets) {
                for (auto const& osName : t.runOn) {
                    arms.push_back({exampleId, t.spec, osName, t.emulator});
                }
            }
        }
    }
    // A lint that silently linted nothing is the same class of defect as the
    // skip it exists to catch.
    ASSERT_GT(manifestCount, 0u)
        << "no expected.json manifests under " << corpusRoot.generic_string()
        << " — the lint measured nothing and must not report success";
    ASSERT_FALSE(arms.empty())
        << "no target arm in the corpus declares a runOn host";

    auto const findings = ::dss::test_support::lintDeclaredEmulators(arms);
    std::ostringstream report;
    for (auto const& f : findings) {
        report << "\n  " << (f.manifest.empty() ? "corpus" : f.manifest);
        if (!f.spec.empty()) report << " spec=" << f.spec;
        report << ": " << f.message;
    }
    EXPECT_TRUE(findings.empty())
        << findings.size() << " manifest emulator finding(s) over "
        << manifestCount << " manifests / " << arms.size()
        << " declared (arm x runOn) pairs:" << report.str();
}

// ── Recursive neighbour staging: the BEHAVIOURAL pin ───────────────────────
//
// Filed in the `ExamplesCorpusLint` suite, which is the ONE suite the
// per-example ctest entries exclude by `--gtest_filter` and the single
// `examples/corpus-lints` entry selects. The question this asks is
// corpus-wide and its answer identical for every entry, so it runs ONCE rather
// than 581 times — the same reasoning the emulator lint above records.
//
// ⚠ RUN THIS SUITE THROUGH ctest, NOT the bare `dss_examples_runner.exe`.
// This pin itself is cwd-independent — it plants its own fixture in a temp
// sandbox — but its sibling below resolves the repo through `repo_root.hpp`,
// whose candidate (3) is a 12-hop walk UP from the PROCESS cwd, and the
// compiles this binary drives resolve shipped config the same way
// (`core/types/config_path_walk.cpp`). A bare invocation started somewhere
// else can therefore walk into a DIFFERENT tree and assert confidently against
// it. Only the ctest entry pins the answer: it sets `WORKING_DIRECTORY
// ${CMAKE_SOURCE_DIR}` and the target bakes `DSS_TEST_REPO_ROOT` (see the ★ -D
// note in tests/examples/CMakeLists.txt, which exists because this binary was
// the ONE test target that had silently lost the baked root).
//
// The assertions live in `stageExampleTreeSelfTest`, in the shared header, so
// that the CLI-subprocess runner executes THE SAME checks from THE SAME code —
// which is what the hoist bought: "character-identical" used to be a property a
// lint had to keep re-proving, and is now a property of there being one copy.
// All this wrapper adds is a scratch sandbox and the GTest reporting.
TEST(ExamplesCorpusLint, StagesNestedSubdirectoriesWithContentIntact) {
    // `Location::Temp`, not `InsideRepo`: this pin drives no schema loader, so
    // it needs no cwd-rooted config walk, and keeping its fixture out of the
    // repo means a crashed run cannot leave a stray `dep_module/` where the
    // corpus glob might later meet it.
    ScratchDir sandbox{Location::Temp, "stage-tree-pin"};
    std::string const findings =
        stageExampleTreeSelfTest(sandbox.path() / "stage-tree");
    EXPECT_TRUE(findings.empty()) << findings;
}

// ── D-TEST-A-RELEASE-ARM-BYTE-IDENTICAL-TO-BASELINE-ASSERTS-NOTHING ────────
//
// The two-direction pin for the optimized-vs-baseline identity check. Filed in
// `ExamplesCorpusLint` (the one suite the per-example entries exclude and the
// single `examples/corpus-lints` entry selects) because its answer is the same
// for every entry, exactly like its neighbours here.
//
// ★ WHY THE FIXTURE IS TWO FILES OF BYTES AND NOT TWO COMPILES. The obvious
// fixture — an example whose release arm genuinely optimizes and one whose
// arm cannot — would pin the rule THROUGH the compiler, and would thereby make
// this pin's own verdict depend on the host, the target format, and on the
// optimizer continuing to make that particular choice. The "arms differ" half
// would silently invert on any leg where that source stopped optimizing, and a
// pin that can invert is worse than none. The MECHANISM under test compares two
// artifacts' bytes and knows nothing else; the fixture is built at exactly that
// altitude, so it is deterministic on every leg — while the CORPUS is what
// exercises the mechanism against real compiler output on every run, and the
// `compared == armsThatRan` reconciliation in `RunFromManifest` is what makes
// that exercise fail loud if the wiring between them ever breaks.
//
// The bytes deliberately contain embedded NULs: capture goes through a text-
// mode-hostile path (an `ifstream` in binary mode into a `std::string`), and an
// implementation that truncated at the first NUL would make two DIFFERENT
// images compare equal — a false "identical", i.e. a false red on any arm that
// opted in.
TEST(ExamplesCorpusLint, ByteIdenticalOptimizedArtifactIsDetectedBothWays) {
    ScratchDir sandbox{Location::Temp, "artifact-identity-pin"};
    auto const dir = sandbox.path();

    auto const plant = [&dir](char const* name, std::string const& bytes) {
        fs::path const p = dir / name;
        std::ofstream out(p, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();
        // A fixture that failed to write its own subject would make every
        // capture below return the same empty string, and the "identical" half
        // would pass for the wrong reason.
        EXPECT_TRUE(out.good()) << "could not plant " << p.generic_string();
        return p;
    };

    // Two images that DIFFER, and two that are byte-identical while living at
    // DIFFERENT paths (which is the real situation: each arm builds in its own
    // scratch dir, so path equality is never the question).
    //
    // Assembled byte by byte rather than written as one literal with a hand-
    // counted length: a `std::string{lit, N}` whose N is one too large reads
    // past the literal, and the fixture would be pinning undefined behaviour.
    std::string imageA;
    imageA += '\x7F';
    imageA += "DSS";
    imageA += '\0';              // the first embedded NUL
    imageA += "\x01\x02\x03";
    imageA += " baseline image ";
    imageA += '\0';              // and a second, well past it
    imageA += " tail";
    std::string const imageACopy = imageA;

    // The differing byte is chosen PAST the first NUL, so a capture that
    // truncated there could not produce a passing "differed" verdict.
    std::size_t const firstNul = imageA.find('\0');
    ASSERT_NE(firstNul, std::string::npos);
    std::size_t const flipAt = imageA.size() / 2;
    ASSERT_GT(flipAt, firstNul);
    std::string imageB = imageA;
    imageB[flipAt] = static_cast<char>(imageB[flipAt] ^ 0x5A);  // always changes

    ASSERT_EQ(imageA.size(), imageB.size())
        << "the fixture must differ in CONTENT, not in length — a length-only"
           " difference would let a size comparison pass for a byte comparison";
    ASSERT_NE(imageA, imageB);
    ASSERT_EQ(imageA, imageACopy);

    auto const pBaseline  = plant("baseline.img",  imageA);
    auto const pDifferent = plant("optimized.img", imageB);
    auto const pSame      = plant("identical.img", imageACopy);

    // CAPTURE — through the SAME reader the runner uses, so this pin cannot be
    // green over a capture path the corpus does not take.
    auto const capBaseline  = readArtifactBytes(pBaseline);
    auto const capDifferent = readArtifactBytes(pDifferent);
    auto const capSame      = readArtifactBytes(pSame);
    ASSERT_TRUE(capBaseline.has_value());
    ASSERT_TRUE(capDifferent.has_value());
    ASSERT_TRUE(capSame.has_value());
    // Binary-exact, NULs and all — the property the rest of this pin rests on.
    EXPECT_EQ(*capBaseline, imageA);
    EXPECT_EQ(capBaseline->size(), imageA.size());
    EXPECT_NE(*capBaseline, *capDifferent);

    // DIRECTION 1 — the two arms genuinely differ ⇒ the check STAYS QUIET.
    // Quiet means all three things: not Identical, not red, and NOTHING added
    // to the summary. A rule that reported on every arm would drown the corpus
    // and would be ignored, which is the same outcome as not reporting.
    {
        auto const j = judgeOptimizedArtifactIdentity(
            *capBaseline, *capDifferent, /*requireDiffers*/ false);
        EXPECT_EQ(static_cast<int>(j.outcome),
                  static_cast<int>(ArtifactIdentity::Differed));
        EXPECT_FALSE(j.red);
        EXPECT_TRUE(j.note.empty()) << "unexpected note: " << j.note;
    }
    // ... and STILL quiet when the arm opted in: opting in must strengthen the
    // identical case only. If `mustDifferFromBaseline` could red an arm that
    // DID differ, the lever would be unusable and nobody would set it.
    {
        auto const j = judgeOptimizedArtifactIdentity(
            *capBaseline, *capDifferent, /*requireDiffers*/ true);
        EXPECT_EQ(static_cast<int>(j.outcome),
                  static_cast<int>(ArtifactIdentity::Differed));
        EXPECT_FALSE(j.red);
        EXPECT_TRUE(j.note.empty()) << "unexpected note: " << j.note;
    }

    // DIRECTION 2 — the two arms are BYTE-IDENTICAL ⇒ the check FIRES.
    // Report by default: detected and narrated, but not a failure.
    {
        auto const j = judgeOptimizedArtifactIdentity(
            *capBaseline, *capSame, /*requireDiffers*/ false);
        EXPECT_EQ(static_cast<int>(j.outcome),
                  static_cast<int>(ArtifactIdentity::Identical));
        EXPECT_FALSE(j.red) << "the DEFAULT must not red — see the ★ DECISION"
                               " note: 77 corpus arms are identical today, 7 of"
                               " them legitimately (their source is assembly)";
        EXPECT_NE(j.note.find("BYTE-IDENTICAL"), std::string::npos)
            << "the note must SAY what it found; it is the entire output of the"
               " default path. Got: " << j.note;
        // The fingerprint is in the note so an operator can grep one image out
        // of a CI log — it is never what the judgement compared.
        EXPECT_NE(j.note.find(artifactFingerprint(*capBaseline)),
                  std::string::npos) << j.note;
    }
    // ... and the SAME input reds once the arm declares it must differ. This is
    // the escalation lever the corpus can arm per arm.
    {
        auto const j = judgeOptimizedArtifactIdentity(
            *capBaseline, *capSame, /*requireDiffers*/ true);
        EXPECT_EQ(static_cast<int>(j.outcome),
                  static_cast<int>(ArtifactIdentity::Identical));
        EXPECT_TRUE(j.red)
            << "an arm that declares mustDifferFromBaseline and then produces"
               " the baseline's image byte-for-byte must FAIL";
        EXPECT_NE(j.note.find("mustDifferFromBaseline"), std::string::npos)
            << "the red must name the key that caused it. Got: " << j.note;
    }

    // FAIL-CLOSED — an UNCAPTURED image is neither "identical" nor "differed".
    // Two empty strings compare equal, so without this clause a capture that
    // silently returned nothing would report the strongest possible finding on
    // zero evidence, and would red every opted-in arm at once.
    for (auto const require : {false, true}) {
        auto const j = judgeOptimizedArtifactIdentity(*capBaseline, "", require);
        EXPECT_EQ(static_cast<int>(j.outcome),
                  static_cast<int>(ArtifactIdentity::CaptureMissing));
        EXPECT_TRUE(j.red) << "a missing capture must red whether or not the arm"
                              " opted in (requireDiffers=" << require << ')';
        EXPECT_NE(j.note.find("not captured"), std::string::npos) << j.note;
    }
    // And the reader itself reports absence rather than an empty success — the
    // upstream half of the same fail-closed property.
    EXPECT_FALSE(readArtifactBytes(dir / "no-such-artifact.img").has_value());

    // ── THE ESCALATION LEVER MUST ACTUALLY BE WIRED TO THE MANIFEST ─────────
    //
    // ★ This clause exists because of the row this whole change closes. No
    // corpus manifest sets `mustDifferFromBaseline` yet (`examples/**` is
    // another lane's), so the key's PARSE is exercised by nothing — which makes
    // it precisely the shape of defect being fixed here: a declared capability
    // that is never witnessed. A parser that silently read every arm as `false`
    // would leave the lever permanently unarmed, and the arm that opted in
    // would go on passing while the manifest said it must not.
    //
    // AGNOSTIC BY CONSTRUCTION: the fixture manifest's language and target spec
    // are obvious placeholders. `readManifest` stores both as opaque strings —
    // nothing about a real language, arch or object format is needed to ask
    // whether one boolean survived the parse, and naming one here would put
    // target identity into a file that has none.
    {
        fs::path const mp = dir / "expected.json";
        std::ofstream mf(mp);
        mf << R"({
  "language": "<fixture-language>",
  "source": "<fixture-source>",
  "exitCode": 0,
  "targets": [{"spec": "<fixture-arch>:<fixture-format>",
               "artifact": "<fixture-artifact>", "runOn": ["<fixture-host>"]}],
  "optimizedPipelines": [
    {"label": "opted-in",  "passes": ["ConstFold"], "mustDifferFromBaseline": true},
    {"label": "opted-out", "passes": ["ConstFold"], "mustDifferFromBaseline": false},
    {"label": "unstated",  "passes": ["ConstFold"]}
  ]
})";
        mf.close();
        ASSERT_TRUE(mf.good()) << "could not plant " << mp.generic_string();

        auto const parsed = readManifest(mp);
        ASSERT_EQ(parsed.optimizedPipelines.size(), 3u)
            << "the fixture manifest did not parse — the assertions below would"
               " then be reading default-constructed arms";
        EXPECT_EQ(parsed.optimizedPipelines[0].label, "opted-in");
        EXPECT_TRUE(parsed.optimizedPipelines[0].mustDifferFromBaseline)
            << "an arm that declares mustDifferFromBaseline:true must reach the"
               " comparison as opted-in, or the lever is decorative";
        EXPECT_FALSE(parsed.optimizedPipelines[1].mustDifferFromBaseline);
        // ABSENT must mean report-only. If the default ever flipped, the 77
        // byte-identical corpus arms measured today would all red at once.
        EXPECT_FALSE(parsed.optimizedPipelines[2].mustDifferFromBaseline)
            << "an arm that does not mention the key must default to"
               " REPORT-ONLY — see the ★ DECISION note";
    }
}

// ── The primitive has exactly ONE definition site: the STRUCTURAL pin ──────
//
// ★★ THIS PIN REPLACES `StagingTwinsAreCharacterIdentical`, WHICH THE HOIST
// DELETED. That lint read both runners off disk and compared the 14,765 bytes
// between two twin markers, because `stageExampleTree` +
// `stageExampleTreeSelfTest` were duplicated VERBATIM in the two files. It was
// a good guard over a bad situation, and once the block moved to
// `tests/test_support/stage_tree.hpp` it could never fail again — there was no
// longer a second copy to differ from. A pin that cannot fail is worse than no
// pin, because it reads as coverage; so it was removed in the same change that
// removed its subject, and this one took its place.
//
// WHAT CAN STILL REGRESS, and is therefore what this pins: a copy coming BACK.
// That is not hypothetical here — this repo has paid for exactly that drift
// twice (D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT,
// D-TEST-SCOPED-ENV-DUPLICATED-THREE-WAYS), both times by someone adding a
// local copy rather than reaching for the shared header. The three clauses are
// the three ways the single-definition property dies: the header stops
// defining it, a runner starts defining it, or a runner stops including it.
//
// It is deliberately NOT mirrored into the sibling runner, and the distinction
// is worth restating because the house rule cuts the other way: a CAPABILITY
// must land in both harnesses, but this is not a capability — it is a statement
// ABOUT the pair, true or false exactly once, and a second copy would itself
// need a third pin to keep the two copies of the comparison honest.
TEST(ExamplesCorpusLint, StagingPrimitiveLivesOnlyInTheSharedHeader) {
    // ⚠ EVERY NEEDLE IS ASSEMBLED AT RUN TIME, and that is load-bearing rather
    // than stylistic: this pin reads the very file it is compiled from. A whole
    // literal here would be a match in `examples_runner.cpp` no matter what the
    // rest of the file said — so the "no definition in a runner" clause would
    // red on the pin itself, and worse, the "runner includes the header" clause
    // would go GREEN off the pin's own text even if the real `#include` were
    // deleted. That is the exact self-measuring failure the lint this replaces
    // warned about in its own comment.
    std::string const defPrimitive =
        std::string{"std::string stageExample"} + "Tree(fs::path const&";
    std::string const defSelfTest =
        std::string{"std::string stageExample"} + "TreeSelfTest(fs::path const&";
    std::string const includeLine =
        std::string{"#include \"stage_"} + "tree.hpp\"";

    char const* const kHeader = "tests/test_support/stage_tree.hpp";
    char const* const kRunners[] = {
        "tests/examples/examples_runner.cpp",
        "integrated_tests/runner.cpp",
    };
    // Throws with `repoRootDiagnostic()` — which names all three resolution
    // sources and the cwd — rather than failing as a puzzling absent file.
    fs::path const root = ::dss::test::repoRoot();

    auto const slurp = [&root](char const* rel) -> std::string {
        fs::path const p = root / rel;
        std::ifstream in(p, std::ios::binary);
        EXPECT_TRUE(in.good())
            << "cannot read " << p.generic_string()
            << " — this pin must never pass because it failed to look";
        std::string text{std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>{}};
        // CR stripped so a checkout with `core.autocrlf=true` matches the same
        // needles this LF tree does.
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        return text;
    };

    // CLAUSE 1 — the shared header really is the definition site. Checked FIRST
    // and with a size floor, because clauses 2 and 3 are both satisfied by an
    // EMPTY header: no runner defines the primitive if nobody does, and an
    // `#include` of a blank file is still an `#include`. Without this the pin
    // would report the strongest possible green over a deleted implementation.
    std::string const header = slurp(kHeader);
    EXPECT_GT(header.size(), 8000u)
        << kHeader << " is implausibly small (" << header.size()
        << " bytes) — the staging primitive and its self-test should both be"
           " here";
    EXPECT_NE(header.find(defPrimitive), std::string::npos)
        << kHeader << " does not DEFINE the staging primitive, so the two"
                      " runners have no shared implementation to agree on";
    EXPECT_NE(header.find(defSelfTest), std::string::npos)
        << kHeader << " does not DEFINE the staging self-test, so the two"
                      " runners cannot be running the same assertions";

    for (auto const* rel : kRunners) {
        std::string const text = slurp(rel);
        // CLAUSE 2 — no runner may carry its own copy. This is the one that
        // catches the relapse: a second definition compiles perfectly (the
        // local one simply wins unqualified lookup) and the suite stays green
        // while the two harnesses silently stage different trees again.
        EXPECT_EQ(text.find(defPrimitive), std::string::npos)
            << rel << " DEFINES the staging primitive again. It belongs in "
            << kHeader << " and nowhere else — a per-runner copy is how the"
               " 14,765-byte duplication this pin replaced came about, and it"
               " is invisible at the call site.";
        EXPECT_EQ(text.find(defSelfTest), std::string::npos)
            << rel << " DEFINES the staging self-test again; both harnesses"
                      " must run the assertions from "
            << kHeader << ", or one of them is pinning its own copy.";
        // CLAUSE 3 — and it must actually reach the shared one. Without this a
        // runner could satisfy clause 2 by dropping the capability entirely.
        EXPECT_NE(text.find(includeLine), std::string::npos)
            << rel << " does not include " << kHeader
            << ", so whatever staging it performs is not the shared one.";
    }
}
