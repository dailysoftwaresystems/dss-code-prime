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
// binary. This runner binds ONE target per manifest — of those
// whose `runOn` includes the current host, the one whose ARCH IS
// THE HOST'S, else the first such target (`selectBoundTargetIndex`,
// tests/test_support/arm_verdict_ledger.hpp) — and uses THAT
// target spec; if no target matches the host, the example is
// skipped with a loud diagnostic (not silent — every example must
// run somewhere). ⚠ `runOn` names an OS, NOT a machine: two arms
// can both say `linux` while only one of them can execute here,
// which is exactly how a native aarch64 host came to verify
// nothing at all (D-TEST-INTEGRATED-TESTS-CANNOT-PASS-ON-A-NATIVE-
// ARM64-LINUX-HOST).
//
// A SKIP IS NOT A PASS (D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT). Every
// DECLARED target arm of every manifest — including the ones this runner's
// one-target binding never reaches — lands in an `ArmVerdictLedger` with a
// named reason, and the Results line reports `K of T declared target arms NOT
// verified` beside the pass/fail counts. A skip whose cause is the MACHINE
// (a declared `emulator` absent from PATH) is a warning by default and a
// [FAIL] under `DSS_STRICT_ARM_VERDICTS=1`. `[Test 4]` additionally lints the
// corpus for an arm that omits the emulator its (arch, runOn) siblings
// declare. All of it is shared with the in-process sibling
// (`tests/examples/examples_runner.cpp`) through
// `tests/test_support/arm_verdict_ledger.hpp`.
//
// User invariant (2026-06-02): strict asserts on every observable
// — exit codes EQ, no timeouts, no spawn failures. Wrong-value
// breaks the run.
//
// OPTIMIZED ARMS (D-TEST-INTEGRATED-RUNNER-HAS-NO-OPTIMIZATION-ARM-CONCEPT +
// D-TEST-INTEGRATED-RUNNER-IGNORES-THE-RELEASE-ARM-AND-STDOUT-PINS). A manifest
// declares `optimizedPipelines` arms whose binaries must behave exactly like the
// baseline's. This runner used to read NONE of that vocabulary and never passed
// `--config` at all, so it built every example at the CLI default only — the
// project's flagship "witness the OPTIMIZER" rule was enforced by ONE of its two
// runners, over a corpus where a large majority of manifests declare an arm.
// ✔MEASURED 2026-08-17 at HEAD b52784a6: **474 of 597** manifests declare
// `optimizedPipelines`, **388** of them via `shippedPipeline` (all `"release"`).
//
// Each arm the CLI can express is now a SECOND build+run of the same source with
// `--config=<shippedPipeline>`, differentially compared against the baseline —
// exit code, and stdout where the manifest pins it. An arm the CLI CANNOT
// express (an inline `passes` list — there is no flag for one, and publishing
// the optimizer's internal pass vocabulary as a user surface to satisfy a
// harness would be the wrong trade) is ledgered, printed and COUNTED, never
// silently dropped. `[Test 5]` then asserts the whole mechanism is not inert,
// including one check a build that never received the flag cannot satisfy.
//
// PROJECT MODE (D-EXAMPLES-RUNNER-PROJECT-MANIFEST): a manifest may name a
// `.dss-project.json` via the top-level `"project"` key INSTEAD of
// `source`/`sources`, and this runner then drives `dss-code-prime --project`
// instead of `--compile`. That is the only CLI mode which expands the
// manifest's source GLOBS and runs its `preBuildScripts`/`postBuildScripts`
// hooks, so it is the only way the corpus can witness a build script that
// GENERATES the source it then compiles. Three things it changes, all handled
// explicitly below: the project manifest's own `targets[]` is the build
// authority (a corpus `spec` is a cross-checked MIRROR), the artifact lands
// under a per-format subdirectory, and the COMPILE — not just the run — must
// happen with `outDir` as the working directory. Every one of the three is
// implemented identically in the in-process sibling.

#include "arm_verdict_ledger.hpp"
#include "repo_root.hpp"
#include "run_binary.hpp"
#include "stage_tree.hpp"  // recursive corpus staging — ONE copy, shared with
                           // tests/examples/examples_runner.cpp

#include <nlohmann/json.hpp>

#include <algorithm>  // std::sort (do not rely on a transitive include)
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>  // std::memcmp (the arm-vs-baseline artifact comparison)
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>  // std::istreambuf_iterator (do not rely on a transitive include)
#include <optional>  // std::optional (per-target exitCode override)
#include <stdexcept>     // std::runtime_error (scratch-root setup fails loud)
#include <string>
#include <system_error>  // std::error_code (do not rely on a transitive include)
#include <utility>       // std::pair (kept-root prune list)
#include <vector>

// The per-run scratch root seeds its unique name with the pid.
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

// D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT: `currentHostOs`, `currentHostArch`,
// `specTargetArch` and `findOnPath` used to be defined HERE and, byte-
// identically, in `tests/examples/examples_runner.cpp`. Both harnesses now use
// the one copy in `arm_verdict_ledger.hpp`, so they cannot disagree about what
// host they are on or whether a declared emulator exists.
using ::dss::test_support::ArmVerdict;
using ::dss::test_support::ArmVerdictLedger;
using ::dss::test_support::armVerdictName;
using ::dss::test_support::currentHostArch;
using ::dss::test_support::currentHostOs;
using ::dss::test_support::DeclaredArm;
using ::dss::test_support::findOnPath;
using ::dss::test_support::HostBindingCandidate;
using ::dss::test_support::kNoBoundTarget;
using ::dss::test_support::qemuSysrootHint;
using ::dss::test_support::selectBoundTargetIndex;
using ::dss::test_support::specFormatName;
using ::dss::test_support::specTargetArch;
// …and from `stage_tree.hpp`, which is the SAME header the in-process sibling
// includes. Named one-by-one rather than pulled in with a using-DIRECTIVE so
// that a name appearing here is a name this file deliberately took.
using ::dss::test_support::stageExampleTree;
using ::dss::test_support::stageExampleTreeSelfTest;

int passes  = 0;
int failures = 0;

// Every DECLARED target arm of every manifest lands here with a reason, so the
// Results line can state what was VERIFIED rather than only what passed. Global
// alongside `passes`/`failures` for exactly the same reason they are: this
// runner is one process, one corpus walk, one summary.
ArmVerdictLedger armLedger;

// Flattened (target arm × runOn OS) declarations for the corpus-wide emulator
// lint. Accumulated during the same walk rather than by a second pass, so the
// lint can never disagree with what the run actually saw.
std::vector<DeclaredArm> declaredArms;

// How many `expected.json` manifests the corpus walk actually read. Kept
// SEPARATE from `declaredArms.size()` so [Test 4]'s non-vacuity guard can tell
// "the corpus root produced no manifest" apart from "manifests were read but
// not one of them declared a runOn host" — the two distinct diagnostics the
// in-process twin gives at tests/examples/examples_runner.cpp:1227 and :1230.
// Either state makes the lint vacuous; they have different causes and
// different fixes, so collapsing them would cost the reader the fix.
std::size_t manifestsWalked = 0;

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

// ── Filesystem answers that cannot terminate the run ───────────
//
// D-TEST-INTEGRATED-CORPUS-WALK-THROWS-UNCAUGHT: the examples-corpus path uses the `error_code` overload of every `std::filesystem` call, because the throwing forms here sit OUTSIDE any `try` and end the process via `std::terminate`.
//
// What that cost was not an unhandled corner: `libc++abi: terminating` is the
// WHOLE output. No `[FAIL]` line, no example name, no path, no Results line —
// an aggregate test that reports nothing about what it was doing when it died.
// The scratch-root section below was hardened for the same reason (TF-C98);
// this is the other half of the same file.
//
// The sharpest part, and why this is not tidying: the artifact checks are what
// a RED run reaches. A genuinely failing example could terminate the runner
// instead of reporting itself, so the harness was least informative at exactly
// the moment it mattered most.
//
// A negative answer carries its own REASON because the reasons are not
// interchangeable — "absent", "0 bytes" and "cannot be read" send a reader to
// three different places, and printing the wrong one would trade a crash for a
// confident wrong diagnostic, which is the worse of the two.
struct FsAnswer {
    bool        ok = false;
    std::string why;  // empty iff ok
};

// `exists` with the failure kept SEPARATE from the answer. An unreadable path
// is not an absent one — ELOOP on a self-referential symlink, EACCES on a
// locked parent — and only the `error_code` overload can tell them apart; the
// throwing form answers one of those two questions by aborting the run.
[[nodiscard]] FsAnswer fileExists(fs::path const& p) {
    std::error_code ec;
    bool const present = fs::exists(p, ec);
    if (ec) {
        return {false,
                "cannot stat '" + p.generic_string() + "': " + ec.message()};
    }
    // Plain absence says "absent" and NOT the path: both callers already name
    // the path in the check's description, and the commonest red line in the
    // corpus should not print it twice. An ERROR keeps its path, because that
    // branch carries a cause worth being unambiguous about.
    return {present, present ? std::string{} : "absent"};
}

// NOT `file_size(p)`. Its no-argument form THROWS for a file that is missing
// (ENOENT — e.g. an artifact deleted under a concurrent run, the exact shape
// D-TEST-INTEGRATED-FIXED-TEMP-PATH-COLLIDES produced) or that is not a regular
// file, and both are RED-run states. An error is a FAILED check reported with
// its own cause: calling it "empty" would replace a crash with a diagnostic
// that sends the reader to look at a file that is not even there.
[[nodiscard]] FsAnswer fileNonEmpty(fs::path const& p) {
    std::error_code ec;
    auto const size = fs::file_size(p, ec);
    if (ec) {
        return {false, "cannot read the size of '" + p.generic_string()
                           + "': " + ec.message()};
    }
    return {size > 0u, size > 0u ? std::string{} : "0 bytes"};
}

// `create_directories`, reported rather than thrown. Deliberately SILENT on
// success: a new `[PASS]` per example would change this runner's pinned pass
// count, and a behaviour change wearing error handling's clothes is not what
// this defect asked for.
[[nodiscard]] FsAnswer madeDirectory(fs::path const& d) {
    std::error_code ec;
    // A false return with `ec` clear only means "already there" — the error
    // code is the answer, not the return value.
    fs::create_directories(d, ec);
    if (ec) {
        return {false,
                "cannot create '" + d.generic_string() + "': " + ec.message()};
    }
    return {true, {}};
}

// The corpus neighbour-staging primitive now lives ONCE, in
// `tests/test_support/stage_tree.hpp`, and this runner and its in-process
// sibling both include it (see the ★★ hoist note at the top of that header).
// It used to sit here duplicated VERBATIM — ✔MEASURED 14,765 bytes between the
// twin markers, `cmp`-identical against the sibling's copy.
// The pin that a copy never comes BACK is
// `ExamplesCorpusLint.StagingPrimitiveLivesOnlyInTheSharedHeader`, which reads
// THIS file off disk from the in-process sibling's gtest binary.

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

// D-TEST-INTEGRATED-RUNNER-HAS-NO-OPTIMIZATION-ARM-CONCEPT +
// D-TEST-INTEGRATED-RUNNER-IGNORES-THE-RELEASE-ARM-AND-STDOUT-PINS: one declared
// OPTIMIZED arm. Mirrors the in-process examples_runner's `OptimizedArm` field
// for field, because both runners must ACCEPT and REJECT the same manifests.
//
// ★ WHAT THIS RUNNER CAN AND CANNOT DRIVE, and why the split is a property of
// the CLI rather than a shortcut. An arm declares EXACTLY ONE OF:
//   * `shippedPipeline`: the NAME of a shipped pipeline config. The CLI's
//     `--config=<name>` selects a shipped pipeline BY NAME
//     (src/program/cli_args.cpp → `CompileOptions::config` →
//     `resolvePipelineName` → `loadShippedPipeline`), so this form maps ONTO the
//     CLI surface exactly — and the name is threaded THROUGH from the manifest,
//     never spelled in this file, so the runner learns a new shipped pipeline the
//     day one ships. A name the CLI does not recognise fails LOUD in the
//     subprocess (`--config: '<x>' is not a recognized configuration`) and the
//     arm is Poisoned with that rc — it is never silently downgraded to the
//     default build.
//   * `passes`: an inline PassId-name array. The shipped CLI has NO flag that
//     expresses one, and inventing one would publish the optimizer's internal
//     pass vocabulary as a user-facing surface purely to satisfy a harness. Such
//     an arm is therefore ledgered `NotSelectedByRunner` with a named reason and
//     COUNTED in the summary — it is witnessed by the in-process sibling, which
//     drives `CompileOptions::pipelineOverride` directly. It is NOT skipped
//     silently: a skip nobody can see is the defect this row exists to close.
struct OptimizedArm {
    std::string                 label;   // diagnostic-rendering name (free-form)
    std::vector<std::string>    passes;  // PassId names (inline-array form)
    bool                        hasPasses = false;  // `passes` key present
    std::optional<std::string>  shippedPipeline;    // shipped-config form
    // D-CSUBSET-INLINE-FUNCTION-NO-EXTERNAL-DEFINITION-EMITTED: THIS arm's own
    // expected exit code, legal ONLY inside an example that declares
    // `optimizationObservable`. Absent ⇒ the arm must match the baseline.
    std::optional<std::int64_t> exitCode;
    // The arm's OPT-IN to a hard red when its image comes out byte-identical to
    // the baseline's. Mirrors the in-process sibling's field of the same name:
    // an arm whose job is the optimizer x feature composition witnesses nothing
    // if the pipeline transformed nothing, and only the MANIFEST knows which
    // arms have that job -- 7 of the corpus's arms are `asm/` examples where a
    // pipeline has nothing to transform BY CONSTRUCTION.
    bool                        mustDifferFromBaseline = false;
};

// The example-level EXEMPTION that alone makes a per-arm `exitCode` legal. Its
// `clause` names the standard text under which two conforming compilations may
// legitimately differ (e.g. "C99 6.7.4p7"). Mirrors the in-process runner —
// including the three load-time refusals in `validateOptimizationObservable`,
// because a manifest one runner accepts and the other rejects is the same
// silent-harness-bug class as a capability landing on one runner only.
struct OptimizationObservable {
    std::string clause;
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
    // D-TEST-INTEGRATED-RUNNER-IGNORES-THE-RELEASE-ARM-AND-STDOUT-PINS: optional
    // PER-TARGET expected stdout. Overrides the manifest-level `expectedStdout`
    // for THIS target only — needed where one source's output is platform-
    // divergent (Windows msvcrt CRLF translation makes "hello\r\n" of the same
    // program's "hello\n" elsewhere). Mirrors the in-process examples_runner.
    std::optional<std::string> expectedStdoutOverride;
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
    // D-EXAMPLES-RUNNER-PROJECT-MANIFEST: PROJECT MODE. Present ⇒ this example is
    // built by `dss-code-prime --project <file>` from the named
    // `.dss-project.json` (relative to the example dir) instead of by
    // `--compile <sources>`. MUTUALLY EXCLUSIVE with `source`/`sources`; see the
    // in-process examples_runner's copy of this field for the full rationale,
    // including WHY the key is top-level and why the per-target `spec` becomes a
    // cross-checked MIRROR rather than a driver (`Program::compileProject` takes
    // no targets argument — the project manifest's own `targets[]` is authority).
    std::optional<std::string> project;
    // Multi-CU (CU6): "sources":[...] makes each file its OWN CompilationUnit; the CLI's
    // `--compile a.c b.c` links them into one image (gcc/clang semantics — separate TUs,
    // the linker resolves cross-file references, LK11). The single "source":"x.c" form is
    // one file. `sources` is the canonical file list for either form (single → a 1-element
    // list); `multiCu` records which spelling the manifest used (diagnostic only — the CLI
    // derives multi-vs-single from the file count, not from this flag).
    std::vector<std::string>   sources;
    bool                       multiCu = false;
    std::int64_t               exitCode = 0;
    // D-TEST-INTEGRATED-RUNNER-IGNORES-THE-RELEASE-ARM-AND-STDOUT-PINS: optional
    // captured-stdout pin. Present ⇒ the child's stdout+stderr are routed through
    // a pipe and the drained bytes must equal this string EXACTLY. Empty-string
    // is a VALID pin (asserts the binary printed nothing); the `has_value()` gate
    // distinguishes "no pin" from "pin to empty". Mirrors the in-process runner,
    // including the capture-only-when-pinned rule (a pipe changes the child's
    // stdio, so it is not routed for examples that never asked).
    std::optional<std::string> expectedStdout;
    std::vector<ExampleTarget> targets;
    // D-TEST-INTEGRATED-RUNNER-HAS-NO-OPTIMIZATION-ARM-CONCEPT: the declared
    // OPTIMIZED arms. Each one this runner can express is a SECOND build+run of
    // the same source through `--config=<shippedPipeline>`, differentially
    // compared against the baseline arm.
    std::vector<OptimizedArm>  optimizedPipelines;
    // Present ⇒ this example is EXEMPT from the mechanical "optimized arm equals
    // baseline" half of the differential contract, because a named standard
    // clause makes the difference conforming. Its presence is the ONLY thing that
    // lets an arm declare its own `exitCode`.
    std::optional<OptimizationObservable> optimizationObservable;
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

// ★★★ THE THREE LOAD-TIME REFUSALS THAT MAKE THE `optimizationObservable`
// CARVE-OUT STRUCTURAL RATHER THAN A PROMISE. A byte-for-byte behavioural mirror
// of the in-process runner's `validateOptimizationObservable`, spelled with this
// runner's `std::cerr` + `return false` convention instead of gtest's
// `ADD_FAILURE`.
//
// ⚠ THE MIRROR IS THE POINT, not a convenience. These are REFUSALS: a manifest
// one runner rejects and the other accepts is exactly the silent harness bug
// [[D-TEST-INTEGRATED-RUNNER-HAS-NO-OPTIMIZATION-ARM-CONCEPT]] closes — and it
// is the worse direction of it, because the accepting runner would report a
// green verdict over a manifest the project's own rules forbid.
//
//   R1  an arm declares `exitCode` while the example declares NO
//       `optimizationObservable` — the load-bearing one: without it, any arm
//       that started failing could be silenced by writing down the number it
//       happens to produce, which is precisely the optimizer bug the
//       differential arm exists to catch.
//   R2  the example declares `optimizationObservable` and no arm declares a
//       differing `exitCode` — an exemption that exempts nothing.
//   R3  an arm declares an `exitCode` EQUAL to the baseline it would otherwise
//       have been compared against (manifest-level AND every per-target
//       override) — a declared "difference" that is not one.
[[nodiscard]] bool validateOptimizationObservable(ExampleManifest const& m,
                                                  fs::path const&        path) {
    bool anyDiffering = false;
    for (auto const& arm : m.optimizedPipelines) {
        if (!arm.exitCode.has_value()) continue;
        if (!m.optimizationObservable.has_value()) {  // R1
            std::cerr << "  optimizedPipelines arm '" << arm.label
                      << "' declares its own 'exitCode' but the example declares"
                         " no 'optimizationObservable'. An optimized arm must"
                         " produce the baseline's exit code; declaring a"
                         " different one is legal ONLY where a named standard"
                         " clause makes both results conforming: "
                      << path.generic_string() << "\n";
            return false;
        }
        if (*arm.exitCode == m.exitCode) {  // R3, manifest-level
            std::cerr << "  optimizedPipelines arm '" << arm.label
                      << "' declares exitCode " << *arm.exitCode
                      << ", which EQUALS the manifest's baseline exitCode — a"
                         " declared difference that is not one. Drop the key: an"
                         " arm with no 'exitCode' is already required to match"
                         " the baseline, and that is the stronger assertion: "
                      << path.generic_string() << "\n";
            return false;
        }
        for (auto const& t : m.targets) {  // R3, per-target overrides
            if (t.exitCodeOverride.has_value()
                && *arm.exitCode == *t.exitCodeOverride) {
                std::cerr << "  optimizedPipelines arm '" << arm.label
                          << "' declares exitCode " << *arm.exitCode
                          << ", which EQUALS target '" << t.spec
                          << "'s exitCode override — so on that target the arm"
                             " asserts no difference at all while claiming an"
                             " exemption: " << path.generic_string() << "\n";
                return false;
            }
        }
        anyDiffering = true;
    }
    if (m.optimizationObservable.has_value() && !anyDiffering) {  // R2
        std::cerr << "  manifest declares 'optimizationObservable' (clause \""
                  << m.optimizationObservable->clause
                  << "\") but no optimizedPipelines arm declares a differing"
                     " 'exitCode'. An exemption that exempts nothing weakens the"
                     " corpus-wide count of examples allowed to diverge; remove"
                     " it: " << path.generic_string() << "\n";
        return false;
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

    // ── WHAT DOES THIS EXAMPLE COMPILE? EXACTLY ONE SPELLING ANSWERS IT ─────
    //
    // `project` (a .dss-project.json build) / `sources` (multi-CU) / `source`
    // (single file) — declaring two of them is a manifest DEFECT, not a
    // precedence question. `sources` USED TO "take precedence over" `source`,
    // i.e. silently drop one of them; MEASURED, zero of the 580 corpus
    // manifests declare both, so the rule protected nothing and stood ready to
    // swallow a rename typo. Mirrors the in-process examples_runner exactly —
    // both runners must accept and REJECT the same manifests.
    bool const hasSource  = j.contains("source");
    bool const hasSources = j.contains("sources");
    if (j.contains("project")) {
        if (!j.at("project").is_string()
            || j.at("project").get<std::string>().empty()) {
            std::cerr << "  'project' must be a NON-EMPTY string naming a "
                         ".dss-project.json relative to the example dir, in "
                      << path.generic_string() << "\n";
            return false;
        }
        if (hasSource || hasSources) {
            std::cerr << "  manifest declares BOTH 'project' and '"
                      << (hasSources ? "sources" : "source")
                      << "'. A project build's inputs come from the "
                         ".dss-project.json's own 'sources' (which a "
                         "preBuildScripts hook may GENERATE), so the "
                         "expected.json list would be silently ignored — delete "
                         "one: " << path.generic_string() << "\n";
            return false;
        }
        out.project = j.at("project").get<std::string>();
    }
    if (hasSource && hasSources) {
        std::cerr << "  manifest declares BOTH 'source' and 'sources' — one "
                     "names a single CU, the other a multi-CU list, and "
                     "honoring either would silently discard the other. Delete "
                     "one: " << path.generic_string() << "\n";
        return false;
    }
    if (hasSources) {
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
    } else if (hasSource) {
        auto single = j.value("source", std::string{});
        if (!j.at("source").is_string() || single.empty()) {
            std::cerr << "  'source' must be a non-empty string in "
                      << path.generic_string() << "\n";
            return false;
        }
        out.sources = {std::move(single)};
    } else if (!out.project.has_value()) {
        // RELAXED, not deleted: a manifest naming NONE of the three still fails
        // exactly as loudly. Only project mode is exempt, and only because its
        // inputs live in the other file.
        std::cerr << "  manifest requires 'source' (single CU), 'sources' "
                     "(multi-CU), or 'project' (a .dss-project.json build): "
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
    // PROJECT MODE has no expect-error branch, and saying so LOUDLY is the
    // point: `runErrorExampleViaCli` builds a `--compile <sources>` command line
    // a project manifest never populates. Rejected in the PARSER, in BOTH
    // runners, so the unsupported shape is named where the author can fix it.
    if (out.project.has_value() && !out.expectDiagnostics.empty()) {
        std::cerr << "  manifest declares BOTH 'project' and "
                     "'expectDiagnostics'. The expect-error path compiles a "
                     "single named source so a diagnostic's position is "
                     "unambiguous; a project build has no such source. "
                     "Implement the project-mode expect-error path in BOTH "
                     "runners before declaring this pair: "
                  << path.generic_string() << "\n";
        return false;
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
    // D-TEST-INTEGRATED-RUNNER-IGNORES-THE-RELEASE-ARM-AND-STDOUT-PINS: the
    // manifest-level stdout pin. Same shape and same rules as the in-process
    // runner's — an empty string is a real pin, not "absent".
    if (j.contains("expectedStdout")) {
        if (!j.at("expectedStdout").is_string()) {
            std::cerr << "  'expectedStdout' must be a string in "
                      << path.generic_string() << "\n";
            return false;
        }
        out.expectedStdout = j.at("expectedStdout").get<std::string>();
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
        // D-TEST-INTEGRATED-RUNNER-IGNORES-THE-RELEASE-ARM-AND-STDOUT-PINS:
        // optional per-target stdout override (mirrors the in-process runner).
        if (t.contains("expectedStdout")) {
            if (!t.at("expectedStdout").is_string()) {
                std::cerr << "  target 'expectedStdout' must be a string in "
                          << path.generic_string() << "\n";
                return false;
            }
            et.expectedStdoutOverride = t.at("expectedStdout").get<std::string>();
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
    // D-TEST-INTEGRATED-RUNNER-HAS-NO-OPTIMIZATION-ARM-CONCEPT: the OPTIMIZED
    // arms. Parsed with the in-process runner's exact rules — including the
    // CLOSED per-arm key set, which is what stops a manifest from declaring an
    // expectation no runner reads.
    if (j.contains("optimizedPipelines")) {
        if (!j.at("optimizedPipelines").is_array()) {
            std::cerr << "  'optimizedPipelines' must be an array in "
                      << path.generic_string() << "\n";
            return false;
        }
        for (auto const& arm : j.at("optimizedPipelines")) {
            if (!arm.is_object()
                || !arm.contains("label") || !arm.at("label").is_string()) {
                std::cerr << "  each optimizedPipelines entry needs string"
                             " 'label' + exactly one of 'passes' /"
                             " 'shippedPipeline' in "
                          << path.generic_string() << "\n";
                return false;
            }
            OptimizedArm oa;
            oa.label = arm.at("label").get<std::string>();
            if (arm.contains("passes")) {
                if (!arm.at("passes").is_array()) {
                    std::cerr << "  optimizedPipelines 'passes' must be an array"
                                 " in " << path.generic_string() << "\n";
                    return false;
                }
                oa.hasPasses = true;
                for (auto const& p : arm.at("passes")) {
                    if (!p.is_string()) {
                        std::cerr << "  optimizedPipelines.passes entries must be"
                                     " strings in " << path.generic_string()
                                  << "\n";
                        return false;
                    }
                    oa.passes.push_back(p.get<std::string>());
                }
            }
            if (arm.contains("shippedPipeline")) {
                if (!arm.at("shippedPipeline").is_string()
                    || arm.at("shippedPipeline").get<std::string>().empty()) {
                    std::cerr << "  optimizedPipelines 'shippedPipeline' must be"
                                 " a NON-EMPTY string naming a shipped pipeline"
                                 " config (it is threaded verbatim into the CLI's"
                                 " --config=<name>) in "
                              << path.generic_string() << "\n";
                    return false;
                }
                oa.shippedPipeline =
                    arm.at("shippedPipeline").get<std::string>();
            }
            // EXACTLY-ONE-OF, enforced at LOAD rather than at use. The in-process
            // sibling enforces the same rule inside `buildPipeline`; hoisting it
            // to the parse here is deliberate and NOT a divergence — this runner
            // has no pipeline object to build, so `buildPipeline` has no twin,
            // and a manifest that declares both (or neither) must still be
            // REJECTED by both runners rather than accepted by one of them.
            if (oa.hasPasses == oa.shippedPipeline.has_value()) {
                std::cerr << "  optimizedPipelines arm '" << oa.label
                          << "' must declare EXACTLY ONE OF 'passes' or"
                             " 'shippedPipeline' (got "
                          << (oa.hasPasses ? "both" : "neither") << ") in "
                          << path.generic_string() << "\n";
                return false;
            }
            if (arm.contains("exitCode")) {
                if (!arm.at("exitCode").is_number_integer()) {
                    std::cerr << "  optimizedPipelines 'exitCode' must be an"
                                 " integer in " << path.generic_string() << "\n";
                    return false;
                }
                oa.exitCode = arm.at("exitCode").get<std::int64_t>();
            }
            if (arm.contains("mustDifferFromBaseline")) {
                if (!arm.at("mustDifferFromBaseline").is_boolean()) {
                    std::cerr << "  optimizedPipelines arm '" << oa.label
                              << "' key 'mustDifferFromBaseline' must be a"
                                 " boolean in " << path.generic_string() << "\n";
                    return false;
                }
                oa.mustDifferFromBaseline =
                    arm.at("mustDifferFromBaseline").get<bool>();
            }
            // The CLOSED per-arm key set, mirroring the sibling exactly. An
            // expectation the runner does not read is an assertion that never
            // fires, so an unknown key is a LOAD ERROR naming it.
            for (auto const& [k, unusedV] : arm.items()) {
                (void)unusedV;
                if (k == "label" || k == "passes" || k == "shippedPipeline"
                    || k == "exitCode" || k == "mustDifferFromBaseline"
                    || k.starts_with("$")) {
                    continue;
                }
                std::cerr << "  optimizedPipelines arm '" << oa.label
                          << "' declares unknown key '" << k
                          << "' — the runner reads label / passes /"
                             " shippedPipeline / exitCode (plus $comment keys)."
                             " An expectation the runner does not read is an"
                             " assertion that never fires: "
                          << path.generic_string() << "\n";
                return false;
            }
            out.optimizedPipelines.push_back(std::move(oa));
        }
    }
    // Parsed AFTER the arms so the refusals below can see both halves.
    if (j.contains("optimizationObservable")) {
        auto const& oo = j.at("optimizationObservable");
        if (!oo.is_object() || !oo.contains("clause")
            || !oo.at("clause").is_string()
            || oo.at("clause").get<std::string>().empty()) {
            std::cerr << "  'optimizationObservable' must be an object with a"
                         " non-empty string 'clause' naming the standard clause"
                         " that makes the optimized arm's different result"
                         " conforming (e.g. \"C99 6.7.4p7\") in "
                      << path.generic_string() << "\n";
            return false;
        }
        out.optimizationObservable =
            OptimizationObservable{oo.at("clause").get<std::string>()};
    }
    if (!validateOptimizationObservable(out, path)) return false;
    return true;
}

// ── PROJECT MODE support (mirrors tests/examples/examples_runner.cpp) ───────

// The FORMAT half of a manifest spec ("x86_64:pe64-x86_64-windows-exec" →
// "pe64-x86_64-windows-exec") — the subdirectory a PROJECT build routes its
// artifact into, because `Program::compileProject` forces
// `setPerFormatOutputSubdir(true)` (src/program/program.cpp:1760) even for a
// single-target build. Empty ⇒ the spec has no ':' and the caller fails loud
// rather than composing a wrong path.
//
// `specFormatName` is the shared `dss::test_support` helper beside its twin
// `specTargetArch` (arm_verdict_ledger.hpp), imported with the sibling
// using-declarations above. Both runners briefly carried byte-identical local
// copies when project mode landed; folding them into the shared header keeps
// one spec-splitting vocabulary, which is what that header is for.

// The FACTS this harness needs out of a `.dss-project.json`. NOT a second
// `parseProjectConfig` — the DRIVER owns that parse and fails loud on every
// structural error in it. Two questions, each with a harness-side reason:
// `targets[]`/`language` are what the expected.json entries MIRROR (and
// `compileProject` takes no targets argument, so the mirror must be checked
// somewhere), and each APPLICABLE build script's argv[0] is what decides
// `SkippedBuildInputMissing` rather than letting an absent interpreter surface
// as a source glob that matched nothing. Mirrors the in-process twin field for
// field so both runners accept and reject the same project manifests.
struct ProjectFacts {
    std::string              language;
    std::vector<std::string> targets;
    std::vector<std::string> applicableScriptPrograms;  // argv[0]s for THIS host
};

[[nodiscard]] bool readProjectFacts(fs::path const& projectPath,
                                    ProjectFacts&   out) {
    std::ifstream in(projectPath);
    if (!in) {
        std::cerr << "  cannot open project manifest "
                  << projectPath.generic_string() << "\n";
        return false;
    }
    nlohmann::json j;
    try { in >> j; }
    catch (std::exception const& e) {
        std::cerr << "  JSON parse failed for project manifest "
                  << projectPath.generic_string() << ": " << e.what() << "\n";
        return false;
    }
    if (!j.contains("language") || !j.at("language").is_string()) {
        std::cerr << "  project manifest needs a string 'language': "
                  << projectPath.generic_string() << "\n";
        return false;
    }
    out.language = j.at("language").get<std::string>();
    if (!j.contains("targets") || !j.at("targets").is_array()
        || j.at("targets").empty()) {
        std::cerr << "  project manifest needs a non-empty 'targets' array: "
                  << projectPath.generic_string() << "\n";
        return false;
    }
    for (auto const& t : j.at("targets")) {
        if (!t.is_string()) {
            std::cerr << "  project manifest 'targets' entries must be strings: "
                      << projectPath.generic_string() << "\n";
            return false;
        }
        out.targets.push_back(t.get<std::string>());
    }
    // BOTH hook arrays: a post-build script's interpreter is as absent as a
    // pre-build one's, and both are build inputs the manifest asked for.
    auto const host = currentHostOs();
    for (char const* key : {"preBuildScripts", "postBuildScripts"}) {
        if (!j.contains(key)) continue;
        if (!j.at(key).is_array()) {
            std::cerr << "  project manifest '" << key << "' must be an array: "
                      << projectPath.generic_string() << "\n";
            return false;
        }
        for (auto const& e : j.at(key)) {
            if (!e.is_object() || !e.contains("run") || !e.at("run").is_array()
                || e.at("run").empty() || !e.at("run").at(0).is_string()) {
                std::cerr << "  project manifest '" << key << "' entries need a "
                             "non-empty string array 'run': "
                          << projectPath.generic_string() << "\n";
                return false;
            }
            // ABSENT `runOn` ⇒ EVERY host (the driver's own default, see
            // src/program/build_scripts.cpp `appliesToHost`). Reading absent as
            // "matches nothing" would silently exempt every unfiltered hook.
            bool applies = true;
            if (e.contains("runOn")) {
                if (!e.at("runOn").is_array()) {
                    std::cerr << "  project manifest '" << key
                              << "' entry 'runOn' must be an array: "
                              << projectPath.generic_string() << "\n";
                    return false;
                }
                applies = false;
                for (auto const& o : e.at("runOn")) {
                    if (o.is_string() && o.get<std::string>() == host) {
                        applies = true;
                    }
                }
            }
            if (applies) {
                out.applicableScriptPrograms.push_back(
                    e.at("run").at(0).get<std::string>());
            }
        }
    }
    return true;
}

// Is a build script's argv[0] present on THIS machine? Mirrors the contract of
// the driver's `dss::substrate::resolveExecutableOnPath` closely enough for a
// PRESENCE probe: a name carrying a directory separator is taken AS A PATH (no
// search), anything else is a PATH lookup through the SAME `findOnPath` the
// emulator gate uses — so the two skip classes cannot disagree about what
// "missing" means. The driver still spawns and still emits
// `D_ScriptSpawnFailed` if this probe is wrong.
[[nodiscard]] bool buildScriptProgramPresent(std::string const& argv0) {
    if (argv0.find('/') != std::string::npos
        || argv0.find('\\') != std::string::npos) {
        std::error_code ec;
        bool const there = fs::exists(fs::path{argv0}, ec);
        return there && !ec;
    }
    return !findOnPath(argv0).empty();
}

// ── Scoped working-directory change ────────────────────────────────────────
//
// HOISTED out of `runSelectedTargetViaCli`, where it used to be declared inline
// immediately before the RUN. It now has TWO users in that function — the
// project-mode COMPILE and the run — and a type defined at the first use site
// cannot serve the earlier one.
//
// SAFE HERE and stated so the next reader can check it: this runner walks
// examples SEQUENTIALLY in one thread, so there is no concurrent arm whose
// relative-path resolution a process-global chdir could disturb. The guard
// RESTORES the previous directory because the runner resolves relative paths of
// its own (the compiler path, the corpus root) between examples.
struct CwdGuard {
    fs::path saved;
    bool     ok = false;
    explicit CwdGuard(fs::path const& to) {
        std::error_code ec;
        saved = fs::current_path(ec);
        if (ec) return;
        fs::current_path(to, ec);
        ok = !ec;
    }
    ~CwdGuard() {
        if (!ok || saved.empty()) return;
        std::error_code ec;
        fs::current_path(saved, ec);
    }
    CwdGuard(CwdGuard const&) = delete;
    CwdGuard& operator=(CwdGuard const&) = delete;
};

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
    // D-TEST-INTEGRATED-CORPUS-WALK-THROWS-UNCAUGHT: `error_code` overloads — the throwing `exists`/`file_size` pair terminated the run instead of failing this check.
    // The three causes stay distinguishable in the ONE check this has always
    // emitted (a second check here would change the pinned pass count).
    std::string depWhy;
    if (depRc != 0) {
        depWhy = "rc=" + std::to_string(depRc);
    } else if (auto const present = fileExists(depArtifact); !present.ok) {
        depWhy = present.why;
    } else if (auto const nonEmpty = fileNonEmpty(depArtifact); !nonEmpty.ok) {
        depWhy = nonEmpty.why;
    }
    bool const depOk = depWhy.empty();
    check(exampleName + ": dependsOn library " + dep.spec + " built ("
          + depArtifact.generic_string() + ", buildlog: "
          + depLog.generic_string() + ")", depOk, depWhy);
    if (!depOk) return std::nullopt;
    return depArtifact;
}

// D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT: the verdict this runner reached
// for the ONE target it bound. Returned rather than recorded at each of the ten
// early exits below, so the ledger write happens in exactly one place and a
// future exit path cannot forget it.
struct CliArmOutcome {
    ArmVerdict  verdict = ArmVerdict::Poisoned;
    std::string detail;
    // Populated only when `verdict == Ran`. The caller owns the differential
    // comparison, exactly as the in-process sibling's `runOneTarget` does — the
    // helper produces observations, the caller asserts over them.
    int         exitCode = 0;
    std::string capturedStdout;
    // Populated as soon as the COMPILE produced an artifact, even when the run
    // was skipped: the artifact-byte instrument below compares an optimized
    // arm's image with the baseline's, and that comparison is host-independent.
    fs::path    artifactPath;
    bool        compiled = false;
};

// ── THE INSTRUMENT THAT KEEPS THE OPTIMIZED ARM FROM GOING INERT ────────────
//
// ★★★ WHY A COUNT OF ARMS IS NOT ENOUGH, and this is the whole reason this
// section exists. If a future edit drops the `--config=<name>` from the arm's
// command line, every optimized arm still COMPILES, still RUNS, and still
// produces the baseline's exit code — because it now IS the baseline build. The
// differential comparison would pass, the ledger would say `Ran`, and the arm
// would be witnessing nothing at all. That is the exact "masked effectiveness"
// shape the bar names: a guard that is present, green, and asserting nothing.
//
// ⇒ the runner asserts a POSITIVE, SELF-WITNESSING property: at least one
// optimized arm's ARTIFACT must differ BYTE-WISE from its baseline's. It cannot
// be satisfied by a build that never received the flag.
//
// ✔MEASURED 2026-08-17 (the control experiment that makes this sound, run before
// the instrument was written): compiling `examples/c-subset/array_decay/main.c`
// for `x86_64:pe64-x86_64-windows-exec` TWICE into two DIFFERENT output
// directories produced BYTE-IDENTICAL images, and the same source at
// `--config=release` produced a DIFFERENT one (first difference at byte 401).
// So the compiler is deterministic across output paths — an artifact difference
// is a signal about the PIPELINE and not about the directory it landed in, which
// is the premise the whole instrument rests on. Had DSS embedded its output path
// or a timestamp, this guard would have been green in BOTH directions and worse
// than useless.
std::size_t optimizedArmsDeclaredOnBoundTarget = 0;
std::size_t optimizedArmsRunViaConfigFlag      = 0;
std::size_t optimizedArmsArtifactDiffered      = 0;
std::size_t optimizedArmsNotExpressibleOnCli   = 0;
// The stdout half needs its own non-vacuity witness, and for a reason specific
// to it: an EMPTY pin ("this program prints nothing") is satisfied by a capture
// pipe that was never routed at all. ✔MEASURED 2026-08-17 over the shipped
// corpus — of the effective pins on a bound target, windows 150 of which 26 are
// NON-EMPTY, linux 125 / 27, darwin 152 / 27 — so every host has real bytes to
// compare, and only a NON-EMPTY pin proves the drain actually happened.
std::size_t stdoutPinsAsserted         = 0;
std::size_t stdoutPinsAssertedNonEmpty = 0;

// Byte-compare two files. A negative answer carries its REASON for the same
// reason `fileExists`/`fileNonEmpty` do: "same size, different bytes" and
// "cannot read one of them" send a reader to different places, and the
// instrument above must never count an unreadable pair as a difference.
[[nodiscard]] FsAnswer filesDiffer(fs::path const& a, fs::path const& b) {
    std::error_code ec;
    auto const sizeA = fs::file_size(a, ec);
    if (ec) return {false, "cannot size '" + a.generic_string() + "'"};
    auto const sizeB = fs::file_size(b, ec);
    if (ec) return {false, "cannot size '" + b.generic_string() + "'"};
    if (sizeA != sizeB) return {true, {}};
    std::ifstream fa(a.string(), std::ios::binary);
    std::ifstream fb(b.string(), std::ios::binary);
    if (!fa || !fb) return {false, "cannot open both images for comparison"};
    constexpr std::size_t kChunk = 64u * 1024u;
    std::vector<char> bufA(kChunk);
    std::vector<char> bufB(kChunk);
    while (fa && fb) {
        fa.read(bufA.data(), static_cast<std::streamsize>(kChunk));
        fb.read(bufB.data(), static_cast<std::streamsize>(kChunk));
        auto const gotA = fa.gcount();
        auto const gotB = fb.gcount();
        if (gotA != gotB) return {true, {}};
        if (gotA == 0) break;
        if (std::memcmp(bufA.data(), bufB.data(),
                        static_cast<std::size_t>(gotA)) != 0) {
            return {true, {}};
        }
    }
    return {false, "byte-identical"};
}

// Drive ONE ARM of one example's SELECTED target through the CLI subprocess path:
//   1. spawn `dss-code-prime --compile <src> --language <l> --target <spec> --output <outdir> [--config=<name>]`
//   2. check rc == 0
//   3. check artifact file exists at outdir/<artifact>
//   4. spawn artifact, capture exit code (and, when pinned, stdout) via run_binary.hpp
//
// It does NOT assert the exit code or the stdout. The CALLER owns those, because
// the differential contract is a relation BETWEEN arms and only the caller holds
// both sides — the same division of labour as the in-process sibling's
// `compileAndRunArm` / `runOneTarget` pair.
//
// `configName` EMPTY ⇒ no `--config` is passed at all, which is the BASELINE arm
// and is exactly what this runner did for its whole life before the optimized
// arms landed. That is deliberate rather than "debug spelled implicitly": the
// CLI's own default is a shipped behaviour worth exercising, and passing
// `--config=debug` here would silently stop testing it.
//
// All checks are strict — wrong values fail the test.
[[nodiscard]] CliArmOutcome compileAndRunArmViaCli(std::string const& compiler,
                      fs::path const&    exampleDir,
                      fs::path const&    outDir,
                      ExampleManifest const& m,
                      ExampleTarget const*   target,
                      std::string const&     exampleName,
                      std::string const&     armLabel,
                      std::string const&     configName,
                      bool                   captureStdout) {
    // Every `[FAIL]` line names the ARM it belongs to, so a red run cannot leave
    // a reader guessing which of two builds of the same source produced it. The
    // baseline keeps its historical bare-name spelling (no suffix) so existing
    // failure lines read exactly as before.
    auto const armName = (armLabel == "baseline")
                             ? exampleName
                             : exampleName + " [arm=" + armLabel + "]";
    // D-TEST-INTEGRATED-CORPUS-WALK-THROWS-UNCAUGHT: a full temp filesystem used to abort the whole run here; it is now this one example's [FAIL].
    if (auto const made = madeDirectory(outDir); !made.ok) {
        check(exampleName + ": output directory created", false, made.why);
        return {ArmVerdict::Poisoned, "output directory: " + made.why};
    }

    // ── Mirror the example dir's FILE NEIGHBORHOOD into `outDir` ────────────
    //
    // ★ THIS CLOSES A REAL ASYMMETRY BETWEEN THE TWO RUNNERS, not a cosmetic
    // one. The in-process sibling (tests/examples/examples_runner.cpp) already
    // mirrors every regular file except the manifest into its scratch dir and
    // then makes that dir the CWD; this runner did neither, so an example that
    // needs a file AT RUN TIME passed in-process and failed here. Measured on
    // exactly that: `examples/c-subset/environ_alias_object_identity` ships a
    // prebuilt gcc-built `.so` it dlopens as `./libdss_env_probe_<arch>.so`
    // (an OBJECT-IDENTITY property cannot be witnessed by one image, so the
    // example needs a second image DSS did not build) — green in-process,
    // exit 10 here, and exit 10 is that example's FAIL-CLOSED "the witness is
    // absent" code, which is the only reason it did not pass for the wrong
    // reason.
    // [[D-EXAMPLES-RUNNER-TWO-RUNNERS-MUST-AGREE]]: one runner enforcing while
    // its sibling shrugs is a SILENT harness bug of the same shape as a
    // `.ps1`/`.sh` pair where only one side is wrong.
    // CONTRACT, deliberately identical to the sibling's, and it is now the
    // RECURSIVE one: whole SUBDIRECTORIES cross with their relative subpaths
    // intact, and only the TOP-LEVEL manifest is excluded. It used to be the
    // immediate directory only, with a `continue` on every non-regular entry —
    // so an example whose dependency lives in `<example>/dep_module/` had that
    // directory dropped in silence and then died on a missing-file error
    // naming the manifest. The walk is `stageExampleTree` from the shared
    // `tests/test_support/stage_tree.hpp` — the SAME function the in-process
    // sibling calls, not a copy held equal to it — so the two runners cannot
    // stage different trees, which is the whole point of the ★ note above.
    if (std::string const err = stageExampleTree(exampleDir, outDir);
        !err.empty()) {
        check(exampleName + ": staged the example's neighbor files into "
                  + outDir.generic_string(),
              false, err);
        return {ArmVerdict::Poisoned, "neighbor staging: " + err};
    }

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
            compiler, dep, exampleDir, outDir, m.language, armName);
        if (!depArtifact.has_value()) {  // check already fired
            return {ArmVerdict::Poisoned,
                    "dependsOn library " + dep.spec + " did not build"};
        }
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
    // ── PROJECT MODE: validate the manifest, then compile WITH `outDir` AS THE
    //    WORKING DIRECTORY ────────────────────────────────────────────────────
    //
    // ★ THE CWD IS LOAD-BEARING HERE, and this runner did not have it. MEASURED:
    // its ctest entry (integrated_tests/CMakeLists.txt) sets no
    // WORKING_DIRECTORY, so the process cwd is `${CMAKE_BINARY_DIR}/
    // integrated_tests`, and the `CwdGuard` wrapped only the RUN. A project
    // manifest's relative `sources[]` globs expand against the PROCESS working
    // directory (D-AP2-SOURCES-GLOB), and a `preBuildScripts` generator is
    // spawned in that same directory — so without the guard the generator would
    // write its source into the BUILD TREE while the glob searched it there too,
    // scattering generated files outside the per-example scratch and defeating
    // the neighbor staging entirely. The in-process sibling has always compiled
    // with its scratch dir as the cwd (`ScratchDir::useAsCwd()`); this is what
    // makes the two agree.
    //
    // Scoped to PROJECT MODE deliberately. A `--compile` invocation is handed
    // ABSOLUTE source paths, so its behaviour does not depend on the cwd, and
    // moving 580 existing examples' compiles onto a different working directory
    // would be an unmeasured change riding along with this one.
    bool const projectMode = m.project.has_value();
    std::string projectArg;
    if (projectMode) {
        auto const projectPath = outDir / *m.project;
        auto const present = fileExists(projectPath);
        check(armName + ": project manifest staged at "
                  + projectPath.generic_string(),
              present.ok,
              present.ok ? ""
                         : present.why
                               + " — the neighbor staging above is RECURSIVE "
                                 "and preserves relative subpaths, so a "
                                 "manifest in a SUBDIRECTORY does reach the "
                                 "scratch tree; an absent one now means the "
                                 "path is wrong in expected.json, not that the "
                                 "staging dropped it");
        if (!present.ok) {
            return {ArmVerdict::Poisoned, "project manifest: " + present.why};
        }
        ProjectFacts facts;
        if (!readProjectFacts(projectPath, facts)) {
            check(armName + ": project manifest is readable", false,
                  "see the parse error above");
            return {ArmVerdict::Poisoned, "project manifest unreadable"};
        }
        // THE MIRROR CHECK. `compileProject` takes NO targets argument, so this
        // `spec` does not drive the build — it selects which of the project's
        // OWN targets this arm runs, gates `runOn`/`emulator`, and names the
        // artifact subdirectory. A spec the project never builds must fail HERE
        // rather than three steps later as a baffling missing artifact.
        bool const declared = std::find(facts.targets.begin(),
                                        facts.targets.end(), target->spec)
                            != facts.targets.end();
        std::string declaredList;
        for (auto const& s : facts.targets) {
            declaredList += (declaredList.empty() ? "" : ", ") + s;
        }
        check(armName + ": target spec " + target->spec
                  + " is declared by " + *m.project,
              declared,
              "the project builds only [" + declaredList + "], so this arm "
              "would assert an artifact nothing was asked to produce");
        if (!declared) {
            return {ArmVerdict::Poisoned,
                    "spec " + target->spec + " not in the project's targets"};
        }
        bool const langAgrees = m.language.empty()
                             || m.language == facts.language;
        check(armName + ": expected.json language mirrors " + *m.project,
              langAgrees,
              "expected.json says '" + m.language + "', the project manifest "
              "says '" + facts.language + "'; in project mode the project "
              "manifest is the authority and the mirror must agree");
        if (!langAgrees) {
            return {ArmVerdict::Poisoned, "language mirror disagrees"};
        }
        if (specFormatName(target->spec).empty()) {
            check(armName + ": target spec has an '<arch>:<format>' separator",
                  false,
                  "without it the per-format artifact subdirectory a project "
                  "build routes into cannot be derived");
            return {ArmVerdict::Poisoned,
                    "spec " + target->spec + " has no format half"};
        }
        // ENVIRONMENTAL, and the ONLY skip this mode may produce. A declared
        // build script whose interpreter this machine lacks is a BUILD INPUT the
        // manifest asked for and the machine could not supply — precisely
        // `SkippedBuildInputMissing` (warned by default, a [FAIL] under
        // DSS_STRICT_ARM_VERDICTS=1). Probed BEFORE the compile so the ledger
        // says "this box has no <interpreter>" instead of letting the build fail
        // on a source glob that matched nothing, which names the compiler for
        // the machine's shortfall.
        for (auto const& argv0 : facts.applicableScriptPrograms) {
            if (buildScriptProgramPresent(argv0)) continue;
            std::string const why = "build script program '" + argv0
                + "' declared by '" + *m.project
                + "' is not present on this machine (host=" + currentHostOs()
                + ")";
            std::cout << "  [SKIP] " << armName << " — " << why << "\n";
            return {ArmVerdict::SkippedBuildInputMissing, why};
        }
        projectArg = quote(projectPath.string());
    }

    std::string compileArgs;
    for (auto const& s : m.sources) {
        compileArgs += " " + quote((exampleDir / s).string());
    }
    auto const cliLog = outDir / "cli.log";
    // PROJECT MODE carries language, targets and sources INSIDE the
    // `.dss-project.json`, and the CLI accordingly requires neither
    // `--language` nor `--target` for it (src/program/cli_args.cpp, the
    // `Mode::Project` arm). Passing them would be inert at best and a second
    // source of truth at worst. `--output` and `--resolve-library` still apply:
    // the manifest MERGES its own `resolveLibraries` onto them.
    //
    // D-TEST-INTEGRATED-RUNNER-HAS-NO-OPTIMIZATION-ARM-CONCEPT: `--config=<name>`
    // is the ONE token that turns this from a default build into the arm the
    // manifest declared, and the name is threaded VERBATIM from the manifest's
    // `shippedPipeline`. Nothing here knows what pipelines exist — the CLI owns
    // that vocabulary (`parseCompileConfig`) and REJECTS an unrecognised name
    // with a real diagnostic, so a typo surfaces as a failed compile naming the
    // bad value rather than as a silent default build wearing the arm's label.
    // Applied in PROJECT MODE too: `--config` is a global CLI flag, not a
    // `--compile`-only one, so a project manifest that grows an arm gets it for
    // free instead of silently ignoring it.
    std::string const configArg =
        configName.empty() ? std::string{} : (" --config=" + configName);
    std::string cmd = quote(compiler)
        + (projectMode
               ? (" --project " + projectArg)
               : (" --compile"   + compileArgs
                  + " --language " + m.language
                  + " --target "   + target->spec))
        + resolveArgs
        + configArg
        + " --output "   + quote(outDir.string())
        + " > " + quote(cliLog.string()) + " 2>&1";

    // Reported SEPARATELY from the compile's rc so a chdir failure keeps its own
    // cause: "compile rc=-1" would send the reader to the compiler for a problem
    // that is entirely this harness's.
    bool compileCwdOk = true;
    int const sysRc = [&]() {
        if (!projectMode) return std::system(shellWrap(cmd).c_str());
        CwdGuard compileCwd{outDir};
        // FAIL-CLOSED: a chdir that did not take would send the generated
        // sources somewhere else and the failure would look like a DSS defect.
        check(armName + ": compile cwd set to " + outDir.generic_string(),
              compileCwd.ok,
              compileCwd.ok ? "" : "chdir failed; a project manifest's relative "
                                   "sources and its generated files would "
                                   "resolve against the build tree instead");
        compileCwdOk = compileCwd.ok;
        if (!compileCwd.ok) return -1;
        return std::system(shellWrap(cmd).c_str());
    }();
    if (!compileCwdOk) {
        return {ArmVerdict::Poisoned, "could not chdir to the output dir for "
                                      "the project-mode compile"};
    }

    auto const compileOk = [&]() -> bool {
        if (sysRc == 0) return true;
        check(armName + ": compile rc == 0 (rc=" + std::to_string(sysRc)
              + ", cli log: " + cliLog.generic_string() + ")",
              false);
        return false;
    }();
    if (!compileOk) {
        return {ArmVerdict::Poisoned,
                "compile rc=" + std::to_string(sysRc)};
    }
    check(armName + ": compile exits 0", true);

    // D-TEST-INTEGRATED-CORPUS-WALK-THROWS-UNCAUGHT: THE case this defect is about — these two are the checks a RED run reaches, and in their throwing form a failing example killed the runner rather than naming itself.
    // D-AP2-OUTPUT-ROUTING: a PROJECT build forces `setPerFormatOutputSubdir(true)`
    // (src/program/program.cpp:1760), so its artifact is at
    // `<outDir>/<formatName>/<name><ext>`, NOT at `<outDir>/<artifact>`.
    // COMPUTED EXPLICITLY rather than folded into the manifest string: spelling
    // the target's `artifact` as `"pe64-x86_64-windows-exec/main.exe"` composes
    // correctly through `fs::path`, and it would make `artifact` mean a FILENAME
    // in the 1,930 target entries that carry it today and a PATH in project-mode
    // ones, hiding the routing rule inside a string. One key, one meaning.
    auto const artifactPath = projectMode
        ? outDir / specFormatName(target->spec) / target->artifact
        : outDir / target->artifact;
    auto const artifactPresent = fileExists(artifactPath);
    check(armName + ": artifact exists at "
          + artifactPath.generic_string(),
          artifactPresent.ok, artifactPresent.why);
    if (!artifactPresent.ok) {
        return {ArmVerdict::Poisoned, "artifact: " + artifactPresent.why};
    }

    auto const artifactNonEmpty = fileNonEmpty(artifactPath);
    check(armName + ": artifact non-empty",
          artifactNonEmpty.ok, artifactNonEmpty.why);
    if (!artifactNonEmpty.ok) {
        return {ArmVerdict::Poisoned, "artifact: " + artifactNonEmpty.why};
    }
    // From here the COMPILE has demonstrably produced an image. Every remaining
    // exit carries it, because the arm-vs-baseline artifact comparison is
    // host-independent and must survive a run this MACHINE cannot perform: a box
    // with no emulator still proves the two pipelines produced different code.
    auto const compiledOutcome = [&](ArmVerdict v, std::string why) {
        CliArmOutcome o;
        o.verdict      = v;
        o.detail       = std::move(why);
        o.artifactPath = artifactPath;
        o.compiled     = true;
        return o;
    };

    // D-LK10-ENTRY-ARM64: cross-ARCH execution gate. The runOn match
    // above reconciled the host OS; now reconcile the host ARCH. A
    // binary whose target arch differs from the host's cannot exec
    // natively — it needs the manifest's emulator (e.g. qemu-aarch64
    // for an AArch64 ELF on x86_64).
    //
    // D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT: these two `[SKIP]` prints used
    // to increment NEITHER the pass nor the fail counter — a unit that was
    // never measured, invisible in the Results line. They now return distinct
    // VERDICTS: a missing `emulator` KEY is a manifest defect (the corpus lint
    // reds it host-independently), while an emulator absent from PATH is a
    // property of this MACHINE and is what DSS_STRICT_ARM_VERDICTS reds.
    // Mirrors the in-process examples_runner exactly.
    std::vector<std::string> launcherPrefix;
    if (std::string const targetArch = specTargetArch(target->spec);
        !targetArch.empty() && targetArch != currentHostArch()) {
        if (target->emulator.empty()) {
            std::string const why = "target arch '" + targetArch
                + "' != host arch '" + currentHostArch()
                + "' and the manifest declares no 'emulator'";
            std::cout << "  [SKIP] " << armName << " — " << why << "\n";
            return compiledOutcome(ArmVerdict::SkippedNoEmulatorDeclared, why);
        }
        auto const emuPath = findOnPath(target->emulator);
        if (emuPath.empty()) {
            std::string const why = "declared emulator '" + target->emulator
                + "' is not on PATH (target arch '" + targetArch
                + "' != host arch '" + currentHostArch() + "')";
            std::cout << "  [SKIP] " << armName << " — " << why << "\n";
            return compiledOutcome(ArmVerdict::SkippedEmulatorMissing, why);
        }
        launcherPrefix.push_back(emuPath);
    }

    // ── Run WITH `outDir` AS THE WORKING DIRECTORY ──────────────────────────
    //
    // The staged neighbor files above are only reachable if the child's CWD is
    // the directory they were staged into — a program that opens
    // `./libfoo.so` resolves it against its CWD, not against its own path.
    // The in-process sibling achieves this with `ScratchDir::useAsCwd()`; this
    // is the same move, scoped and restored (the `CwdGuard` hoisted above this
    // function, which the project-mode COMPILE also uses).
    // FAIL-CLOSED: if the chdir did not take, an example that needs a staged
    // file would fail for a reason that looks like a DSS defect. Say which.
    auto const absArtifact = fs::absolute(artifactPath);
    dss::test_support::RunResult result;
    {
        CwdGuard cwd{outDir};
        check(armName + ": run cwd set to " + outDir.generic_string(),
              cwd.ok,
              cwd.ok ? "" : "chdir failed; a staged neighbor file would be "
                            "unreachable and the example would fail as if DSS "
                            "were at fault");
        if (!cwd.ok) {
            return compiledOutcome(ArmVerdict::Poisoned,
                                   "could not chdir to the output dir");
        }
        // D-TEST-INTEGRATED-RUNNER-IGNORES-THE-RELEASE-ARM-AND-STDOUT-PINS:
        // capture ONLY when a pin exists, byte-for-byte the in-process sibling's
        // rule (examples_runner.cpp: `m.expectedStdout || t.expectedStdoutOverride`).
        // Capturing unconditionally would swap every example's inherited stdio
        // for a pipe — a behavioural change to 597 examples riding along with a
        // harness fix, and one that examples never pinned would never notice.
        result = dss::test_support::runBinary(
            absArtifact, std::chrono::milliseconds{5000}, captureStdout,
            launcherPrefix);
    }
    check(armName + ": spawn succeeded (diag='"
          + result.diagnostic + "')", result.spawned);
    if (!result.spawned) {
        return compiledOutcome(ArmVerdict::Poisoned,
                               "spawn failed: " + result.diagnostic);
    }

    check(armName + ": no timeout", !result.timedOut);
    if (result.timedOut) {
        return compiledOutcome(ArmVerdict::Poisoned, "spawn timed out");
    }

    // `Ran` means EXECUTED, not "asserted successfully" — the CALLER owns the
    // assertions (an arm's expectation is a relation between arms, and only the
    // caller holds both sides). Conflating the two would let a failing example
    // disappear from the verified count and reappear as a skip.
    auto out = compiledOutcome(ArmVerdict::Ran, {});
    out.exitCode       = result.exitCode;
    out.capturedStdout = result.capturedStdout;
    return out;
}

// Render a `\n`/`\r`-bearing pin readably in a one-line [FAIL] detail. A raw
// CRLF diff printed literally looks like two identical strings, which is the
// single most confusing way a stdout pin can fail.
[[nodiscard]] std::string escapeForDetail(std::string const& s) {
    std::string out;
    out.reserve(s.size() + 8u);
    for (char const c : s) {
        if (c == '\n')      out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else                out += c;
    }
    return out;
}

// A filesystem-safe spelling of an arm label (labels are free-form manifest
// text: `full-release-like`, `mem2reg-cse-dce`, …). Anything outside
// `[A-Za-z0-9._-]` becomes `_`, so an arm's scratch directory can never depend
// on what a manifest author typed.
[[nodiscard]] std::string sanitizeForPath(std::string const& s) {
    std::string out;
    out.reserve(s.size());
    for (char const c : s) {
        bool const safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                       || (c >= '0' && c <= '9') || c == '.' || c == '-'
                       || c == '_';
        out += safe ? c : '_';
    }
    return out;
}

// Drive one example's SELECTED target: the BASELINE arm, then every declared
// OPTIMIZED arm, then the differential comparison between them.
//
// ★★★ WHAT "ENFORCED" MEANS HERE, and why this is not a copy of the in-process
// differential. The in-process sibling compares an optimized arm against the
// BASELINE'S OBSERVED exit code. This runner compares against the same thing —
// but it reaches it through the CLI, where the arm is selected by a FLAG the
// user actually types (`--config=release`) rather than by an in-process
// `pipelineOverride` that BYPASSES the shipped pipeline registry entirely
// (src/program/compile_pipeline.hpp:172). So the two runners witness genuinely
// different halves of the same promise: the sibling proves the release PIPELINE
// preserves behaviour, and this one proves that asking the shipped BINARY for it
// on the command line actually delivers that pipeline. Neither substitutes for
// the other, which is why both must run the arm.
void runSelectedTargetViaCli(std::string const& compiler,
                      fs::path const&    exampleDir,
                      fs::path const&    outputBase,
                      ExampleManifest const& m,
                      ExampleTarget const*   target,
                      std::string const&     exampleName,
                      std::string const&     exampleId) {
    // Target spec format is `<cpu>:<format>`; the `:` is illegal in Windows path
    // components. Substitute `_` to derive a filesystem-safe sub-directory name.
    // The substitution is local to disk layout and never leaks back into the
    // CLI's --target argument (which keeps the canonical `:`-form).
    auto const specDir = [&]() {
        std::string s = target->spec;
        for (auto& c : s) if (c == ':') c = '_';
        return s;
    }();
    // Model 3: capture stdout when EITHER the manifest-level pin OR this
    // target's override is present (so a per-target override alone still routes
    // the pipe). Identical to the in-process sibling's rule.
    bool const captureStdout = m.expectedStdout.has_value()
                            || target->expectedStdoutOverride.has_value();
    std::optional<std::string> const effectiveStdout =
        target->expectedStdoutOverride.has_value()
            ? target->expectedStdoutOverride : m.expectedStdout;
    // C11/C23 6.4.5: the per-target override (when present) is the authority for
    // THIS target's exit code; otherwise the manifest-level `exitCode`.
    std::int64_t const expectedExit = target->exitCodeOverride.has_value()
                                          ? *target->exitCodeOverride
                                          : m.exitCode;

    // ── The BASELINE arm — the build this runner has always performed ───────
    auto const baseline = compileAndRunArmViaCli(
        compiler, exampleDir, outputBase / "ex" / exampleName / specDir, m,
        target, exampleName, "baseline", /*configName*/ "", captureStdout);
    // Recorded HERE rather than by the caller so the bound target's baseline row
    // precedes its own optimized-arm rows in the ledger — a summary that lists
    // an arm before the build it is differential against reads backwards. The
    // ledger's `arm` string stays the historical `"cli"` for the baseline, so
    // every pre-existing skip line greps exactly as it did.
    armLedger.record(exampleId, target->spec, "cli", baseline.verdict,
                     baseline.detail);
    if (baseline.verdict == ArmVerdict::Ran) {
        bool const exitMatches =
            static_cast<std::int64_t>(baseline.exitCode) == expectedExit;
        // The qemu-sysroot remedy line is appended ONLY on the failing branch,
        // and through the SAME shared helper the in-process runner uses
        // (D-TEST-QEMU_LD_PREFIX-AMBIENT-ONLY item (2), first half). Pairing is
        // the point: an arm that explains itself in one harness and stays mute
        // in the other is the divergence `arm_verdict_ledger.hpp` exists to
        // prevent. On a pass the hint would be noise, so it never renders there.
        check(exampleName + ": OS exit code == " + std::to_string(expectedExit)
              + " (got " + std::to_string(baseline.exitCode) + ")"
              + (exitMatches ? std::string{}
                             : qemuSysrootHint(target->spec, target->emulator)),
              exitMatches);
        if (effectiveStdout.has_value()) {
            ++stdoutPinsAsserted;
            if (!effectiveStdout->empty()) ++stdoutPinsAssertedNonEmpty;
            bool const stdoutMatches =
                baseline.capturedStdout == *effectiveStdout;
            check(exampleName + ": captured stdout matches the manifest pin ("
                      + std::to_string(effectiveStdout->size()) + " bytes)",
                  stdoutMatches,
                  stdoutMatches ? ""
                                : "expected \"" + escapeForDetail(*effectiveStdout)
                                      + "\", got \""
                                      + escapeForDetail(baseline.capturedStdout)
                                      + "\"");
        }
    }

    // ── The declared OPTIMIZED arms ─────────────────────────────────────────
    for (auto const& arm : m.optimizedPipelines) {
        ++optimizedArmsDeclaredOnBoundTarget;
        auto const ledgerArm = "cli:" + arm.label;
        // An inline `passes` list has no CLI spelling. LOUD AND COUNTED, never
        // silent: it lands in the ledger (so the Results line's "NOT verified"
        // total carries it), it prints, and the corpus summary reports the
        // count. The in-process sibling drives `pipelineOverride` directly and
        // IS the witness for this form.
        if (arm.hasPasses) {
            ++optimizedArmsNotExpressibleOnCli;
            std::string const why =
                "arm '" + arm.label + "' declares an inline 'passes' list, and"
                " the shipped CLI has no flag that expresses one (only"
                " --config=<shipped pipeline name>). Witnessed by the in-process"
                " runner (tests/examples/examples_runner), which drives"
                " CompileOptions::pipelineOverride directly";
            std::cout << "  [SKIP] " << exampleName << " [arm=" << arm.label
                      << "] — " << why << "\n";
            armLedger.record(exampleId, target->spec, ledgerArm,
                             ArmVerdict::NotSelectedByRunner, why);
            continue;
        }
        // The baseline's COMPILE failing means this arm has nothing to be
        // differential against, and a second failing compile of the same source
        // would only double the noise. Ledgered rather than dropped, so the
        // declared work still appears in the accounting.
        if (!baseline.compiled) {
            armLedger.record(exampleId, target->spec, ledgerArm,
                             baseline.verdict,
                             baseline.detail
                                 + " (compile not attempted: the baseline arm"
                                   " produced no artifact)");
            continue;
        }
        auto const armOut = compileAndRunArmViaCli(
            compiler, exampleDir,
            outputBase / "ex" / exampleName
                / (specDir + ".arm-" + sanitizeForPath(arm.label)),
            m, target, exampleName, arm.label, *arm.shippedPipeline,
            captureStdout);
        armLedger.record(exampleId, target->spec, ledgerArm, armOut.verdict,
                         armOut.detail);
        if (!armOut.compiled) continue;  // its own check already fired
        ++optimizedArmsRunViaConfigFlag;

        // ★ THE SELF-WITNESSING HALF. Compared even when the RUN was skipped,
        // because two pipelines producing different code is a fact about the
        // COMPILER and needs no machine to execute the result. This is what a
        // dropped `--config` cannot survive: without the flag the arm IS the
        // baseline build and every image below is byte-identical.
        auto const differs = filesDiffer(armOut.artifactPath,
                                         baseline.artifactPath);
        if (differs.ok) {
            ++optimizedArmsArtifactDiffered;
        }
        // The arm's declared opt-in, read HERE rather than merely accepted at
        // load time. `differs.why` carries "byte-identical" when the images
        // matched and a REASON when the comparison could not run at all; both
        // are failures for an arm that declares it must differ, because an
        // uncomparable arm must never be read as a compared one.
        if (arm.mustDifferFromBaseline) {
            check(exampleName + " [arm=" + arm.label
                      + "]: optimized image differs from the baseline"
                        " (manifest declares mustDifferFromBaseline)",
                  differs.ok,
                  differs.ok
                      ? ""
                      : "the arm declares \"mustDifferFromBaseline\": true but"
                        " the comparison did not witness a difference ("
                            + differs.why
                            + ") — either the source has nothing for the"
                              " pipeline to transform (give it one: an inlinable"
                              " helper, an accumulator, a loop) or the pipeline"
                              " regressed");
        }

        if (baseline.verdict != ArmVerdict::Ran) continue;  // nothing to compare
        // The two arms must share the run outcome: both ran, or the arm names
        // why it could not. A baseline that ran while its arm did not is a real
        // finding, not a skip.
        bool const armRan = armOut.verdict == ArmVerdict::Ran;
        check(exampleName + " [arm=" + arm.label
                  + "]: ran (the baseline ran, so this arm must too)",
              armRan,
              armRan ? "" : "verdict=" + std::string{armVerdictName(armOut.verdict)}
                                + " — " + armOut.detail);
        if (!armRan) continue;

        // D-CSUBSET-INLINE-FUNCTION-NO-EXTERNAL-DEFINITION-EMITTED: an arm inside
        // an `optimizationObservable` example may pin its OWN exit code; every
        // other arm must equal the baseline. Still a STRICT equality against a
        // declared number — the arm is not exempted from being checked, only
        // from being checked against the baseline. R1/R3 at load time already
        // established that the number exists because a clause sanctions it and
        // that it genuinely differs.
        std::int64_t const armExpectedExit =
            arm.exitCode.has_value() ? *arm.exitCode
                                     : static_cast<std::int64_t>(baseline.exitCode);
        bool const armExitMatches =
            static_cast<std::int64_t>(armOut.exitCode) == armExpectedExit;
        check(exampleName + " [arm=" + arm.label + "]: OS exit code == "
                  + std::to_string(armExpectedExit) + " (got "
                  + std::to_string(armOut.exitCode) + ")",
              armExitMatches,
              armExitMatches
                  ? ""
                  : "differential-verify FAIL: optimized arm '" + arm.label
                        + "' (--config=" + *arm.shippedPipeline
                        + ") produced exit code "
                        + std::to_string(armOut.exitCode) + " vs expected "
                        + std::to_string(armExpectedExit)
                        + (arm.exitCode.has_value()
                               ? " (this arm declares its own exit code under"
                                 " optimizationObservable clause \""
                                     + m.optimizationObservable->clause + "\")"
                               : " (the baseline's)")
                        + " — pipeline regression");
        if (effectiveStdout.has_value()) {
            bool const armStdoutMatches =
                armOut.capturedStdout == baseline.capturedStdout;
            check(exampleName + " [arm=" + arm.label
                      + "]: captured stdout matches the baseline arm",
                  armStdoutMatches,
                  armStdoutMatches
                      ? ""
                      : "differential-verify FAIL: baseline \""
                            + escapeForDetail(baseline.capturedStdout)
                            + "\", optimized \""
                            + escapeForDetail(armOut.capturedStdout) + "\"");
        }
    }
}

// Bind the target this runner will drive, ledger EVERY declared arm of the
// manifest, and run the bound one.
//
// D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT + D-TEST-CLI-HARNESS-BINDS-FIRST-
// MATCHING-TARGET: this runner drives ONE target per manifest and never
// considers the rest — a known harness limitation with its own anchor,
// deliberately NOT fixed here. What IS fixed here is the accounting: an arm this
// runner never reaches is ledgered as `NotSelectedByRunner` rather than being
// absent (which would understate the declared work) or counted as a skip (which
// would blame the manifest or the machine for a harness rule).
//
// D-TEST-INTEGRATED-TESTS-CANNOT-PASS-ON-A-NATIVE-ARM64-LINUX-HOST: WHICH one it
// binds is the fixed part. It used to be the first `runOn` match, which on a
// native aarch64 Linux box is the corpus's x86_64 arm — cross-arch there, no
// emulator declared — so that host ran NOTHING and [Test 5]'s stdout-capture
// witness had no non-empty pin to assert. The rule now prefers the arm THIS
// MACHINE CAN EXECUTE (`selectBoundTargetIndex`, arm_verdict_ledger.hpp: target
// arch == host arch, else first `runOn` match). On every host whose manifests
// offer exactly one `runOn` match — every windows and darwin arm in the corpus —
// the two rules pick the SAME target by construction.
void runExampleViaCli(std::string const& compiler,
                      fs::path const&    exampleDir,
                      fs::path const&    outputBase,
                      ExampleManifest const& m,
                      std::string const& exampleId) {
    auto const host = currentHostOs();

    // The `runOn` verdict is computed ONCE and feeds both the binding and the
    // ledger below, so the two can never disagree about which arms matched this
    // host — the divergence class that produced this whole family of defects.
    std::vector<HostBindingCandidate> candidates;
    candidates.reserve(m.targets.size());
    for (auto const& t : m.targets) {
        candidates.push_back(
            {t.spec, std::find(t.runOn.begin(), t.runOn.end(), host)
                         != t.runOn.end()});
    }
    std::size_t const boundIndex =
        selectBoundTargetIndex(candidates, currentHostArch());
    ExampleTarget const* const target =
        (boundIndex == kNoBoundTarget) ? nullptr : &m.targets[boundIndex];

    auto const exampleName = exampleDir.filename().generic_string();

    // `target == nullptr` ⇒ no target matched, so no arm can be
    // NotSelectedByRunner; read once so the loop never dereferences a null.
    std::string const boundSpec =
        (target != nullptr) ? target->spec : std::string{"<none>"};

    // Every target that is NOT the bound one gets its verdict FIRST, so no
    // return path below can drop it.
    //
    // D-TEST-INTEGRATED-RUNNER-HAS-NO-OPTIMIZATION-ARM-CONCEPT: the declared
    // work is (target × arm), not target alone. Before the optimized arms
    // landed, a 4-target 2-arm manifest declared 4 rows here and the Results
    // line's `T declared target arms` understated the corpus by the whole
    // optimizer axis — the same undercount D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-
    // VERDICT closed one level up. An unreached target's arms carry THAT
    // target's reason, because that is genuinely why they did not run.
    for (std::size_t i = 0; i < m.targets.size(); ++i) {
        if (i == boundIndex) continue;
        auto const& t             = m.targets[i];
        bool const  runOnMatches  = candidates[i].runsOnHost;
        ArmVerdict  verdict = ArmVerdict::SkippedByRunOn;
        std::string why;
        if (runOnMatches) {
            verdict = ArmVerdict::NotSelectedByRunner;
            // ⚠ The anchor name stays ONE contiguous token in the source: a
            // `"D-TEST-..." "..."` split still produces the right runtime
            // string, and makes the anchor ungreppable in the file that cites it.
            why = "runOn includes host=" + host
                + " but this runner binds ONE target per manifest and bound"
                  " spec=" + boundSpec + " (the host's own arch where the"
                  " manifest offers it)"
                + " — D-TEST-CLI-HARNESS-BINDS-FIRST-MATCHING-TARGET";
        } else {
            std::string runOnList;
            // `k`, not `i`: the enclosing loop now owns `i` (it indexes
            // `candidates` in lockstep with `m.targets`), and a shadowing
            // counter here would compile silently while reading as a bug.
            for (std::size_t k = 0; k < t.runOn.size(); ++k) {
                if (k != 0) runOnList += ',';
                runOnList += t.runOn[k];
            }
            why = "runOn=[" + runOnList + "] excludes host=" + host;
        }
        armLedger.record(exampleId, t.spec, "cli", verdict, why);
        for (auto const& arm : m.optimizedPipelines) {
            armLedger.record(exampleId, t.spec, "cli:" + arm.label, verdict,
                             why + " (so this target's optimized arm was not"
                                   " built either)");
        }
    }

    if (target == nullptr) {
        std::cout << "  [SKIP] " << exampleName
                  << " — no target's runOn includes host=" << host
                  << " (cross-host compile-only is exercised by"
                  << " tests/examples/ in-process runner)\n";
        return;
    }

    runSelectedTargetViaCli(compiler, exampleDir, outputBase, m, target,
                            exampleName, exampleId);
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
                           ExampleManifest const& m,
                           std::string const& exampleId) {
    auto const exampleName = exampleDir.filename().generic_string();
    if (m.targets.empty()) {
        check(exampleName + ": expect-error example declares a target spec", false);
        return;
    }
    auto const& spec = m.targets.front().spec;

    // D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT: an expect-error arm is VERIFIED
    // without being spawned — the assertion is the rejected compile plus the
    // rendered diagnostics, which are host-independent. Only the FIRST target
    // drives that compile here (the front-end error is target-independent), so
    // any further declared target is honestly `NotSelectedByRunner` rather than
    // silently absent.
    armLedger.record(exampleId, spec, "cli-expect-error",
                     ArmVerdict::ExpectErrorAsserted,
                     "CLI must reject the source and render the declared"
                     " diagnostics (nothing is spawned)");
    for (std::size_t i = 1; i < m.targets.size(); ++i) {
        armLedger.record(exampleId, m.targets[i].spec, "cli-expect-error",
                         ArmVerdict::NotSelectedByRunner,
                         "expect-error compiles only the FIRST declared target"
                         " (spec=" + spec + ")");
    }

    auto const specDir = [&]() {
        std::string s = spec;
        for (auto& c : s) if (c == ':') c = '_';
        return s;
    }();
    auto const outDir = outputBase / "ex" / exampleName / specDir;
    // D-TEST-INTEGRATED-CORPUS-WALK-THROWS-UNCAUGHT: as in runExampleViaCli — reported against THIS example, never thrown out of the run.
    if (auto const made = madeDirectory(outDir); !made.ok) {
        check(exampleName + ": output directory created", false, made.why);
        return;
    }

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
    // D-TEST-INTEGRATED-CORPUS-WALK-THROWS-UNCAUGHT: this whole walk uses the `error_code` overloads, and every failure below is reported and counted rather than thrown out of a function no `try` encloses.
    //
    // The severity split is deliberate, and it is the decision most worth
    // getting right here. The corpus ROOT — unreadable, or not a directory —
    // is a SETUP error: nothing can run, so it is loud and it ENDS the walk.
    // A single ENTRY — one looping symlink, one directory whose permissions
    // were stripped — is a reported FAILURE and the walk CONTINUES. Inverted
    // either way this reads badly: a broken setup would emit 500 confusing
    // per-example failures, or one bad symlink would black out the whole
    // corpus. Both severities are counted, so RC is 1 in every case.
    std::error_code rootEc;
    bool const rootIsDir = fs::is_directory(examplesRoot, rootEc);
    if (rootEc) {
        std::cerr << "[ERROR] cannot stat examples root "
                  << examplesRoot.generic_string() << ": " << rootEc.message()
                  << "\n";
        ++failures;
        return;
    }
    if (!rootIsDir) {
        std::cerr << "[ERROR] examples root not a directory: "
                  << examplesRoot.generic_string() << "\n";
        ++failures;
        return;
    }
    // Walk examples/<lang>/<name>/expected.json. The tree depth
    // is fixed (2 levels) so a recursive iterator is overkill;
    // a nested loop is clearer.
    std::vector<fs::path> manifestPaths;
    std::error_code langEc;
    fs::directory_iterator langIt(examplesRoot, langEc);
    if (langEc) {
        std::cerr << "[ERROR] cannot scan examples root "
                  << examplesRoot.generic_string() << ": " << langEc.message()
                  << "\n";
        ++failures;
        return;
    }
    // `it.increment(ec)`, NOT a range-for: the range-for's `operator++` is the
    // THROWING increment — the same reason `pruneKeptRoots` spells its walk out
    // longhand. A readdir can fail mid-walk (EIO, or ESTALE on a networked
    // checkout) long after the directory opened cleanly.
    for (fs::directory_iterator const langEnd; langIt != langEnd;
         langIt.increment(langEc)) {
        if (langEc) break;  // reported after the loop
        auto const langPath = langIt->path();
        // `is_directory(ec)`, NOT `is_directory()`: this reaches the filesystem
        // whenever the entry's cached readdir type is a symlink or unknown, and
        // a self-referential symlink answers ELOOP rather than yes-or-no.
        std::error_code langKindEc;
        bool const langIsDir = langIt->is_directory(langKindEc);
        if (langKindEc) {
            std::cerr << "[ERROR] cannot stat corpus entry "
                      << langPath.generic_string() << ": "
                      << langKindEc.message() << "\n";
            ++failures;
            continue;  // one bad entry, not the corpus
        }
        if (!langIsDir) continue;

        std::error_code nameEc;
        fs::directory_iterator nameIt(langPath, nameEc);
        if (nameEc) {
            std::cerr << "[ERROR] cannot scan corpus language directory "
                      << langPath.generic_string() << ": " << nameEc.message()
                      << "\n";
            ++failures;
            continue;
        }
        for (fs::directory_iterator const nameEnd; nameIt != nameEnd;
             nameIt.increment(nameEc)) {
            if (nameEc) break;  // reported after this inner loop
            auto const namePath = nameIt->path();
            std::error_code nameKindEc;
            bool const nameIsDir = nameIt->is_directory(nameKindEc);
            if (nameKindEc) {
                std::cerr << "[ERROR] cannot stat corpus entry "
                          << namePath.generic_string() << ": "
                          << nameKindEc.message() << "\n";
                ++failures;
                continue;
            }
            if (!nameIsDir) continue;
            auto const mp = namePath / "expected.json";
            // Three outcomes, not two. ABSENT is ordinary — a `<name>/` with
            // no manifest simply is not an example, exactly as before. But a
            // manifest that cannot be LOOKED AT is reported rather than
            // skipped: dropping it silently would shrink the corpus and still
            // print a green Results line, which is the failure mode this
            // defect is named for.
            std::error_code mpEc;
            bool const haveManifest = fs::exists(mp, mpEc);
            if (mpEc) {
                std::cerr << "[ERROR] cannot stat manifest "
                          << mp.generic_string() << ": " << mpEc.message()
                          << "\n";
                ++failures;
                continue;
            }
            if (haveManifest) manifestPaths.push_back(mp);
        }
        // Checked here rather than at the `break`: a failed `increment` may
        // leave the iterator AT `end`, in which case the loop condition ends
        // the walk and the body never runs again. One report covers both exits.
        if (nameEc) {
            std::cerr << "[ERROR] scan of corpus language directory "
                      << langPath.generic_string() << " stopped early: "
                      << nameEc.message() << " — examples under it are missing"
                      << " from this run\n";
            ++failures;
        }
    }
    if (langEc) {
        std::cerr << "[ERROR] scan of examples root "
                  << examplesRoot.generic_string() << " stopped early: "
                  << langEc.message() << " — examples are missing from this"
                  << " run\n";
        ++failures;
    }
    // Deterministic order for reproducible logs.
    std::sort(manifestPaths.begin(), manifestPaths.end());

    if (manifestPaths.empty()) {
        std::cerr << "[ERROR] no examples found under "
                  << examplesRoot.generic_string() << "\n";
        ++failures;
        return;
    }
    // Published for [Test 4]'s non-vacuity guard. Counts manifests FOUND (the
    // same thing the in-process twin counts at examples_runner.cpp:1213), not
    // manifests parsed: a manifest that fails to parse is already a `[FAIL]`
    // below, and folding it out of this count would let a corpus-wide parse
    // break re-open the vacuous-lint hole from the other side.
    manifestsWalked = manifestPaths.size();
    std::cout << "[Test 3] Examples corpus via CLI subprocess ("
              << manifestPaths.size() << " manifests)\n";
    for (auto const& mp : manifestPaths) {
        ExampleManifest m;
        if (!readManifest(mp, m)) {
            ++failures;
            continue;
        }
        auto const exampleDir = mp.parent_path();
        // `<lang>/<name>` — the SAME example id the in-process runner prints,
        // so a ledger line from either harness is greppable the same way.
        auto const exampleId =
            exampleDir.parent_path().filename().generic_string() + "/"
            + exampleDir.filename().generic_string();
        // D-TEST-MANIFEST-ARM64-ARM-WITHOUT-EMULATOR: flatten this manifest's
        // (target arm × runOn OS) declarations for the corpus-wide emulator
        // lint below. Collected from the SAME parse the run uses, so the lint
        // cannot be looking at a different corpus than the run did.
        for (auto const& t : m.targets) {
            for (auto const& osName : t.runOn) {
                declaredArms.push_back({exampleId, t.spec, osName, t.emulator});
            }
        }
        // V2-4 Part C: an expectDiagnostics manifest asserts a rejected
        // compile + positioned CLI diagnostics; otherwise the standard
        // compile + run path.
        //
        // ⚠ [[D-TEST-EXAMPLES-OPTIMIZED-ARM-DROPPED-ON-DIAGNOSTIC-MANIFEST]] —
        // NAMED HERE BECAUSE THIS RUNNER NOW REPRODUCES IT, IDENTICALLY AND
        // DELIBERATELY. `optimizedPipelines` is PARSED for every manifest above,
        // and the expect-error branch below never looks at it — so an arm
        // declared on an `expectDiagnostics` example would be accepted and then
        // silently dropped, exactly as the in-process sibling drops it. ✔MEASURED
        // 2026-08-17: ZERO of the shipped manifests declare both, so nothing is
        // being dropped today. The RIGHT fix is a load-time REFUSAL of the pair
        // (the model is the `project` + `expectDiagnostics` refusal in
        // `readManifest` above, which both runners already share) — and it must
        // land in BOTH runners in ONE change. Adding it to only this side would
        // make the two runners accept different manifests, which is the very
        // divergence [Test 6] and this whole change exist to end.
        if (m.expectDiagnostics.empty()) {
            runExampleViaCli(compiler, exampleDir, outputBase, m, exampleId);
        } else {
            runErrorExampleViaCli(compiler, exampleDir, outputBase, m, exampleId);
        }
    }
    std::cout << "\n";
}

// ── D-TEST-MANIFEST-ARM64-ARM-WITHOUT-EMULATOR: the corpus emulator lint ───
//
// A STRICT check, not a warning: an arm that declares no emulator where 449
// siblings with the same (arch, runOn-OS) declare one is silently skipped on
// every host of a different arch, forever. The rule and its rationale live in
// `lintDeclaredEmulators`; this is only the reporting half. Host-independent,
// so it fires on whichever leg the author happens to run.
void runManifestEmulatorLint() {
    std::cout << "[Test 4] Manifest emulator lint (corpus-wide, host-independent)\n";
    // NON-VACUITY, asserted BEFORE the rule runs. With an empty `declaredArms`
    // the rule returns an empty finding set and the `check(..., true)` below
    // reports PASS "(0 declarations)" — a lint that silently linted nothing,
    // which is the same class of defect as the skip it exists to catch.
    // MEASURED 2026-08-04, these two checks removed and `declaredArms` forced
    // empty: [Test 4] printed the single line `[PASS] every declared target arm
    // agrees ... (0 declarations)` and the process exited 0. With the checks
    // present the same injection reds twice and exits 1 (2 failed). The
    // in-process twin already guards this exact pair — examples_runner.cpp:1227
    // (`ASSERT_GT(manifestCount, 0u)`) and :1230 (`ASSERT_FALSE(arms.empty())`)
    // — and one runner enforcing a guard while its sibling shrugs is the silent
    // harness bug this whole change exists to close (integrated_tests/
    // CMakeLists.txt:42-46 states the pairing rule).
    //
    // The floor is `> 0`, deliberately NOT a pinned corpus size: 558 manifests
    // today, 557 last cycle. A hardcoded expected count is the same inert pin in
    // a different disguise — it rots into a rubber stamp the first time someone
    // updates the number to make the suite green again.
    check("the emulator lint had manifests to lint", manifestsWalked > 0,
          "the corpus walk read 0 expected.json manifests — the lint measured"
          " nothing and must not report success");
    check("the emulator lint had declared (arm x runOn) pairs to lint",
          !declaredArms.empty(),
          "0 declared (arm x runOn) pairs out of "
              + std::to_string(manifestsWalked)
              + " manifest(s) — no target arm declared a runOn host, so the"
                " sibling-consistency rule below is vacuously satisfied");
    auto const findings =
        ::dss::test_support::lintDeclaredEmulators(declaredArms);
    if (findings.empty()) {
        check("every declared target arm agrees with its (arch, runOn) siblings"
              " on the emulator vocabulary ("
              + std::to_string(declaredArms.size()) + " declarations)",
              true);
    }
    for (auto const& f : findings) {
        check((f.manifest.empty() ? std::string{"corpus"} : f.manifest)
                  + (f.spec.empty() ? std::string{} : ": spec=" + f.spec)
                  + ": emulator declaration is consistent with its siblings",
              false, f.message);
    }
    std::cout << "\n";
}

// ── D-TEST-INTEGRATED-RUNNER-HAS-NO-OPTIMIZATION-ARM-CONCEPT: the instrument ─
//
// ★★★ THE CHECK THAT CANNOT BE SATISFIED BY A BUILD THAT NEVER GOT THE FLAG.
// Three guards, in the order a reader should doubt them:
//
//   (1) the corpus DECLARED optimized arms on the targets this host binds. A
//       zero here means every assertion below is vacuously satisfied, and a
//       vacuous green is the one outcome this whole change exists to end.
//   (2) at least one of them was BUILT through `--config=<name>`. This is the
//       one a refactor kills first — deleting the arm loop leaves (1) intact.
//   (3) ★ at least one arm's ARTIFACT DIFFERS BYTE-WISE from its baseline's.
//       (1) and (2) are both satisfied by an arm whose `--config` was dropped
//       from the command line: it still compiles, still runs, still exits like
//       the baseline — because it now IS the baseline build — and every
//       differential comparison passes while witnessing nothing. Only a
//       POSITIVE difference in the produced image proves the flag reached the
//       compiler and changed what it emitted.
//
// The floors are `> 0`, deliberately NOT pinned counts: the corpus grows every
// cycle and a hardcoded number is an inert pin that gets edited back into a
// rubber stamp the first time it reds. `> 0` is the honest floor for "this
// instrument measured something", and the exact figures are PRINTED beside it so
// a reader can see the trend without the suite depending on it.
void runOptimizedArmInstrument() {
    std::cout << "[Test 5] Optimized-arm instrument (--config=<shipped pipeline>)\n";
    check("the corpus declared optimized arms on the bound targets",
          optimizedArmsDeclaredOnBoundTarget > 0,
          "0 declared optimizedPipelines arms reached a bound target — every"
          " optimizer assertion in this runner is vacuously satisfied");
    check("at least one optimized arm was BUILT through --config",
          optimizedArmsRunViaConfigFlag > 0,
          "of " + std::to_string(optimizedArmsDeclaredOnBoundTarget)
              + " declared arm(s), none produced an artifact via"
                " --config=<shippedPipeline>; the CLI-surface optimizer witness"
                " is inert");
    check("at least one optimized arm's ARTIFACT DIFFERS byte-wise from its"
          " baseline's (proves --config reached the compiler and changed what"
          " it emitted)",
          optimizedArmsArtifactDiffered > 0,
          "all " + std::to_string(optimizedArmsRunViaConfigFlag)
              + " optimized arm image(s) were BYTE-IDENTICAL to their"
                " baselines. Either the config flag is no longer threaded into"
                " the compile (so the arm IS the baseline build and asserts"
                " nothing), or the shipped pipelines have stopped differing");
    // The stdout half's own non-vacuity witness. An EMPTY pin is satisfied by a
    // pipe that was never routed, so "some pin was asserted" is not enough —
    // only a NON-EMPTY one proves bytes were actually drained from the child.
    check("at least one NON-EMPTY stdout pin was asserted (proves the capture"
          " pipe was routed, which an empty pin cannot)",
          stdoutPinsAssertedNonEmpty > 0,
          "of " + std::to_string(stdoutPinsAsserted)
              + " stdout pin(s) asserted on this host, NONE was non-empty — an"
                " empty pin is satisfied by a capture that never happened, so"
                " this host proved nothing about the capture path");
    std::cout << "  " << stdoutPinsAsserted << " stdout pin(s) asserted ("
              << stdoutPinsAssertedNonEmpty << " non-empty)\n";
    std::cout << "  " << optimizedArmsDeclaredOnBoundTarget
              << " declared on bound targets; "
              << optimizedArmsRunViaConfigFlag << " built via --config; "
              << optimizedArmsArtifactDiffered
              << " produced an image differing from their baseline; "
              << optimizedArmsNotExpressibleOnCli
              << " not expressible on the CLI surface (inline 'passes' arms —"
                 " witnessed by the in-process runner)\n\n";
}

// ── THE CLASS, not the instance: the two runners' manifest vocabularies ─────
//
// ★★★ WHY A VOCABULARY PIN AND NOT JUST THE FIX. The defect this cycle closes
// was NOT "one key was forgotten". It was that a key can be added to one corpus
// runner and never to the other, and NOTHING SAYS SO — the manifests keep
// parsing, both suites keep passing, and the sibling silently asserts less than
// its author believes. That happened to `optimizedPipelines`, `shippedPipeline`,
// `passes`, `optimizationObservable`, `clause` and `expectedStdout` — SIX keys,
// over months, while a written rule ("a capability change MUST hit BOTH
// runners") sat in this very file. ⇒ the rule needs a MACHINE check, not more
// prose. Fixing only the six would leave the seventh to happen exactly the same
// way.
//
// WHAT IT ASSERTS: the set of manifest keys each runner READS — every
// `contains("k")` / `.at("k")` / `.value("k", …)` outside a comment — is
// IDENTICAL in the two sources. ✔MEASURED 2026-08-17 after this change: 25 keys,
// zero difference in either direction.
//
// ⚠ ITS SCOPE, stated so nobody mistakes it for more than it is. This reads
// SOURCE TEXT, so it proves the key is MENTIONED, not that it is honoured; and
// it knows the three nlohmann accessor spellings both files actually use
// (neither uses `operator[]` — ✔MEASURED). It is a guard against the specific
// failure that occurred: a key one runner has never heard of. A runner that
// reads a key and ignores it is a different defect, and the load-time closed
// key sets plus [Test 5]'s instrument are what stand against that one.
//
// If a key is ever LEGITIMATELY one-sided, this pin reds and the remedy is to
// say so where a reader will see it — not to widen the matcher until it stops
// asking.
void runRunnerVocabularyPin() {
    std::cout << "[Test 6] Both corpus runners recognise the same manifest keys\n";
    // D-TEST-INTEGRATED-CORPUS-WALK-THROWS-UNCAUGHT, same discipline: `repoRoot`
    // THROWS when it cannot resolve, and this call sits outside every `try` in
    // `main`. Uncaught, it would end the process with no [FAIL] line, no Results
    // line and no name for what died — the exact failure that defect is about.
    fs::path root;
    try {
        root = ::dss::test::repoRoot();
    } catch (std::exception const& e) {
        check("resolve the repo root for the runner-vocabulary comparison", false,
              std::string{e.what()});
        std::cout << "\n";
        return;
    }
    struct Source {
        char const* label;
        fs::path    path;
    };
    Source const sources[2] = {
        {"integrated_tests/runner.cpp", root / "integrated_tests" / "runner.cpp"},
        {"tests/examples/examples_runner.cpp",
         root / "tests" / "examples" / "examples_runner.cpp"},
    };
    std::vector<std::string> found[2];
    for (int i = 0; i < 2; ++i) {
        std::ifstream in(sources[i].path.string(), std::ios::binary);
        std::string   body((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
        // NON-VACUITY, first and loudest: an unreadable or moved source yields an
        // EMPTY key set, and two empty sets compare EQUAL. That is the false
        // green this pin would otherwise hand out at exactly the moment it stopped
        // working, so it is checked before the comparison, not after.
        check(std::string{"read "} + sources[i].label + " ("
                  + std::to_string(body.size()) + " bytes)",
              !body.empty(),
              "cannot read " + sources[i].path.generic_string()
                  + " — with no text there are no keys, and two empty key sets"
                    " compare equal, so this pin would pass having read nothing");
        if (body.empty()) return;

        // Comments are STRIPPED before matching. Both files discuss manifest keys
        // in prose at length — including keys they deliberately do NOT read — and
        // a matcher that counted prose would report agreement that the code does
        // not have. (Same trap as the mutant whose own comment carried the
        // witness token: the text that DESCRIBES a thing must never be mistaken
        // for the thing.)
        std::string code;
        code.reserve(body.size());
        for (std::size_t i2 = 0; i2 < body.size();) {
            if (body.compare(i2, 2, "//") == 0) {
                while (i2 < body.size() && body[i2] != '\n') ++i2;
            } else if (body.compare(i2, 2, "/*") == 0) {
                i2 += 2;
                while (i2 + 1 < body.size()
                       && !(body[i2] == '*' && body[i2 + 1] == '/')) {
                    ++i2;
                }
                i2 = (i2 + 2 < body.size()) ? i2 + 2 : body.size();
            } else {
                code += body[i2++];
            }
        }
        // `contains("k")` / `.at("k")` / `.value("k", …)` — the three spellings
        // both files use to ASK a manifest for a key.
        for (char const* accessor : {"contains(\"", ".at(\"", ".value(\""}) {
            std::string const needle{accessor};
            for (std::size_t at = code.find(needle); at != std::string::npos;
                 at = code.find(needle, at + 1)) {
                auto const start = at + needle.size();
                auto const end   = code.find('"', start);
                if (end == std::string::npos) break;
                auto key = code.substr(start, end - start);
                if (key.empty()) continue;
                // Manifest keys are plain identifiers. Anything else is a string
                // that merely sits in an accessor's argument position.
                bool identifier = true;
                for (char const c : key) {
                    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                          || (c >= '0' && c <= '9') || c == '_')) {
                        identifier = false;
                        break;
                    }
                }
                if (identifier) found[i].push_back(std::move(key));
                at = end;
            }
        }
        std::sort(found[i].begin(), found[i].end());
        found[i].erase(std::unique(found[i].begin(), found[i].end()),
                       found[i].end());
        check(std::string{sources[i].label} + " reads at least one manifest key"
                  + " (" + std::to_string(found[i].size()) + " found)",
              !found[i].empty(),
              "the key matcher found nothing — its accessor spellings no longer"
              " describe how this file reads a manifest, so its agreement with"
              " the sibling means nothing");
        if (found[i].empty()) return;
    }

    auto const missingFrom = [&](int lacking, int having) {
        std::string out;
        for (auto const& k : found[having]) {
            if (std::find(found[lacking].begin(), found[lacking].end(), k)
                != found[lacking].end()) {
                continue;
            }
            out += (out.empty() ? "" : ", ") + k;
        }
        return out;
    };
    auto const onlyInSibling   = missingFrom(0, 1);
    auto const onlyInThisOne   = missingFrom(1, 0);
    check("the two corpus runners read the SAME manifest key set ("
              + std::to_string(found[0].size()) + " keys)",
          onlyInSibling.empty() && onlyInThisOne.empty(),
          (onlyInSibling.empty()
               ? std::string{}
               : "read ONLY by tests/examples/examples_runner.cpp: ["
                     + onlyInSibling + "]. ")
              + (onlyInThisOne.empty()
                     ? std::string{}
                     : "read ONLY by integrated_tests/runner.cpp: ["
                           + onlyInThisOne + "]. ")
              + "A manifest key one corpus runner has never heard of is"
                " INDISTINGUISHABLE FROM A TYPO to that runner: the example still"
                " passes there, having asserted less than its author believes."
                " Teach both runners the key, or — if the divergence is genuinely"
                " intentional — record WHY here, where the next reader meets it.");
    std::cout << "\n";
}

// ── D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT: the ledger summary ───────────
//
// Printed beside the pass/fail counts so `N passed` can never again be read as
// `N verified`. In STRICT mode every environmental skip additionally becomes a
// [FAIL]; structural skips never do (they are the manifest's intent) and
// NotSelectedByRunner never does (it is a harness limitation with its own
// anchor, and failing on it would make strict mode unusable until that closes).
//
// Runs BEFORE the Results line, not after: the strict-mode failures below go
// through `check()`, so reporting them afterwards would print a `0 failed`
// that the process's own exit code contradicts.
void reportArmVerdicts() {
    std::cout << "Arm verdicts: " << armLedger.renderCountsLine() << "\n"
              << armLedger.renderSkipDetail("  ", /*includeStructural=*/false);

    // NON-VACUITY — the check that makes this ledger an INSTRUMENT rather than a
    // log. Every other consumer of `armLedger` is either a `std::cout` (above,
    // and the Results line) or a strict-mode-only branch (below), and strict
    // mode is enabled on no automated leg — so WITHOUT this line the entire
    // ledger is inert and its absence is indistinguishable from its success.
    // MEASURED 2026-08-04, this check removed and the ledger emptied just above
    // (state-identical to a no-op `ArmVerdictLedger::record`, which only
    // accumulates — every assertion fires at the recording site): the runner
    // printed `Arm verdicts: 0 verified ... (of 0 declared arms)` and
    // `Results: 2711 passed, 0 failed, 0 of 0 declared target arms NOT
    // verified`, and exited 0. With the check present the same injection reds
    // and exits 1.
    //
    // NOT gated on strict mode, by design: an empty ledger is a broken
    // instrument on every host, not a property of the machine — the same reason
    // the twin's guard at examples_runner.cpp:1109 is a plain
    // `ASSERT_GT(ledger.total(), 0u)` and not part of its strict block. `> 0` is
    // the honest floor for "this instrument ran at all"; a pinned count would
    // red every time the corpus grows and would be edited back into a stamp.
    check("the arm-verdict ledger recorded at least one declared arm",
          armLedger.total() > 0,
          "no arm produced a verdict — this ledger is the only thing standing"
          " between a silently unrun arm and a green suite, so a run that"
          " ledgered nothing must never read as a pass");

    auto const strict = ::dss::test_support::readStrictArmVerdicts();
    if (strict.malformed) {
        check(std::string{::dss::test_support::kStrictArmVerdictsEnv}
                  + " has a value this runner will interpret",
              false,
              "unexpected value '" + strict.raw + "' — use '1' to require every"
              " declared arm to run, unset (or '0') otherwise. Refusing to"
              " interpret it, because a typo that silently disabled the gate is"
              " the failure this variable exists to prevent.");
    }

    auto const envSkips = armLedger.environmentalSkips();
    if (envSkips.empty()) return;
    for (auto const& r : envSkips) {
        if (strict.on) {
            check("STRICT ARM VERDICTS: " + r.example + " spec=" + r.spec
                      + " arm=" + r.arm + " ran",
                  false,
                  r.detail + ". This machine cannot supply what the manifest"
                             " declared; install the emulator or unset "
                      + ::dss::test_support::kStrictArmVerdictsEnv + ".");
        } else {
            std::cout << "  [WARN] " << r.example << " spec=" << r.spec
                      << " arm=" << r.arm << " did not run — " << r.detail
                      << " (set " << ::dss::test_support::kStrictArmVerdictsEnv
                      << "=1 to make this a failure)\n";
        }
    }
}

// ── Per-run scratch root ───────────────────────────────────────
//
// D-TEST-INTEGRATED-FIXED-TEMP-PATH-COLLIDES (opened TF-C97): this root MUST be unique per run. Do NOT "simplify" it back to a fixed name.
//
// It USED to be the constant `temp_directory_path()/"dss-integrated-tests"`,
// wiped with `remove_all` at startup. Every run on the host therefore shared
// ONE directory and DELETED it out from under any run already in flight, so
// two concurrent runs — two out-of-tree build dirs on one machine, e.g.
// parallel agents — destroyed each other's artifacts mid-run. What that
// produced was not a clean error but a large, alarming, entirely SPURIOUS
// failure set: ENOENT from `chmod` on an artifact deleted underneath the run,
// and `got 137` (SIGKILL) from binaries whose files vanished mid-exec. Four
// agents once reported four different whole-suite counts for one commit and
// all of it was this. The real cost is not the wasted time — a big spurious
// failure set is exactly the condition under which a GENUINE regression gets
// waved away as "probably the flake". That is why the tidy fixed name is the
// wrong trade at any price.
//
// Two non-fixes, recorded so they are not re-proposed: retrying on ENOENT
// HIDES the collision rather than removing it (that is how this stayed
// invisible), and serialising the test (a lock, RESOURCE_LOCK, RUN_SERIAL)
// conceals it while slowing every gate. The path must be UNIQUE, not
// contended-for.
//
// Uniqueness is by CONSTRUCTION rather than by hope, and deliberately reuses
// the scheme `tests/test_support/scratch_dir.hpp` arrived at when
// `D-TEST-EXAMPLES-RUNNER-PARALLEL-CONTENTION-FLAKE` was corrected: a pid is a
// SEED, not a guarantee (pids recycle, and a killed run leaves its directory
// behind), so the guarantee is the atomic `create_directory` claim below. That
// header is not reused DIRECTLY because its destructor removes the directory
// unconditionally, and a FAILED integration run has to stay inspectable — see
// the cleanup policy at the end of `main`.

// Every run claims its root inside this base. Pruning is scoped to it and
// touches nothing else.
//
// The name deliberately DIFFERS from the pre-fix `dss-integrated-tests`, and
// that is load-bearing rather than cosmetic. A pre-fix build of this runner
// still calls `remove_all()` on that exact path at startup, and every stale
// build directory on the host has one. Had the per-run roots been nested
// INSIDE the old name, a single stale binary would still have deleted all of
// them at once — the new scheme would have been correct and still lost the
// race. Renaming puts the roots somewhere a pre-fix binary cannot reach.
[[nodiscard]] fs::path scratchBase() {
    return fs::temp_directory_path() / "dss-integrated-tests-runs";
}

// The path a pre-fix runner used, kept ONLY so the leftovers can be named.
constexpr char const* kLegacyBaseName = "dss-integrated-tests";
// Written in the scratch base once the leftovers have been reported. This is
// what stops that report from becoming a permanent fixture — see
// `reportLegacyDebris`.
constexpr char const* kLegacyNotedName = "legacy-debris-noted.txt";

// Written once at claim time. Two jobs: it tells a human which build dir
// produced a kept root, and it is the marker that lets the prune recognise a
// directory as this runner's before removing anything.
constexpr char const* kRunInfoName = "run-info.txt";
// Present for the lifetime of the run, removed at exit. A root still carrying
// one may belong to a LIVE sibling, so the prune leaves it alone.
constexpr char const* kRunningName = ".running";
// Kept roots retained. MEASURED: a full corpus run leaves ~24 MB, so 3 kept
// roots is ~72 MB — enough history to compare two red runs, small enough that
// an agent looping on a failure cannot fill the temp filesystem.
constexpr std::size_t kKeptRootLimit = 3;
// A killed run cannot remove its own sentinel. Past this age `.running` is
// treated as stale rather than live, so a `kill -9` cannot leak a root
// forever. It is orders of magnitude longer than a full corpus run.
constexpr auto kRunningStaleAfter = std::chrono::hours{6};

// Claim a scratch root no other process can be using, and return it.
[[nodiscard]] fs::path claimRunRoot(std::string const& compiler,
                                    fs::path const&    examplesRoot) {
    auto const base = scratchBase();
    std::error_code ec;
    fs::create_directories(base, ec);
    if (ec) {
        throw std::runtime_error("cannot create scratch base '"
            + base.generic_string() + "': " + ec.message());
    }

#ifdef _WIN32
    auto const pid = static_cast<std::uint64_t>(_getpid());
#else
    auto const pid = static_cast<std::uint64_t>(getpid());
#endif

    for (std::uint32_t attempt = 0; attempt < 10000u; ++attempt) {
        auto candidate =
            base / (std::to_string(pid) + "-" + std::to_string(attempt));
        std::error_code cec;
        // `create_directory` (SINGULAR) returns true ONLY for the caller that
        // actually created the directory, and that check-and-create is atomic
        // in the OS — this one call IS the uniqueness guarantee, across
        // processes as well as threads. `create_directories` (PLURAL) cannot
        // do the job: it reports SUCCESS for a directory that already exists,
        // which is precisely how a run would silently adopt a live sibling's
        // root and re-open this defect.
        if (!fs::create_directory(candidate, cec)) {
            if (cec) {
                throw std::runtime_error("create_directory('"
                    + candidate.generic_string() + "') failed: "
                    + cec.message());
            }
            continue;  // taken — a live sibling, or a stale kept root
        }
        // `run-info.txt` BEFORE `.running`: a crash between the two leaves a
        // root the prune can still recognise and reclaim. The other order
        // would leak it permanently.
        std::ofstream info((candidate / kRunInfoName).string());
        info << "pid:           " << pid << "\n"
             << "compiler:      " << compiler << "\n"
             << "examples root: " << examplesRoot.generic_string() << "\n"
             << "host:          " << currentHostOs() << "/"
             << currentHostArch() << "\n";
        std::ofstream running((candidate / kRunningName).string());
        if (!info || !running) {
            throw std::runtime_error("could not write the run markers in '"
                + candidate.generic_string() + "' — without them a kept root "
                "is never reclaimed");
        }
        return candidate;
    }
    throw std::runtime_error("could not claim a unique scratch root under '"
        + base.generic_string() + "' after 10000 attempts. Kept roots are "
        "accumulating — delete the directory and re-run.");
}

// Bound the population of KEPT roots (a red run keeps its own — see `main`).
// Called once at startup AFTER this run claimed `mine`, so `mine` is never a
// candidate.
//
// The rule, one sentence so it stays debuggable: retain the `kKeptRootLimit`
// most recent roots and delete the rest — but NEVER one still holding a
// `.running` sentinel younger than `kRunningStaleAfter`, because that root may
// belong to a live sibling. That exclusion is the whole reason pruning is safe
// here: deleting a live run's directory is the ORIGINAL defect, and a
// retention bound that quietly reintroduced it would be worse than no bound.
//
// BEST-EFFORT, ALWAYS — a prune that cannot proceed logs and gets out of the
// way. It must NEVER fail the run, and that is a correctness requirement, not
// politeness: this function exists so that concurrent runs stop failing
// spuriously, so a prune that itself reds a run under concurrency would be the
// defect wearing the fix's clothes. Hence every filesystem call below is the
// `error_code` overload and every failure path is `continue`/`break`/`return`.
// Do NOT "improve" any of this into a hard error; the THROWING overloads
// surface out of `main`'s setup path as `[ERROR] scratch root setup failed`
// and RC=1 for the WHOLE integrated_tests run.
//
// MEASURED (macOS/APFS + libc++, 2026-07-31) so the next reader does not have
// to re-derive it: an entry that merely VANISHES mid-scan does not throw there,
// because `status()` maps ENOENT to `file_type::not_found` rather than to an
// error. The throwing forms still abort the run on every OTHER error class —
// a stat that hits EACCES or ELOOP, a readdir that hits EIO or an ESTALE on a
// networked TMPDIR — all measured to throw from inside this exact loop. The
// `error_code` forms are what make the loop survivable on every host and every
// error class rather than on the one that happens to be benign here.
void pruneKeptRoots(fs::path const& base, fs::path const& mine) {
    auto const now = fs::file_time_type::clock::now();

    std::error_code ec;
    fs::directory_iterator it(base, ec);
    if (ec) {
        std::cerr << "[WARN] cannot scan scratch base '"
                  << base.generic_string() << "': " << ec.message()
                  << " — kept roots may accumulate\n";
        return;
    }

    std::vector<std::pair<fs::file_time_type, fs::path>> prunable;
    // `it.increment(ec)`, NOT a range-for: the range-for's `operator++` is the
    // throwing increment. A sibling's prune mutates this directory WHILE we
    // walk it — that is the expected condition here, not the exotic one.
    for (fs::directory_iterator const end; it != end; it.increment(ec)) {
        if (ec) break;  // reported after the loop
        auto const p = it->path();
        // `is_directory(dec)`, NOT `is_directory()`: same reason, and this one
        // reaches the filesystem whenever the entry's cached type is a symlink
        // or unknown.
        std::error_code dec;
        if (p == mine || !it->is_directory(dec) || dec) continue;
        // Only ever remove a directory this runner demonstrably created.
        std::error_code iec;
        if (!fs::exists(p / kRunInfoName, iec) || iec) continue;
        // A sentinel we cannot READ counts as LIVE. The uncertainty is
        // asymmetric: keeping a dead root costs a little temp space, while
        // deleting a live one is D-TEST-INTEGRATED-FIXED-TEMP-PATH-COLLIDES
        // over again.
        std::error_code rec;
        auto const running = p / kRunningName;
        bool const sentinel = fs::exists(running, rec);
        if (rec) continue;  // could not even ask
        if (sentinel) {
            auto const stamp = fs::last_write_time(running, rec);
            if (rec || now - stamp < kRunningStaleAfter) continue;  // maybe LIVE
        }
        std::error_code mec;
        auto const mtime = fs::last_write_time(p, mec);
        if (mec) continue;  // vanished under us — a sibling cleaning up
        prunable.emplace_back(mtime, p);
    }
    // Checked here rather than at the `break`: a failed `increment` may leave
    // the iterator AT `end`, in which case the loop condition ends the walk and
    // the body never runs again. One report covers both exits.
    if (ec) {
        std::cerr << "[WARN] scratch base scan of '" << base.generic_string()
                  << "' stopped early: " << ec.message()
                  << " — kept roots may accumulate\n";
    }
    if (prunable.size() <= kKeptRootLimit) return;

    std::sort(prunable.begin(), prunable.end(),
              [](auto const& a, auto const& b) { return a.first > b.first; });
    for (std::size_t i = kKeptRootLimit; i < prunable.size(); ++i) {
        auto const& root = prunable[i].second;
        std::error_code rec;
        fs::remove_all(root, rec);
        if (!rec) continue;
        // A sibling prune got there first: the root is GONE, which is the
        // outcome this loop wanted, so there is nothing to report. MEASURED
        // (six concurrent runs over a 6000-root base): libc++ reports ENOENT
        // from the middle of `remove_all`'s own recursion, and warning on it
        // emitted ~2500 [WARN] lines PER RUN — a spurious noise burst thrown by
        // the very function whose job is to stop concurrency producing spurious
        // noise. Warn only if the root is still THERE, i.e. only when a human
        // actually has something to do.
        std::error_code xec;
        if (!fs::exists(root, xec) && !xec) continue;
        std::cerr << "[WARN] cannot prune kept root '"
                  << root.generic_string() << "': " << rec.message() << "\n";
    }
}

// Nothing writes to the pre-fix shared tree any more, so it is inert — but it
// is also invisible, and it holds a whole corpus run's artifacts. REPORT it
// rather than delete it: a stale build of this runner may be using that exact
// tree right now, and removing it underneath them is the very collision this
// whole section exists to end.
//
// The note EXPIRES, and that is the point of the marker below. A `[NOTE]` on
// EVERY run is not a reminder, it is wallpaper: read once, scrolled past
// forever, and thereafter indistinguishable from the noise it is competing
// with — while the thing it describes is a ONE-TIME manual chore. So say it
// ONCE per scratch base, with the exact command, then be quiet. Exactly one
// condition re-arms it, and it is the one that makes it news again: the legacy
// tree being WRITTEN to since we last spoke, i.e. a pre-fix build still in
// flight. Marker and debris share a temp tree, so a wiped `/tmp` clears both
// together and the note is neither leaked nor lost.
void reportLegacyDebris(fs::path const& base) {
    // Derived from `base`, not from a fresh `temp_directory_path()`: the two
    // are siblings BY CONSTRUCTION (see `scratchBase`), and the no-argument
    // `temp_directory_path()` throws — unwanted on a best-effort path.
    auto const legacy = base.parent_path() / kLegacyBaseName;
    std::error_code lec;
    auto const legacyStamp = fs::last_write_time(legacy, lec);
    if (lec) return;  // absent (the normal case), or unreadable — say nothing

    auto const marker = base / kLegacyNotedName;
    std::error_code mec;
    auto const noted = fs::last_write_time(marker, mec);
    bool const saidBefore = !mec;
    if (saidBefore && noted >= legacyStamp) {
        return;  // already said, and nothing has touched the tree since
    }

#ifdef _WIN32
    constexpr char const* removeCmd = "rmdir /s /q ";
#else
    constexpr char const* removeCmd = "rm -rf ";
#endif
    std::cout << "[NOTE] '" << legacy.generic_string()
              << "' is the pre-fix SHARED scratch tree. ";
    if (saidBefore) {
        // The ONLY way to be here a second time is that the tree was written
        // to since the last note — so the "nothing writes there now" wording
        // below would be flatly untrue, and the advice inverts with it.
        std::cout << "It has been WRITTEN TO since this was last reported, so"
                     " a pre-fix build really is still in flight — leave the"
                     " tree alone until that build is gone, then remove it"
                     " with `" << removeCmd << legacy.generic_string()
                  << "`.\n";
    } else {
        std::cout << "Nothing writes there now — remove it with `" << removeCmd
                  << legacy.generic_string()
                  << "` once no pre-fix build of this runner is in flight.\n"
                     "       (said once per scratch base; it returns only if"
                     " that tree is written to again)\n";
    }

    // Best-effort: if the marker cannot be written the note simply repeats,
    // which is the old behaviour and harmless.
    std::ofstream(marker.string())
        << "Reported the pre-fix shared scratch tree\n  "
        << legacy.generic_string()
        << "\nonce. Delete THIS file to be told about it again.\n";
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: integrated_tests <path-to-dss-code-prime> "
                  << "<path-to-examples-root>\n";
        return 1;
    }

    // D-TEST-INTEGRATED-CORPUS-WALK-THROWS-UNCAUGHT (delta beyond the walk itself): these two sit ABOVE the `try` below, and the no-argument `absolute` throws — it consults `current_path()`, which fails if the cwd was deleted.
    // The corpus root is what this defect is about, so it is resolved with the
    // same discipline as the walk that reads it.
    std::error_code argEc;
    auto const compilerPath = fs::absolute(argv[1], argEc);
    if (argEc) {
        std::cerr << "[ERROR] cannot resolve compiler path '" << argv[1]
                  << "': " << argEc.message() << "\n";
        return 1;
    }
    fs::path const examplesRoot = fs::absolute(argv[2], argEc);
    if (argEc) {
        std::cerr << "[ERROR] cannot resolve examples root '" << argv[2]
                  << "': " << argEc.message() << "\n";
        return 1;
    }
    std::string const compiler = compilerPath.string();

    // Per-run, never fixed — see the section comment above `scratchBase()`.
    fs::path outputBase;
    try {
        outputBase = claimRunRoot(compiler, examplesRoot);
    } catch (std::exception const& e) {
        std::cerr << "[ERROR] scratch root setup failed: " << e.what() << "\n";
        return 1;
    }

    // Housekeeping, deliberately OUTSIDE the fatal `try` above: CLAIMING a root
    // is a precondition of running, tidying up after old ones is not, and the
    // two must not share an exit status. Both calls already report-and-continue
    // on their own (`error_code` overloads throughout — see `pruneKeptRoots`);
    // this catch is the second layer, so that an edit which reintroduces a
    // throwing call — `scratchBase()` itself is one, since the no-argument
    // `temp_directory_path()` throws — costs one [WARN] line rather than the
    // whole suite's verdict.
    try {
        auto const base = scratchBase();
        reportLegacyDebris(base);
        pruneKeptRoots(base, outputBase);
    } catch (std::exception const& e) {
        std::cerr << "[WARN] scratch housekeeping skipped: " << e.what()
                  << " — kept roots may accumulate\n";
    }

    // The chosen root is LOGGED once, here, so a failing run stays
    // inspectable even though the name is no longer predictable.
    std::cout << "=== DSS Code Prime — Integration Tests ===\n"
              << "Compiler:      " << compiler << "\n"
              << "Examples root: " << examplesRoot.generic_string() << "\n"
              << "Output:        " << outputBase.string()
              << "  (per-run, unique to THIS process)\n"
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

    // ── Harness self-test: recursive neighbour staging ──
    //
    // Deliberately UNNUMBERED. The `[Test N]` labels in this file are cited
    // from outside it — `.plans/_deferred-anchor-registry.md` names `[Test 4]`
    // as where the manifest emulator lint lives — so slotting a new number in
    // before the corpus would silently invalidate a registry citation, and
    // appending it at the end would put a HARNESS check after the 581 examples
    // it protects. Neither is worth a number: this tests the runner, not the
    // compiler, and running it here means a broken staging primitive announces
    // itself BEFORE the corpus fails 581 times in its wake.
    //
    // Calls the SAME `stageExampleTreeSelfTest` the in-process sibling drives
    // from `ExamplesCorpusLint.StagesNestedSubdirectoriesWithContentIntact` —
    // and "the same" is now literal rather than enforced: since the AP6 hoist
    // there is ONE definition, in `tests/test_support/stage_tree.hpp`, so the
    // byte-compare lint that used to hold two copies together has nothing left
    // to compare and was deleted with them. A capability exercised in one
    // corpus harness and merely present in the other is the silent harness bug
    // this whole file's ★ notes keep pointing at, so the TEST lands in both,
    // not just the code.
    std::cout << "[Harness self-test] Neighbour staging is recursive\n";
    {
        std::string const findings =
            stageExampleTreeSelfTest(outputBase / "harness-stage-tree");
        check("Neighbour staging carries SUBDIRECTORIES across with their"
              " relative paths and exact bytes",
              findings.empty(), findings);
    }
    std::cout << "\n";

    // ── Test 3: Examples corpus via CLI subprocess ──
    runAllExamples(compiler, examplesRoot, outputBase);

    // ── Test 4: manifest emulator lint (needs the walk's declarations) ──
    runManifestEmulatorLint();

    // ── Test 5: the optimized-arm instrument (needs the walk's counters) ──
    runOptimizedArmInstrument();

    // ── Test 6: the two runners' manifest vocabularies (source-level, host-
    //    independent, needs nothing from the walk) ──
    runRunnerVocabularyPin();

    // D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT: the skip accounting, and it is
    // deliberately IN the Results line rather than only near it. The defect was
    // that `N passed, M failed` was the whole story, so a silently unrun arm
    // read exactly like a verified one. A reader who sees only this line must
    // still be unable to mistake the pass count for a coverage count.
    reportArmVerdicts();
    // `total() - verifiedCount()`, NOT `skippedCount()`: a POISONED arm did not
    // produce a verdict either, and filing it only under "failed" would let the
    // coverage half of this line quietly undercount. Verified-vs-declared is
    // the one subtraction that is exact for every verdict class.
    std::cout << "===========================================\n"
              << "Results: " << passes << " passed, "
              << failures << " failed, "
              << (armLedger.total() - armLedger.verifiedCount()) << " of "
              << armLedger.total()
              << " declared target arms NOT verified (see 'Arm verdicts' above)\n";

    // Cleanup policy. A GREEN run leaves nothing behind. A RED run KEEPS its
    // root, because every `[FAIL]` line above cites a `cli.log` path inside
    // it: the old code removed the tree unconditionally right here, so those
    // paths named files that no longer existed by the time anyone read them.
    // `pruneKeptRoots` at the next run's startup is what stops kept roots
    // accumulating — the retention is bounded, not open-ended.
    std::error_code ec;
    fs::remove(outputBase / kRunningName, ec);  // this run is over either way
    if (failures == 0) {
        fs::remove_all(outputBase, ec);
        if (ec) {
            std::cerr << "[WARN] could not remove scratch root '"
                      << outputBase.generic_string() << "': " << ec.message()
                      << "\n";
        }
    } else {
        std::cout << "Artifacts KEPT for inspection: " << outputBase.string()
                  << "\n(kept only on failure; the " << kKeptRootLimit
                  << " most recent are retained, older ones are pruned at the"
                     " next run's startup)\n";
    }

    return failures > 0 ? 1 : 0;
}
